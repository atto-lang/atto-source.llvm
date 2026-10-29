#!/usr/bin/env python3
import importlib.util
import inspect
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,ROOT/path)
    mod=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

oracle=load("authority_oracle",Path("scripts/private_ci_authorization_oracle.py"))
v3=load("llvm_private_ci_v3",Path("scripts/publish-private-ci-source-v3.py"))

class PublisherAuthorityAlignmentTest(unittest.TestCase):
    def test_incidents_and_quarantines_match_canonical_contract(self):
        contract=oracle.load_contract()
        incidents={x["number"]:x["issue_id"] for x in contract["blocking_incidents"]}
        actual={number:issue_id for number,issue_id,_url in v3.INCIDENT_GATES}
        quarantines={x["repository"].casefold():x["repository_id"] for x in contract["quarantined_destinations"]}
        self.assertEqual(actual,incidents)
        self.assertEqual({x.casefold() for x in v3.QUARANTINED_DESTINATIONS},set(quarantines))
        self.assertEqual(set(v3.QUARANTINED_DESTINATION_IDS),set(quarantines.values()))

    def test_real_publisher_keeps_required_runtime_gates(self):
        self.assertEqual(v3.INCIDENT_DISPOSITION_PATH,".github/private-ci-incident-dispositions.json")
        auth=inspect.getsource(v3.authorize_source_write)
        for needle in ("checked_carrier_main","protected_authority"):
            self.assertIn(needle,auth)
        publish=inspect.getsource(v3.publish)
        self.assertGreaterEqual(publish.count("verify_required_source_snapshot"),2)
        self.assertGreaterEqual(publish.count("authorize_source_write"),4)
        protection=inspect.getsource(v3.strict_branch_protection)
        for needle in (
            "require_last_push_approval","app_id","bypass_pull_request_allowances",
            "allow_force_pushes","allow_deletions",
        ):
            self.assertIn(needle,protection)

if __name__=="__main__":
    unittest.main(verbosity=2)
