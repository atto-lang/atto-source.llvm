import tempfile
import unittest
from pathlib import Path

from scripts.check_private_ci_destination import (
    MirrorPolicyError,
    assert_destination_approved,
    normalize_repository,
    read_approved_repositories,
)


class PrivateCIMirrorGateTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.manifest = Path(self.workspace.name) / "approved.txt"

    def allow(self, content):
        self.manifest.write_text(content, encoding="utf-8")

    def test_empty_policy_is_deny_all(self):
        self.allow("# no approved repositories\n")
        with self.assertRaisesRegex(MirrorPolicyError, "not approved"):
            assert_destination_approved("owner/source", "test-org/private", self.manifest)

    def test_exact_destination_after_explicit_allowlisting(self):
        self.allow("# approved after protected review\ntest-org/private\n")
        assert_destination_approved("owner/source", "test-org/private", self.manifest)
        with self.assertRaisesRegex(MirrorPolicyError, "not approved"):
            assert_destination_approved("owner/source", "test-org/public", self.manifest)

    def test_owner_repo_must_be_exact(self):
        for candidate in ("foo/*", " foo/repo", "foo/repo ", "foo/repo/branch", "", "../repo", "a/b\nc/d"):
            with self.subTest(candidate=candidate), self.assertRaises(MirrorPolicyError):
                normalize_repository(candidate)

    def test_duplicate_and_malformed_entries_fail_closed(self):
        for policy in ("owner/repo\nOWNER/REPO\n", "owner/*\n", "owner/repo # inline comment\n"):
            self.allow(policy)
            with self.subTest(policy=policy), self.assertRaises(MirrorPolicyError):
                read_approved_repositories(self.manifest)

    def test_symlinked_manifest_is_denied(self):
        self.allow("test-org/private\n")
        alias = Path(self.workspace.name) / "redirected-approvals.txt"
        try:
            alias.symlink_to(self.manifest)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlinks unsupported in test environment: {exc}")
        with self.assertRaisesRegex(MirrorPolicyError, "regular non-symlink"):
            read_approved_repositories(alias)

    def test_symlink_swap_after_precheck_still_fails_closed(self):
        # Force a path swap precisely after the informational prechecks,
        # but before the authoritative O_NOFOLLOW open.
        import os
        from unittest.mock import patch

        self.allow("test-org/private\n")
        alternate = Path(self.workspace.name) / "forged.txt"
        alternate.write_text("attacker/target\n", encoding="utf-8")
        raw_open = os.open

        def race_swap(path, flags, *args, **kwargs):
            self.manifest.unlink()
            self.manifest.symlink_to(alternate)
            return raw_open(path, flags, *args, **kwargs)

        with patch("scripts.check_private_ci_destination.os.open", side_effect=race_swap):
            with self.assertRaisesRegex(MirrorPolicyError, "cannot read"):
                read_approved_repositories(self.manifest)

    def test_large_or_nonregular_manifest_is_denied(self):
        self.allow("#" + "x" * (16 * 1024))
        with self.assertRaisesRegex(MirrorPolicyError, "exceeds 16 KiB"):
            read_approved_repositories(self.manifest)
        with self.assertRaisesRegex(MirrorPolicyError, "regular non-symlink"):
            read_approved_repositories(Path(self.workspace.name))

    def test_unreadable_policy_is_denied(self):
        with self.assertRaisesRegex(MirrorPolicyError, "cannot read"):
            read_approved_repositories(self.manifest)

    def test_cli_rejects_unreviewed_allowlist_override(self):
        # The operational preflight must always use the protected, anchored
        # repository manifest. The test utility's injected Path must not be
        # confused with a CLI override that an actor could forge.
        import contextlib
        import io
        from scripts.check_private_ci_destination import main

        self.allow("test-org/private\n")
        with contextlib.redirect_stderr(io.StringIO()) as diagnostics:
            with self.assertRaises(SystemExit) as exited:
                main([
                    "--source", "owner/source",
                    "--destination", "test-org/private",
                    "--allowlist", str(self.manifest),
                ])
        self.assertEqual(2, exited.exception.code)
        self.assertIn("unrecognized arguments", diagnostics.getvalue())

    def test_same_organization_target_is_not_a_cross_org_mirror(self):
        self.allow("source-org/private-test\n")
        with self.assertRaisesRegex(MirrorPolicyError, "different organization"):
            assert_destination_approved(
                "source-org/source", "source-org/private-test", self.manifest)

    def test_self_destination_is_denied(self):
        self.allow("owner/source\n")
        with self.assertRaisesRegex(MirrorPolicyError, "different repository"):
            assert_destination_approved("owner/source", "owner/source", self.manifest)


if __name__ == "__main__":
    unittest.main()
