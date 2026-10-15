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

    def test_self_destination_is_denied(self):
        self.allow("owner/source\n")
        with self.assertRaisesRegex(MirrorPolicyError, "different repository"):
            assert_destination_approved("owner/source", "owner/source", self.manifest)


if __name__ == "__main__":
    unittest.main()
