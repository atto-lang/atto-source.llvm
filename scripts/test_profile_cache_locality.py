import unittest
import hashlib
import subprocess
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path
from tempfile import TemporaryDirectory
from profile_cache_locality import EVENTS, parse_perf_stat, rates, validate_event, hash_executable, publish_exclusive, verify_exact_source_tree, require_stable_cpu_mask, profile


class PerfParserTests(unittest.TestCase):
    def setUp(self):
        self.data = {
            "cycles": 10000, "instructions": 25000,
            "L1-dcache-loads": 8000, "L1-dcache-load-misses": 500,
            "LLC-loads": 300, "LLC-load-misses": 30,
            "cpu-migrations": 0, "context-switches": 2,
        }

    def fixture(self, overrides=None):
        data = dict(self.data)
        if overrides:
            data.update(overrides)
        return "# perf stat\n" + "\n".join(
            f"{data[name]};;{name};123456789;100.00;" for name in EVENTS) + "\n"

    def test_rates_and_required_counters(self):
        data = parse_perf_stat(self.fixture())
        self.assertEqual(data, self.data)
        result = rates(data)
        self.assertEqual(result["ipc"], 2.5)
        self.assertEqual(result["l1d_miss_pct"], 6.25)
        self.assertEqual(result["llc_miss_pct"], 10)
        self.assertEqual(result["llc_mpki"], 1.2)
        self.assertNotIn("l2_miss_pct", result)

    def test_reject_missing_duplicate_and_unsupported(self):
        raw = self.fixture()
        self.assertRaisesRegex(ValueError, "duplicate", parse_perf_stat,
                               raw + "1;;cycles;1;100;\n")
        self.assertRaisesRegex(ValueError, "missing", parse_perf_stat,
                               raw.replace("LLC-load-misses", "unknown"))
        self.assertRaisesRegex(ValueError, "unsupported", parse_perf_stat,
                               raw.replace("500;;L1-dcache-load-misses", "<not supported>;;L1-dcache-load-misses"))
        self.assertRaisesRegex(ValueError, "malformed", parse_perf_stat,
                               raw.replace("10000;;cycles", "nan;;cycles"))

    def test_reject_impossible_and_empty_workload(self):
        self.assertRaisesRegex(ValueError, "denominator", parse_perf_stat,
                               self.fixture({"L1-dcache-load-misses": 8001}))
        self.assertRaisesRegex(ValueError, "zero", parse_perf_stat,
                               self.fixture({"instructions": 0}))
        self.assertRaisesRegex(ValueError, "invalid counter", parse_perf_stat,
                               self.fixture({"LLC-load-misses": -1}))

    def test_reject_multiplexing_and_unproven_running_percentage(self):
        raw = self.fixture()
        self.assertRaisesRegex(ValueError, "multiplexed", parse_perf_stat,
                               raw.replace("123456789;100.00", "123456789;80.00", 1))
        self.assertRaisesRegex(ValueError, "percentage", parse_perf_stat,
                               raw.replace("123456789;100.00", "123456789;", 1))

    def test_l2_explicit_not_inferred_from_llc(self):
        requests, misses = "l2_rqsts.all_demand_data_rd", "l2_rqsts.demand_data_rd_miss"
        events = EVENTS + (requests, misses)
        raw = self.fixture() + f"100;;{requests};1;100;\n30;;{misses};1;100;\n"
        counters = parse_perf_stat(raw, events)
        result = rates(counters, (requests, misses))
        self.assertEqual(result["l2_miss_pct"], 30)
        self.assertEqual(result["l2_mpki"], 1.2)
        self.assertRaisesRegex(ValueError, "L2", rates, dict(counters, **{misses: 101}), (requests, misses))

    def test_native_binary_identity_and_mutation_detection(self):
        with TemporaryDirectory() as tmp:
            binary = Path(tmp) / "native_actor_shard_layout_bench"
            binary.write_bytes(b"native-benchmark-v1")
            binary.chmod(0o700)
            expected = hashlib.sha256(b"native-benchmark-v1").hexdigest()
            self.assertEqual(hash_executable(binary), (expected, len(b"native-benchmark-v1")))
            binary.write_bytes(b"native-benchmark-v2")
            changed, size = hash_executable(binary)
            self.assertNotEqual(changed, expected)
            self.assertEqual(size, len(b"native-benchmark-v2"))
            with self.assertRaisesRegex(ValueError, "absolute"):
                hash_executable(Path("relative-binary"))
            binary.chmod(0o600)
            with self.assertRaisesRegex(ValueError, "executable"):
                hash_executable(binary)

    def test_binary_identity_rejects_symlink_and_hardlink_aliases(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "native_actor_shard_layout_bench"
            binary.write_bytes(b"native-benchmark")
            binary.chmod(0o700)
            symlink = root / "benchmark-symlink"
            symlink.symlink_to(binary)
            with self.assertRaisesRegex(ValueError, "not a symlink"):
                hash_executable(symlink)
            hardlink = root / "benchmark-hardlink"
            hardlink.hardlink_to(binary)
            with self.assertRaisesRegex(ValueError, "single-link"):
                hash_executable(binary)
            hardlink.unlink()
            self.assertEqual(hash_executable(binary)[1], len(b"native-benchmark"))

    def test_evidence_publication_never_overwrites_existing_target(self):
        with TemporaryDirectory() as tmp:
            temp = Path(tmp) / ".cache-new.json"
            output = Path(tmp) / "cache.json"
            temp.write_text('{"ok": true}', encoding="utf-8")
            publish_exclusive(temp, output)
            self.assertEqual(output.read_text(), temp.read_text())
            self.assertTrue(temp.exists())
            # Attempting to publish the same evidence twice must fail.
            with self.assertRaisesRegex(RuntimeError, "refusing overwrite"):
                publish_exclusive(temp, output)
            self.assertEqual(output.read_text(), '{"ok": true}')
            output.unlink()
            output.write_text('existing record', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "refusing overwrite"):
                publish_exclusive(temp, output)
            self.assertEqual(output.read_text(), 'existing record')
            output.unlink()
            # exists() would report False on a dangling symlink. An atomic
            # create-if-absent must not replace the symlink directory entry.
            output.symlink_to(Path(tmp) / "absent-target")
            with self.assertRaisesRegex(RuntimeError, "refusing overwrite"):
                publish_exclusive(temp, output)
            self.assertTrue(output.is_symlink())

    def test_exact_tree_preflight_ignores_build_artifacts_not_untracked_source(self):
        sha = "a" * 40
        clean = subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
        changed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="?? src/extra.atto\n", stderr="")
        with patch("profile_cache_locality.current_tree", return_value=sha), \
             patch("profile_cache_locality.subprocess.run", return_value=clean) as run:
            verify_exact_source_tree(sha)
            run.assert_called_once_with(
                ["git", "status", "--porcelain", "--untracked-files=all"],
                capture_output=True, text=True, check=True)
        with patch("profile_cache_locality.current_tree", return_value=sha), \
             patch("profile_cache_locality.subprocess.run", return_value=changed):
            with self.assertRaisesRegex(RuntimeError, "untracked source"):
                verify_exact_source_tree(sha)
        with patch("profile_cache_locality.current_tree", return_value="b" * 40):
            with self.assertRaisesRegex(RuntimeError, "tree"):
                verify_exact_source_tree(sha)
        with self.assertRaisesRegex(ValueError, "40-character"):
            verify_exact_source_tree("not-a-git-tree")

    def test_reject_cpu_mask_drift_during_cache_profile(self):
        # A drifted mask must reject the evidence even if counters look valid.
        with patch("profile_cache_locality.os.sched_getaffinity", return_value={3, 2}):
            require_stable_cpu_mask([2, 3])
        for changed in ({2}, {2, 3, 4}, set()):
            with self.subTest(changed=changed):
                with patch("profile_cache_locality.os.sched_getaffinity", return_value=changed):
                    with self.assertRaisesRegex(RuntimeError, "CPU mask changed"):
                        require_stable_cpu_mask([2, 3])

    def test_cpuset_drift_aborts_full_profile_before_warmup_or_report(self):
        # Exercise profile() integration, not only the isolated mask helper.
        # No real perf invocation or source binary is required for the negative.
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "must-not-exist.json"
            args = SimpleNamespace(repeats=1, warmups=1, tree=None,
                                   command=["/fake/native_actor_shard_layout_bench",
                                            "--layout", "soa", "--rows", "17",
                                            "--iterations", "1"],
                                   output=str(output), perf="perf",
                                   l2_requests=None, l2_misses=None)
            with patch("profile_cache_locality.os.sched_getaffinity",
                       side_effect=({2, 3}, {2})), \
                 patch("profile_cache_locality.hash_executable",
                       return_value=("a" * 64, 123)), \
                 patch("profile_cache_locality.shutil.which", return_value="/usr/bin/perf"), \
                 patch("profile_cache_locality.subprocess.run") as run:
                with self.assertRaisesRegex(RuntimeError, "CPU mask changed"):
                    profile(args)
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_cpuset_drift_after_perf_sample_never_publishes(self):
        # A CPU-mask revocation during perf must reject the whole sample
        # before counters are parsed or a JSON evidence record is created.
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "must-not-exist.json"
            args = SimpleNamespace(repeats=1, warmups=0, tree=None,
                                   command=["/fake/native_actor_shard_layout_bench",
                                            "--layout", "aos", "--rows", "17",
                                            "--iterations", "1"],
                                   output=str(output), perf="perf",
                                   l2_requests=None, l2_misses=None)
            with (patch("profile_cache_locality.os.sched_getaffinity",
                        side_effect=({2, 3}, {2, 3}, {2})),
                  patch("profile_cache_locality.hash_executable",
                        return_value=("a" * 64, 123)),
                  patch("profile_cache_locality.shutil.which", return_value="/usr/bin/perf"),
                  patch("profile_cache_locality.subprocess.run") as run):
                with self.assertRaisesRegex(RuntimeError, "CPU mask changed"):
                    profile(args)
                run.assert_called_once()
            self.assertFalse(output.exists())

    def test_event_name_admission(self):
        self.assertEqual(validate_event("l2_rqsts.demand_data_rd_miss"),
                         "l2_rqsts.demand_data_rd_miss")
        self.assertRaises(ValueError, validate_event, "invalid;event")
        self.assertRaises(ValueError, validate_event, "cycles")


if __name__ == "__main__":
    unittest.main()
