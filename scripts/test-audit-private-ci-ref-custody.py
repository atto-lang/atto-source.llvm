#!/usr/bin/env python3
"""Source-free mutation tests: publishing must remain disabled by default."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent
AUDIT = ROOT / "audit-private-ci-ref-custody.py"
PUBLISHER = ROOT / "publish-private-ci-source-v3.py"
spec = importlib.util.spec_from_file_location("custody_source_audit", AUDIT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class RefCustodyFailClosedSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = PUBLISHER.read_text(encoding="utf-8")

    def changed(self, old, new):
        self.assertEqual(self.source.count(old), 1,
                         "mutation must target exactly one production construct")
        return self.source.replace(old, new, 1)

    def denied(self, source):
        with self.assertRaises(audit.CustodyContractError):
            audit.audit_source(source)

    def test_real_publisher_is_unconditionally_disabled(self):
        self.assertTrue(audit.audit_source(self.source))

    def test_missing_pre_source_guard_denied(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            "    generation =",
        ))

    def test_guard_replaced_by_success_is_denied(self):
        self.denied(self.changed(
            '    raise Denied(\n'
            '        "private CI export disabled: atomic generation cleanup '
            'is staged but not independently approved (issue #1154)"\n'
            '    )',
            "    return True",
        ))

    def test_guard_cannot_be_swallowed_by_try_except(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            "    try:\n        require_atomic_ref_rollback()\n"
            "    except Denied:\n        pass\n    generation =",
        ))

    def test_pre_guard_private_api_access_denied(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            '    gh_api("repos/secret")\n'
            '    require_atomic_ref_rollback()\n    generation =',
        ))

    def test_early_return_bypassing_guard_denied(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            "    if source_pr == 999:\n        return None\n"
            "    require_atomic_ref_rollback()\n    generation =",
        ))

    def test_unexpected_dynamic_before_guard_denied(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            '    os.system("echo unsafe")\n'
            "    require_atomic_ref_rollback()\n    generation =",
        ))

    def test_rest_name_only_delete_enabled_denied(self):
        self.denied(self.changed(
            '    if method != "GET":',
            '    if method == "GET":',
        ))

    def test_rest_cleanup_transport_reintroduced_denied(self):
        self.denied(self.changed(
            "    # GitHub's REST DELETE reference endpoint has no atomic old-SHA",
            '    _gh_ref_request(repository, ref_name, "DELETE")\n'
            "    # GitHub's REST DELETE reference endpoint has no atomic old-SHA",
        ))

    def test_staged_atomic_push_removed_denied(self):
        self.denied(self.changed(
            '"--atomic", "--porcelain",',
            '"--porcelain",',
        ))

    def test_staged_old_sha_lease_removed_denied(self):
        self.denied(self.source.replace(
            "--force-with-lease=refs/heads/",
            "--not-a-lease=refs/heads/",
        ))

    def test_dynamic_eval_before_guard_denied(self):
        self.denied(self.changed(
            "    require_atomic_ref_rollback()\n    generation =",
            '    eval("0")\n'
            "    require_atomic_ref_rollback()\n    generation =",
        ))

    def test_top_level_guard_rebinding_denied(self):
        self.denied(self.changed(
            'if __name__ == "__main__":',
            'require_atomic_ref_rollback = lambda: None\n'
            'if __name__ == "__main__":',
        ))

    def test_top_level_dynamic_exec_denied(self):
        self.denied(self.changed(
            'if __name__ == "__main__":',
            'exec("require_atomic_ref_rollback = lambda: None")\n'
            'if __name__ == "__main__":',
        ))

    def test_top_level_import_alias_override_denied(self):
        self.denied(self.changed(
            'import argparse\n',
            'import argparse\nimport os as require_atomic_ref_rollback\n',
        ))

    def test_top_level_lowercase_variable_denied(self):
        self.denied(self.changed(
            'if __name__ == "__main__":',
            'publisher_enabled = True\nif __name__ == "__main__":',
        ))

    def test_guard_decorator_denied(self):
        self.denied(self.changed(
            'def require_atomic_ref_rollback():',
            '@staticmethod\ndef require_atomic_ref_rollback():',
        ))

    def test_cli_entry_additional_call_denied(self):
        self.denied(self.changed(
            '    sys.exit(main())',
            '    require_atomic_ref_rollback()\n    sys.exit(main())',
        ))

    def test_cli_entry_changed_from_main_denied(self):
        self.denied(self.changed(
            '    sys.exit(main())',
            '    main()',
        ))

    def test_syntax_error_denied(self):
        self.denied("def publish(:\n")

    def test_nonstring_and_oversize_denied(self):
        self.denied(None)
        self.denied("x" * (512 * 1024 + 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
