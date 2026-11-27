#!/usr/bin/env python3
"""Populate an empty sparse checkout only from raw Git blobs, no filters.

This script MUST be copied from trusted reviewed main before checking out any
untrusted PR. Its starting state is an intentionally empty non-cone sparse
checkout with every index entry marked skip-worktree, and .git as the only
root entry. Reject any already materialized source before touching it.
"""
import os
import stat
import subprocess
import sys
from pathlib import Path
from verify_llvm_exact_checkout import Denied, env, git, index, parts, read, tree

MAX_FILES = 150000
MAX_BLOB = 128 * 1024 * 1024
MAX_TOTAL = 3 * 1024 * 1024 * 1024


def assert_empty_sparse(expected):
    if git("rev-parse", "--verify", "HEAD").strip() != expected:
        raise Denied("source SHA mismatch before materialization")
    if Path(os.fsdecode(git("rev-parse", "--show-toplevel").strip())).resolve() != Path.cwd().resolve():
        raise Denied("not at pinned checkout root")
    if git("config", "--bool", "--get", "core.sparseCheckout").strip() != b"true":
        raise Denied("not an active sparse checkout")
    children = list(os.scandir("."))
    if len(children) != 1 or children[0].name != ".git" or not children[0].is_dir(follow_symlinks=False):
        raise Denied("checkout materialized source before trusted extraction")
    files = tree()
    if not files or len(files) > MAX_FILES or files != index():
        raise Denied("invalid or mismatched tree/index")
    skip = git("ls-files", "-t", "-z").split(b"\0")
    if len(skip) != len(files) + 1 or any(not item.startswith(b"S ") or item[2:] not in files for item in skip if item):
        raise Denied("sparse checkout did not exclude every source path")
    return files


def open_dirs(rootfd, name):
    opened = []
    parent = rootfd
    try:
        for piece in parts(name)[:-1]:
            try:
                os.mkdir(piece, 0o700, dir_fd=parent)
            except FileExistsError:
                pass
            fd = os.open(piece, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            opened.append(fd)
            parent = fd
        return parent, opened
    except BaseException:
        for fd in reversed(opened):
            os.close(fd)
        raise


def materialize(files):
    proc = subprocess.Popen(
        ["git", "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null", "cat-file", "--batch"],
        env=env(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    rootfd = os.open(".", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    total = 0
    try:
        for name, (mode, oid) in sorted(files.items()):
            parent, opened = open_dirs(rootfd, name)
            try:
                proc.stdin.write(oid + b"\n")
                proc.stdin.flush()
                header = proc.stdout.readline().rstrip(b"\n").split(b" ")
                if len(header) != 3 or header[:2] != [oid, b"blob"] or not header[2].isdigit():
                    raise Denied("Git blob response invalid")
                size = int(header[2])
                total += size
                if size > MAX_BLOB or total > MAX_TOTAL:
                    raise Denied("raw object budget exceeded")
                fd = os.open(parts(name)[-1], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600, dir_fd=parent)
                with os.fdopen(fd, "wb", buffering=0) as out:
                    remaining = size
                    while remaining:
                        n = min(1048576, remaining)
                        chunk = read(proc.stdout, n)
                        if out.write(chunk) != len(chunk):
                            raise Denied("short materialized file write")
                        remaining -= n
                    os.fchmod(out.fileno(), 0o755 if mode == b"100755" else 0o644)
                if read(proc.stdout, 1) != b"\n":
                    raise Denied("invalid raw Git blob terminator")
            except OSError as exc:
                raise Denied("unsafe materialized destination") from exc
            finally:
                for fd in reversed(opened):
                    os.close(fd)
    finally:
        os.close(rootfd)
        proc.stdin.close()
        proc.stdout.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if proc.returncode:
            raise Denied("Git raw blob reader failed")


def clear_skip_worktree(files):
    # Only modify index bits; never use checkout, reset --hard, restore, or
    # a command which converts bytes through smudge/clean attributes.
    indexed = git("ls-files", "-z")
    if set(indexed.split(b"\0")) - {b""} != set(files):
        raise Denied("index path set changed")
    run = subprocess.run(["git", "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null",
                          "update-index", "--no-skip-worktree", "-z", "--stdin"],
                         input=indexed, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env())
    if run.returncode:
        raise Denied("could not clear sparse index flags")
    # actions/checkout v4 non-cone mode sets core.sparseCheckout locally;
    # git sparse-checkout set --no-cone uses a separate worktree config.
    extension = subprocess.run(["git", "config", "--local", "--bool", "--get",
                                "extensions.worktreeConfig"], env=env(),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if extension.returncode not in (0, 1):
        raise Denied("could not inspect Git worktree configuration")
    scope = "--worktree" if extension.stdout.strip() == b"true" else "--local"
    result = subprocess.run(["git", "config", scope, "core.sparseCheckout", "false"],
                            env=env(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise Denied("could not disable sparse rules")
    flags = git("ls-files", "-t", "-z").split(b"\0")
    if len(flags) != len(files) + 1 or any(not item.startswith(b"H ") for item in flags if item):
        raise Denied("sparse skip flags remain in index")


def main():
    if len(sys.argv) != 2:
        raise Denied("expected exact SHA")
    expected = os.fsencode(sys.argv[1])
    if not expected or not expected.isalnum() or len(expected) not in (40, 64):
        raise Denied("malformed SHA")
    files = assert_empty_sparse(expected)
    materialize(files)
    clear_skip_worktree(files)
    print("FILTER_FREE_RAW_BLOB_MATERIALIZATION=PASS files=" + str(len(files)))


if __name__ == "__main__":
    try:
        main()
    except (Denied, OSError, ValueError, subprocess.SubprocessError) as exc:
        print("FILTER_FREE_RAW_BLOB_MATERIALIZATION=DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
