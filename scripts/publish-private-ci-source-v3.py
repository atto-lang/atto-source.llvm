#!/usr/bin/env python3
"""Fail-closed exact-tree publisher for private LLVM CI.

Reads only the exact open PR Git tree from the official private source API,
uploads that tree to one reviewed private carrier, emits a v3 receipt, and
creates the carrier's exact-tree branch plus dispatcher authorization ref.
No cross-org checkout, caller-provided API routes, or arbitrary commands.
"""
import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys

SOURCE_REPO = "ores-truffle-oreslang/oreslang-source.llvm"
SOURCE_REPO_ID = 1410897874
SOURCE_BRANCH = "main"
SELF_PATH = "scripts/publish-private-ci-source-v3.py"
POLICY_PATH = ".github/private-ci-approved-repositories.txt"
CONTRACT_PATH = ".github/private-ci-carrier-contracts.json"
PREFIX = "private-oreslang-source"
RECEIPT_PATH = "ci/oreslang-receipt.json"

NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$")
HANDOFF_REF = re.compile(
    r"^ci/(?:oreslang-[0-9a-f]{40}|authorized-[0-9a-f]{40})$"
)
HEX40 = re.compile(r"^[0-9a-f]{40}$")
MAX_FILES = 1024
MAX_TREE_ENTRIES = 4096
MAX_TOTAL = 40 * 1024 * 1024
MAX_FILE = 10 * 1024 * 1024
MAX_POLICY = 64 * 1024
MAX_PLAN = 64 * 1024
TTL = dt.timedelta(hours=24)


class Denied(RuntimeError):
    pass


def valid_sha(value, label="invalid SHA"):
    if not isinstance(value, str) or not HEX40.fullmatch(value):
        raise Denied(label)
    return value


def git_blob_sha(data):
    return hashlib.sha1(
        b"blob " + str(len(data)).encode("ascii") + bytes([0]) + data
    ).hexdigest()


def canonical_bytes(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def canonical_digest(value):
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise Denied("duplicate JSON key")
        out[key] = value
    return out


def gh_api(route, method="GET", payload=None):
    argv = [
        "gh", "api", "--method", method, route,
        "--header", "Accept: application/vnd.github+json",
    ]
    if payload is not None:
        argv.extend(["--input", "-"])
    env = dict(os.environ, GH_HOST="github.com", GH_PROMPT_DISABLED="1")
    try:
        proc = subprocess.run(
            argv,
            input=json.dumps(payload) if payload is not None else None,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            env=env,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Denied("authenticated GitHub API unavailable") from exc
    if proc.returncode != 0:
        # Never echo gh stderr; API failures can include private context.
        raise Denied("authenticated GitHub API request failed")
    try:
        result = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise Denied("invalid GitHub API response") from exc
    if not isinstance(result, dict):
        raise Denied("unexpected GitHub API response shape")
    return result


def decode_contents(item, path, max_bytes):
    if (item.get("path") != path or item.get("type") != "file"
            or item.get("encoding") != "base64"):
        raise Denied("protected file metadata mismatch: " + path)
    encoded = item.get("content")
    if not isinstance(encoded, str):
        raise Denied("protected file content missing: " + path)
    try:
        data = base64.b64decode("".join(encoded.split()), validate=True)
    except (ValueError, base64.binascii.Error) as exc:
        raise Denied("invalid base64 for protected file: " + path) from exc
    if not data or len(data) > max_bytes:
        raise Denied("protected file missing or oversized: " + path)
    return data


def read_local_regular(path, max_bytes):
    p = Path(path)
    try:
        meta = p.lstat()
    except OSError as exc:
        raise Denied("local protected file unavailable: " + path) from exc
    if (p.is_symlink() or not stat.S_ISREG(meta.st_mode)
            or meta.st_nlink != 1 or meta.st_size <= 0 or meta.st_size > max_bytes):
        raise Denied("local protected file unsafe: " + path)
    data = p.read_bytes()
    if len(data) != meta.st_size:
        raise Denied("local protected file changed while reading: " + path)
    return data


def source_protection():
    repo = gh_api(f"repos/{SOURCE_REPO}")
    if (repo.get("id") != SOURCE_REPO_ID
            or repo.get("full_name") != SOURCE_REPO
            or repo.get("private") is not True):
        raise Denied("source repository identity/privacy mismatch")
    branch = gh_api(f"repos/{SOURCE_REPO}/branches/{SOURCE_BRANCH}")
    if (branch.get("name") != SOURCE_BRANCH
            or branch.get("protected") is not True):
        raise Denied("source main is not protected")
    protection = gh_api(f"repos/{SOURCE_REPO}/branches/{SOURCE_BRANCH}/protection")
    reviews = protection.get("required_pull_request_reviews")
    if (not isinstance(reviews, dict)
            or reviews.get("require_code_owner_reviews") is not True
            or reviews.get("dismiss_stale_reviews") is not True
            or not isinstance(reviews.get("required_approving_review_count"), int)
            or isinstance(reviews.get("required_approving_review_count"), bool)
            or reviews.get("required_approving_review_count", 0) < 1):
        raise Denied("source main does not enforce independent CODEOWNER review")
    admins = protection.get("enforce_admins")
    force = protection.get("allow_force_pushes")
    delete = protection.get("allow_deletions")
    if (not isinstance(admins, dict) or admins.get("enabled") is not True
            or not isinstance(force, dict) or force.get("enabled") is not False
            or not isinstance(delete, dict) or delete.get("enabled") is not False):
        raise Denied("source main admin/force/delete protection incomplete")
    bypasses = reviews.get("bypass_pull_request_allowances")
    if bypasses is not None:
        if (not isinstance(bypasses, dict)
                or any(not isinstance(bypasses.get(k, []), list)
                       or bypasses.get(k) for k in ("users", "teams", "apps"))):
            raise Denied("source main has pull-request review bypasses")
    return valid_sha(branch.get("commit", {}).get("sha"),
                     "source protected-main SHA invalid")


def remote_source_file(path, main_sha, max_bytes):
    item = gh_api(f"repos/{SOURCE_REPO}/contents/{path}?ref={main_sha}")
    data = decode_contents(item, path, max_bytes)
    return data, valid_sha(item.get("sha"), "protected file blob SHA invalid")


def parse_allowlist(raw, repository):
    try:
        lines = raw.decode("utf-8").splitlines()
    except UnicodeError as exc:
        raise Denied("allowlist is not UTF-8") from exc
    seen = set()
    allowed = False
    for line in lines:
        if not line or line.startswith("#"):
            continue
        if not NAME.fullmatch(line) or line.casefold() in seen:
            raise Denied("allowlist contains invalid/duplicate entry")
        seen.add(line.casefold())
        if line.casefold() == repository.casefold():
            allowed = True
    if not allowed:
        raise Denied("destination absent from protected allowlist")


def safe_path(value, prefix, suffix):
    if not isinstance(value, str):
        raise Denied("invalid protected carrier path")
    parts = PurePosixPath(value).parts
    if (not value.startswith(prefix) or not value.endswith(suffix)
            or not parts or value.startswith("/") or ".." in parts
            or ".git" in parts or "\\" in value):
        raise Denied("invalid protected carrier path")
    return value


def parse_contract(raw, repository, source_pr, test_plan):
    try:
        doc = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise Denied("invalid carrier contract JSON") from exc
    if not isinstance(doc, dict) or doc.get("schema") != 1:
        raise Denied("carrier contract schema")
    carriers = doc.get("carriers")
    carrier = carriers.get(repository) if isinstance(carriers, dict) else None
    if not isinstance(carrier, dict):
        raise Denied("destination absent from protected carrier contract")
    if set(carrier) != {
        "repository_id", "base_sha", "workflow_path",
        "workflow_blob_sha", "test_plans"
    }:
        raise Denied("carrier contract fields")
    repo_id = carrier.get("repository_id")
    if not isinstance(repo_id, int) or isinstance(repo_id, bool) or repo_id <= 0:
        raise Denied("carrier repository id")
    base_sha = valid_sha(carrier.get("base_sha"), "carrier base SHA")
    workflow_path = safe_path(
        carrier.get("workflow_path"), ".github/workflows/", ".yml"
    )
    workflow_blob = valid_sha(
        carrier.get("workflow_blob_sha"), "carrier workflow blob"
    )
    plans = carrier.get("test_plans")
    plan_path = safe_path(test_plan, "ci/test-plans/", ".json")
    plan = plans.get(plan_path) if isinstance(plans, dict) else None
    if not isinstance(plan, dict) or set(plan) != {"blob_sha", "allowed_prs"}:
        raise Denied("test plan absent from protected carrier contract")
    blob_sha = valid_sha(plan.get("blob_sha"), "test plan blob")
    allowed = plan.get("allowed_prs")
    if (not isinstance(allowed, list) or not allowed
            or len(set(allowed)) != len(allowed)
            or any(not isinstance(x, int) or isinstance(x, bool) or x <= 0
                   for x in allowed)
            or source_pr not in allowed):
        raise Denied("source PR not authorized by protected test plan")
    return {
        "repository_id": repo_id,
        "base_sha": base_sha,
        "workflow_path": workflow_path,
        "workflow_blob_sha": workflow_blob,
        "test_plan_path": plan_path,
        "test_plan_blob_sha": blob_sha,
    }


def protected_authority(repository, source_pr, test_plan):
    """Attest this exact script plus local policies to protected source main."""
    main_sha = source_protection()
    remote_self, remote_self_sha = remote_source_file(
        SELF_PATH, main_sha, 512 * 1024
    )
    local_self = read_local_regular(__file__, 512 * 1024)
    if local_self != remote_self or git_blob_sha(local_self) != remote_self_sha:
        raise Denied("publisher bytes differ from protected source main")

    remote_allow, _ = remote_source_file(POLICY_PATH, main_sha, MAX_POLICY)
    local_allow = read_local_regular(POLICY_PATH, MAX_POLICY)
    if local_allow != remote_allow:
        raise Denied("local allowlist differs from protected source main")
    parse_allowlist(remote_allow, repository)

    remote_contract, _ = remote_source_file(CONTRACT_PATH, main_sha, MAX_POLICY)
    local_contract = read_local_regular(CONTRACT_PATH, MAX_POLICY)
    if local_contract != remote_contract:
        raise Denied("local carrier contract differs from protected source main")
    contract = parse_contract(
        remote_contract, repository, source_pr, test_plan
    )
    return main_sha, contract


def checked_carrier_main(repository, expected_sha):
    branch = gh_api(f"repos/{repository}/branches/main")
    if (branch.get("name") != "main"
            or branch.get("protected") is not True):
        raise Denied("carrier main is not protected")
    base_sha = valid_sha(
        branch.get("commit", {}).get("sha"), "carrier main SHA"
    )
    if base_sha != expected_sha:
        raise Denied("carrier main advanced beyond reviewed base")

    protection = gh_api(f"repos/{repository}/branches/main/protection")
    reviews = protection.get("required_pull_request_reviews")
    if (not isinstance(reviews, dict)
            or reviews.get("require_code_owner_reviews") is not True
            or reviews.get("dismiss_stale_reviews") is not True
            or not isinstance(reviews.get("required_approving_review_count"), int)
            or isinstance(reviews.get("required_approving_review_count"), bool)
            or reviews.get("required_approving_review_count", 0) < 1):
        raise Denied("carrier main does not enforce independent CODEOWNER review")
    admins = protection.get("enforce_admins")
    force = protection.get("allow_force_pushes")
    delete = protection.get("allow_deletions")
    if (not isinstance(admins, dict) or admins.get("enabled") is not True
            or not isinstance(force, dict) or force.get("enabled") is not False
            or not isinstance(delete, dict) or delete.get("enabled") is not False):
        raise Denied("carrier main admin/force/delete protection incomplete")
    bypasses = reviews.get("bypass_pull_request_allowances")
    if bypasses is not None:
        if (not isinstance(bypasses, dict)
                or any(not isinstance(bypasses.get(k, []), list)
                       or bypasses.get(k) for k in ("users", "teams", "apps"))):
            raise Denied("carrier main has pull-request review bypasses")
    return base_sha


def checked_destination(repository, contract):
    repo = gh_api(f"repos/{repository}")
    if (repo.get("id") != contract["repository_id"]
            or repo.get("full_name") != repository
            or repo.get("private") is not True
            or repo.get("visibility") != "private"
            or repo.get("default_branch") != "main"
            or repo.get("archived") is not False
            or repo.get("disabled") is not False
            or repo.get("fork") is not False):
        raise Denied("carrier identity/state differs from protected contract")
    base_sha = checked_carrier_main(repository, contract["base_sha"])
    commit = gh_api(f"repos/{repository}/git/commits/{base_sha}")
    base_tree = valid_sha(commit.get("tree", {}).get("sha"), "carrier base tree")
    listing = gh_api(f"repos/{repository}/git/trees/{base_tree}?recursive=1")
    if (listing.get("sha") != base_tree
            or listing.get("truncated") is not False
            or not isinstance(listing.get("tree"), list)):
        raise Denied("carrier base tree listing incomplete")
    for entry in listing["tree"]:
        path = entry.get("path") if isinstance(entry, dict) else None
        if (path == PREFIX
                or isinstance(path, str) and path.startswith(PREFIX + "/")
                or path == RECEIPT_PATH):
            raise Denied("reviewed carrier base is not source-free")

    workflow = gh_api(
        f"repos/{repository}/contents/{contract['workflow_path']}?ref={base_sha}"
    )
    if (workflow.get("path") != contract["workflow_path"]
            or workflow.get("type") != "file"
            or workflow.get("sha") != contract["workflow_blob_sha"]):
        raise Denied("carrier workflow differs from protected contract")

    plan = gh_api(
        f"repos/{repository}/contents/{contract['test_plan_path']}?ref={base_sha}"
    )
    if (plan.get("path") != contract["test_plan_path"]
            or plan.get("type") != "file"
            or plan.get("encoding") != "base64"
            or plan.get("sha") != contract["test_plan_blob_sha"]):
        raise Denied("carrier test plan differs from protected contract")
    plan_bytes = decode_contents(plan, contract["test_plan_path"], MAX_PLAN)
    try:
        plan_doc = json.loads(
            plan_bytes.decode("utf-8"), object_pairs_hook=unique_object
        )
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise Denied("invalid reviewed carrier test plan") from exc
    if (not isinstance(plan_doc, dict)
            or plan_doc.get("schema") != "ores-private-ci-test-plan-v1"
            or plan_doc.get("source_repo") != SOURCE_REPO):
        raise Denied("carrier test plan source/schema mismatch")
    return base_tree, plan_bytes


def verify_source_pr(source_pr, source_commit):
    pr = gh_api(f"repos/{SOURCE_REPO}/pulls/{source_pr}")
    head = pr.get("head")
    base = pr.get("base")
    if (pr.get("number") != source_pr
            or pr.get("state") != "open"
            or not isinstance(head, dict)
            or head.get("sha") != source_commit
            or not isinstance(head.get("repo"), dict)
            or head["repo"].get("full_name") != SOURCE_REPO
            or not isinstance(base, dict)
            or base.get("ref") != SOURCE_BRANCH):
        raise Denied("source commit is not exact open PR head against main")
    return SOURCE_REPO_ID


def exact_source_tree(source_pr, source_commit):
    verify_source_pr(source_pr, source_commit)

    commit = gh_api(f"repos/{SOURCE_REPO}/git/commits/{source_commit}")
    if commit.get("sha") != source_commit:
        raise Denied("source commit identity mismatch")
    root_tree = valid_sha(commit.get("tree", {}).get("sha"), "source root tree")
    tree = gh_api(f"repos/{SOURCE_REPO}/git/trees/{root_tree}?recursive=1")
    entries = tree.get("tree")
    if (tree.get("sha") != root_tree or tree.get("truncated") is not False
            or not isinstance(entries, list) or len(entries) > MAX_TREE_ENTRIES):
        raise Denied("source tree incomplete or exceeds safety limit")

    # Validate all paths, modes, SHAs and declared blob sizes before fetching
    # any blob bytes. GitHub's tree metadata gives us enough information to
    # enforce the mirror budget without first allocating a potentially huge
    # base64 blob response.
    declared = []
    declared_total = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise Denied("malformed source tree entry")
        kind = entry.get("type")
        if kind == "tree":
            continue
        mode = entry.get("mode")
        path = entry.get("path")
        if kind != "blob" or mode not in ("100644", "100755"):
            raise Denied("source tree contains symlink/submodule/unsupported mode")
        if not isinstance(path, str) or not path:
            raise Denied("source tree path invalid")
        parts = PurePosixPath(path).parts
        if (path.startswith("/") or ".." in parts or ".git" in parts
                or "\\" in path or any(ord(c) < 32 or ord(c) == 127 for c in path)):
            raise Denied("source tree path unsafe")
        sha = valid_sha(entry.get("sha"), "source blob SHA")
        size = entry.get("size")
        if (not isinstance(size, int) or isinstance(size, bool)
                or size < 0 or size > MAX_FILE):
            raise Denied("source file exceeds mirror limit")
        declared_total += size
        if declared_total > MAX_TOTAL or len(declared) >= MAX_FILES:
            raise Denied("source mirror exceeds bounded capacity")
        declared.append((path, mode, sha, size))

    if not declared:
        raise Denied("source tree is empty")

    files = []
    for path, mode, sha, declared_size in declared:
        blob = gh_api(f"repos/{SOURCE_REPO}/git/blobs/{sha}")
        if (blob.get("sha") != sha or blob.get("encoding") != "base64"):
            raise Denied("source blob metadata mismatch")
        encoded = blob.get("content")
        if not isinstance(encoded, str):
            raise Denied("source blob content missing")
        try:
            data = base64.b64decode("".join(encoded.split()), validate=True)
        except (ValueError, base64.binascii.Error) as exc:
            raise Denied("source blob base64 invalid") from exc
        if len(data) != declared_size:
            raise Denied("source blob size differs from tree metadata")
        if git_blob_sha(data) != sha:
            raise Denied("source blob bytes do not match Git SHA")
        files.append((path, mode, sha, data))

    files.sort(key=lambda item: item[0])
    rows = [[path, mode, sha, len(data)] for path, mode, sha, data in files]
    return root_tree, files, rows, hashlib.sha256(canonical_bytes(rows)).hexdigest()


def recheck_destination_identity(repository, contract):
    repo = gh_api(f"repos/{repository}")
    if (repo.get("id") != contract["repository_id"]
            or repo.get("full_name") != repository
            or repo.get("private") is not True
            or repo.get("visibility") != "private"):
        raise Denied("carrier identity/privacy changed during transfer")


def _gh_ref_request(repository, ref_name, method):
    if not isinstance(ref_name, str) or not HANDOFF_REF.fullmatch(ref_name):
        raise Denied("ref cleanup target is invalid")
    if method not in ("GET", "DELETE"):
        raise Denied("ref cleanup method is invalid")
    encoded = ref_name.replace("/", "%2F")
    family = "ref" if method == "GET" else "refs"
    argv = [
        "gh", "api", "--method", method,
        f"repos/{repository}/git/{family}/heads/{encoded}",
        "--header", "Accept: application/vnd.github+json",
    ]
    env = dict(os.environ, GH_HOST="github.com", GH_PROMPT_DISABLED="1")
    try:
        return subprocess.run(
            argv, capture_output=True, text=True, env=env,
            timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Denied("carrier ref rollback is unavailable") from exc


def live_ref_sha(repository, ref_name, expected_repository_id):
    repo = gh_api(f"repos/{repository}")
    if (repo.get("id") != expected_repository_id
            or repo.get("full_name") != repository
            or repo.get("private") is not True
            or repo.get("visibility") != "private"
            or repo.get("archived") is not False
            or repo.get("disabled") is not False):
        raise Denied("carrier repository identity changed during ref verification")
    result = _gh_ref_request(repository, ref_name, "GET")
    if result.returncode != 0:
        if "HTTP 404" in result.stderr:
            return None
        raise Denied("carrier ref lookup failed")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise Denied("carrier ref lookup returned invalid JSON") from exc
    if (not isinstance(payload, dict)
            or payload.get("ref") != "refs/heads/" + ref_name
            or payload.get("object", {}).get("type") != "commit"):
        raise Denied("carrier ref lookup returned unexpected identity")
    return valid_sha(
        payload.get("object", {}).get("sha"),
        "carrier ref lookup returned invalid SHA",
    )


def require_exact_ref(repository, ref_name, expected_sha, expected_repository_id):
    actual = live_ref_sha(repository, ref_name, expected_repository_id)
    if actual != expected_sha:
        raise Denied("carrier ref does not point to exact handoff commit")


def delete_exact_ref(repository, ref_name, expected_sha, expected_repository_id):
    expected_sha = valid_sha(expected_sha, "invalid rollback commit SHA")
    actual = live_ref_sha(repository, ref_name, expected_repository_id)
    if actual is None:
        return False
    if actual != expected_sha:
        raise Denied("carrier ref moved; refusing rollback")
    result = _gh_ref_request(repository, ref_name, "DELETE")
    if result.returncode != 0 and "HTTP 404" not in result.stderr:
        raise Denied("carrier ref rollback failed")
    if live_ref_sha(repository, ref_name, expected_repository_id) is not None:
        raise Denied("carrier ref still exists after rollback")
    return True


def publish(repository, source_pr, source_commit, test_plan, now=None):
    if not NAME.fullmatch(repository):
        raise Denied("invalid destination repository")
    if not isinstance(source_pr, int) or isinstance(source_pr, bool) or source_pr <= 0:
        raise Denied("invalid source PR number")
    source_commit = valid_sha(source_commit, "invalid source commit")

    protected_sha, contract = protected_authority(
        repository, source_pr, test_plan
    )
    base_tree, plan_bytes = checked_destination(repository, contract)
    source_tree, files, rows, manifest_sha256 = exact_source_tree(
        source_pr, source_commit
    )
    branch = "ci/oreslang-" + source_tree
    plan_sha256 = hashlib.sha256(plan_bytes).hexdigest()

    elements = []
    for path, mode, sha, data in files:
        recheck_destination_identity(repository, contract)
        result = gh_api(
            f"repos/{repository}/git/blobs",
            method="POST",
            payload={
                "content": base64.b64encode(data).decode("ascii"),
                "encoding": "base64",
            },
        )
        uploaded = valid_sha(result.get("sha"), "uploaded blob SHA")
        if uploaded != sha:
            raise Denied("uploaded blob differs from exact source blob")
        elements.append({
            "path": path, "mode": mode, "type": "blob", "sha": uploaded
        })

    recheck_destination_identity(repository, contract)
    isolated = gh_api(
        f"repos/{repository}/git/trees",
        method="POST",
        payload={"tree": elements},
    )
    isolated_sha = valid_sha(isolated.get("sha"), "uploaded source tree SHA")
    if isolated_sha != source_tree:
        raise Denied("uploaded source tree differs from source PR root tree")

    now = now or dt.datetime.now(dt.timezone.utc)
    if now.tzinfo is None or now.utcoffset() is None:
        raise Denied("publisher clock must be timezone-aware")
    expires = (now.astimezone(dt.timezone.utc) + TTL).replace(microsecond=0)
    receipt = {
        "schema": "ores-private-ci-receipt-v3",
        "expires_at": expires.isoformat().replace("+00:00", "Z"),
        "carrier": {"workflow_blob_sha": contract["workflow_blob_sha"]},
        "destination": {
            "repo": repository,
            "repo_id": contract["repository_id"],
            "base_sha": contract["base_sha"],
        },
        "mirror": {"root_tree_sha": source_tree},
        "source": {
            "repo": SOURCE_REPO,
            "repo_id": SOURCE_REPO_ID,
            "pr": source_pr,
            "head_sha": source_commit,
            "root_tree_sha": source_tree,
            "manifest_sha256": manifest_sha256,
        },
        "test_plan": {
            "path": contract["test_plan_path"],
            "sha256": plan_sha256,
        },
    }
    receipt_bytes = canonical_bytes(receipt) + b"\n"
    receipt_digest = canonical_digest(receipt)

    recheck_destination_identity(repository, contract)
    rb = gh_api(
        f"repos/{repository}/git/blobs",
        method="POST",
        payload={
            "content": base64.b64encode(receipt_bytes).decode("ascii"),
            "encoding": "base64",
        },
    )
    receipt_blob = valid_sha(rb.get("sha"), "receipt blob SHA")
    if receipt_blob != git_blob_sha(receipt_bytes):
        raise Denied("receipt blob digest mismatch")

    recheck_destination_identity(repository, contract)
    root = gh_api(
        f"repos/{repository}/git/trees",
        method="POST",
        payload={
            "base_tree": base_tree,
            "tree": [
                {"path": PREFIX, "mode": "040000", "type": "tree", "sha": source_tree},
                {"path": RECEIPT_PATH, "mode": "100644", "type": "blob", "sha": receipt_blob},
            ],
        },
    )
    root_sha = valid_sha(root.get("sha"), "carrier root tree SHA")

    recheck_destination_identity(repository, contract)
    commit = gh_api(
        f"repos/{repository}/git/commits",
        method="POST",
        payload={
            "message": (
                "ci: exact private LLVM PR tree\n\n"
                "Oreslang-CI-Receipt-SHA256: " + receipt_digest
            ),
            "tree": root_sha,
            "parents": [contract["base_sha"]],
        },
    )
    carrier_commit = valid_sha(commit.get("sha"), "carrier commit SHA")

    # Full protected-source + immutable-carrier recheck before executable refs.
    again_sha, again_contract = protected_authority(
        repository, source_pr, test_plan
    )
    if again_sha != protected_sha or again_contract != contract:
        raise Denied("protected source authority changed during transfer")
    checked_destination(repository, contract)
    verify_source_pr(source_pr, source_commit)

    # Create the inert source-bearing ref first. The carrier workflow refuses
    # to execute without the exact authorization ref. Treat every POST as
    # potentially successful before it returns so transport ambiguity cannot
    # strand private source indefinitely.
    authorization = "ci/authorized-" + carrier_commit
    payload_attempted = False
    authorization_attempted = False
    try:
        payload_attempted = True
        branch_ref = gh_api(
            f"repos/{repository}/git/refs",
            method="POST",
            payload={
                "ref": "refs/heads/" + branch,
                "sha": carrier_commit,
            },
        )
        if (branch_ref.get("ref") != "refs/heads/" + branch
                or branch_ref.get("object", {}).get("sha")
                   not in (None, carrier_commit)):
            raise Denied("carrier did not confirm exact-tree branch")
        require_exact_ref(
            repository, branch, carrier_commit, contract["repository_id"]
        )

        # Re-run mutable governance checks while the payload is still inert.
        again_sha, again_contract = protected_authority(
            repository, source_pr, test_plan
        )
        if again_sha != protected_sha or again_contract != contract:
            raise Denied("protected source authority changed before authorization")
        checked_destination(repository, contract)

        # The exact open PR head is the last source-side fact checked before
        # the carrier becomes executable.
        verify_source_pr(source_pr, source_commit)

        authorization_attempted = True
        auth_ref = gh_api(
            f"repos/{repository}/git/refs",
            method="POST",
            payload={
                "ref": "refs/heads/" + authorization,
                "sha": carrier_commit,
            },
        )
        if (auth_ref.get("ref") != "refs/heads/" + authorization
                or auth_ref.get("object", {}).get("sha")
                   not in (None, carrier_commit)):
            raise Denied("carrier did not confirm dispatcher authorization ref")

        # Never trust create responses alone. Re-read both refs before success.
        require_exact_ref(
            repository, branch, carrier_commit, contract["repository_id"]
        )
        require_exact_ref(
            repository, authorization, carrier_commit, contract["repository_id"]
        )
    except Denied as exc:
        rollback_error = None
        if authorization_attempted:
            try:
                delete_exact_ref(
                    repository, authorization, carrier_commit,
                    contract["repository_id"],
                )
            except Denied as rollback_exc:
                rollback_error = rollback_exc
        if payload_attempted:
            try:
                delete_exact_ref(
                    repository, branch, carrier_commit,
                    contract["repository_id"],
                )
            except Denied as rollback_exc:
                rollback_error = rollback_error or rollback_exc
        if rollback_error is not None:
            raise Denied(
                "private CI handoff failed and carrier ref rollback was incomplete"
            ) from rollback_error
        raise exc

    return {
        "commit": carrier_commit,
        "branch": branch,
        "authorization_ref": authorization,
        "source_tree": source_tree,
        "manifest_sha256": manifest_sha256,
        "receipt_sha256": receipt_digest,
        "files": len(files),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository")
    parser.add_argument("--source-pr", required=True, type=int)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--test-plan", required=True)
    args = parser.parse_args(argv)
    try:
        result = publish(
            args.repository,
            args.source_pr,
            args.source_commit,
            args.test_plan,
        )
    except (Denied, OSError, ValueError) as exc:
        print("PRIVATE_CI_DESTINATION_REJECTED: " + str(exc), file=sys.stderr)
        return 1
    print(
        "PRIVATE_CI_V3_CREATED: "
        + " ".join(f"{key}={value}" for key, value in result.items())
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
