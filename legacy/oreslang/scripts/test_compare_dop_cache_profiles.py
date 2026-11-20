import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from compare_dop_cache_profiles import MAX_PROFILE_BYTES, compare
from profile_cache_locality import EVENTS, rates


class ThreeLayoutComparisonTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.paths = {}
        counters = dict.fromkeys(EVENTS, 0)
        counters.update({"cycles": 10000, "instructions": 25000,
                         "L1-dcache-loads": 8000, "L1-dcache-load-misses": 500,
                         "LLC-loads": 300, "LLC-load-misses": 30})
        measured = rates(counters)
        self.reports = {}
        for layout in ("aos", "soa", "aosoa"):
            self.reports[layout] = {
                "format": "oreslang-cpu-cache-v1", "source_tree": "a" * 40,
                "binary_sha256": "b" * 64, "binary_size_bytes": 192345,
                "command": ["/build/native_actor_shard_layout_bench", "--layout", layout,
                            "--rows", "100000", "--iterations", "200"],
                "events": list(EVENTS), "hardware_l2_events": [],
                "repeats": 2, "warmups": 1,
                "host": {"machine": "x86_64", "kernel": "6.9",
                         "allowed_cpus": [2, 3]},
                "samples": [{"counters": dict(counters), "rates": dict(measured)} for _ in range(2)],
                "median_rates": dict(measured),
            }
            self.paths[layout] = Path(self.folder.name) / (layout + ".json")
        self.flush()

    def flush(self):
        for k in self.paths:
            self.paths[k].write_text(json.dumps(self.reports[k]), encoding="utf-8")

    def test_accepts_cmake_built_benchmark_and_rejects_unrelated_executable(self):
        # CMake builds oreslang-native-actor-shard-bench, not the source file
        # basename native_actor_shard_layout_bench. Real build outputs must
        # pass the same exact-binary comparison contract as legacy paths.
        canonical = "/build/oreslang-native-actor-shard-bench"
        for layout in ("aos", "soa", "aosoa"):
            self.reports[layout]["command"][0] = canonical
        self.flush()
        self.assertEqual(compare(self.paths)["workload_except_layout"][0], canonical)

        # A different executable remains prohibited, even if all three
        # reports use the same SHA and declare the same workload.
        for layout in ("aos", "soa", "aosoa"):
            self.reports[layout]["command"][0] = "/build/unrelated-benchmark"
        self.flush()
        with self.assertRaisesRegex(ValueError, "benchmark"):
            compare(self.paths)

    def test_comparable_profiles(self):
        result = compare(self.paths)
        self.assertEqual(result["median_rates"]["aos"]["l1d_mpki"], 20)
        self.assertEqual(result["binary_sha256"], "b" * 64)
        self.assertEqual(result["relative_to_aos"]["soa"]["llc_mpki"], 1)

    def test_wrong_or_duplicate_layout_rejected(self):
        for command in (["--layout", "aos", "--layout", "soa"],
                        ["--layout", "aos"], ["--layout=oops"]):
            with self.subTest(command=command):
                prior = self.reports["soa"]["command"]
                self.reports["soa"]["command"] = [prior[0], *command,
                                                     "--rows", "100000", "--iterations", "200"]
                self.flush()
                with self.assertRaisesRegex(ValueError, "layout"):
                    compare(self.paths)
                self.reports["soa"]["command"] = prior

    def test_reject_mismatched_workload_tree_host_and_events(self):
        for field, value in (("source_tree", "b" * 40),
                             ("binary_sha256", "c" * 64),
                             ("binary_size_bytes", 42),
                             ("host", {"machine": "x86_64", "kernel": "7.0", "allowed_cpus": [2, 3]}),
                             ("events", list(reversed(EVENTS))),
                             ("warmups", 2)):
            with self.subTest(field=field):
                original = copy.deepcopy(self.reports["soa"][field])
                self.reports["soa"][field] = value
                self.flush()
                with self.assertRaises(ValueError):
                    compare(self.paths)
                self.reports["soa"][field] = original
        self.reports["soa"]["command"][-1] = "300"
        self.flush()
        with self.assertRaisesRegex(ValueError, "workload"):
            compare(self.paths)

    def test_forged_medians_incomplete_samples_and_nonfinite_rejected(self):
        for mutation in (lambda x: x["median_rates"].update({"ipc": 900}),
                         lambda x: x["samples"].pop(),
                         lambda x: x["samples"][0]["counters"].update({"cycles": -1}),
                         lambda x: x["samples"][0]["rates"].update({"ipc": float("nan")})):
            original = copy.deepcopy(self.reports["soa"])
            mutation(self.reports["soa"])
            self.flush()
            with self.assertRaises(ValueError):
                compare(self.paths)
            self.reports["soa"] = original

    def test_missing_and_invalid_binary_identity_rejected(self):
        for bad in (None, "f" * 63, "INVALID", "f" * 65):
            with self.subTest(bad=bad):
                self.reports["aosoa"]["binary_sha256"] = bad
                self.flush()
                with self.assertRaisesRegex(ValueError, "binary SHA-256"):
                    compare(self.paths)
        self.reports["aosoa"]["binary_sha256"] = "b" * 64
        self.reports["aosoa"]["binary_size_bytes"] = 0
        self.flush()
        with self.assertRaisesRegex(ValueError, "binary size"):
            compare(self.paths)

    def test_comparator_matches_native_benchmark_cli(self):
        original = copy.deepcopy(self.reports["soa"]["command"])
        invalid = [
            (["--layout=soa", "--rows", "100000", "--iterations", "200"], "layout"),
            (["--layout", "soa", "--iterations", "200", "--rows", "100000"], "workload"),
            (["--layout", "soa", "--rows", "100000", "--iterations", "200", "--extra"], "workload"),
            (["--layout", "soa", "--rows", "100001", "--iterations", "200"], "rows"),
            (["--layout", "soa", "--rows", "100000", "--iterations", "100001"], "iterations"),
            (["--layout", "soa", "--rows", "100000", "--iterations", "501"], "50 million"),
            (["--layout", "soa", "--rows", "0", "--iterations", "200"], "rows"),
        ]
        for flags, message in invalid:
            with self.subTest(flags=flags):
                self.reports["soa"]["command"] = [original[0], *flags]
                self.flush()
                with self.assertRaisesRegex(ValueError, message):
                    compare(self.paths)
        self.reports["soa"]["command"] = original
        self.flush()
        self.assertEqual(compare(self.paths)["source_tree"], "a" * 40)

    def test_sample_limits_match_profiler_contract(self):
        # The producer accepts 1..30 repeats and 0..20 warmups.
        original = copy.deepcopy(self.reports["soa"])
        invalid = [
            ("repeats", True),
            ("warmups", True),
            ("warmups", 21),
        ]
        for key, value in invalid:
            with self.subTest(field=key, value=value):
                self.reports["soa"] = copy.deepcopy(original)
                self.reports["soa"][key] = value
                self.flush()
                with self.assertRaisesRegex(ValueError, "perf samples"):
                    compare(self.paths)
        for count in (0, 31):
            with self.subTest(sample_count=count):
                self.reports["soa"] = copy.deepcopy(original)
                self.reports["soa"]["samples"] = [
                    copy.deepcopy(original["samples"][0]) for _ in range(count)
                ]
                self.reports["soa"]["repeats"] = count
                self.flush()
                with self.assertRaisesRegex(ValueError, "perf samples"):
                    compare(self.paths)
        self.reports["soa"] = original
        self.flush()
        self.assertIn("soa", compare(self.paths)["median_rates"])

    def test_duplicate_json_keys_rejected_at_every_depth(self):
        # json.loads normally accepts duplicate object keys and keeps the
        # last one, masking attacker-supplied conflicting cache evidence.
        path = self.paths["soa"]
        raw = path.read_text(encoding="utf-8")
        for original, replacement in (
                ('"source_tree": "', '"source_tree": "c", "source_tree": "'),
                ('"machine": "x86_64"', '"machine": "spoof", "machine": "x86_64"'),
                ('"cycles": 10000', '"cycles": 1, "cycles": 10000')):
            with self.subTest(replacement=replacement):
                self.assertIn(original, raw)
                path.write_text(raw.replace(original, replacement, 1), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                    compare(self.paths)
        path.write_text(raw, encoding="utf-8")

    def test_bounded_file_and_nonregular_inputs(self):
        path = self.paths["soa"]
        path.write_bytes(b"x" * (MAX_PROFILE_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "evidence limit"):
            compare(self.paths)
        path.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty"):
            compare(self.paths)
        if hasattr(os, "O_NOFOLLOW"):
            path.unlink()
            path.symlink_to(self.paths["aos"])
            with self.assertRaises(OSError):
                compare(self.paths)
        if hasattr(os, "mkfifo") and hasattr(os, "O_NONBLOCK"):
            path.unlink()
            os.mkfifo(path)
            with self.assertRaisesRegex(ValueError, "regular file"):
                compare(self.paths)

    def test_hardlinked_evidence_is_rejected_then_recovers(self):
        # Both paths may be regular JSON files with byte-identical contents.
        # A second hard link must still invalidate the evidence descriptor.
        source = self.paths["soa"]
        alias = Path(self.folder.name) / "hardlinked-evidence.json"
        os.link(source, alias)
        try:
            with self.assertRaisesRegex(ValueError, "single-link regular file"):
                compare(self.paths)
        finally:
            alias.unlink()
        # Normal single-link evidence must remain usable after unlink.
        self.assertIn("soa", compare(self.paths)["median_rates"])

    def test_invalid_l2_event_syntax_and_duplicate_pair_rejected(self):
        for events in (["bad;event", "l2_miss"],
                       ["cycles", "l2_miss"],
                       ["l2_requests", "l2_requests"]):
            with self.subTest(events=events):
                self.reports["aosoa"]["hardware_l2_events"] = events
                self.reports["aosoa"]["events"] = list(EVENTS) + events
                self.flush()
                with self.assertRaisesRegex(ValueError, "L2"):
                    compare(self.paths)

    def test_zero_source_tree_and_missing_l2_event_not_accepted(self):
        self.reports["aosoa"]["source_tree"] = None
        self.flush()
        with self.assertRaisesRegex(ValueError, "source-tree"):
            compare(self.paths)
        self.reports["aosoa"]["source_tree"] = "a" * 40
        self.reports["aosoa"]["hardware_l2_events"] = ["l2_requests"]
        self.flush()
        with self.assertRaisesRegex(ValueError, "L2"):
            compare(self.paths)


if __name__ == "__main__":
    unittest.main()
