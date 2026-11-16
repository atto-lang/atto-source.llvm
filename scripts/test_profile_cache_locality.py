import unittest
from profile_cache_locality import EVENTS, parse_perf_stat, rates, validate_event


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

    def test_event_name_admission(self):
        self.assertEqual(validate_event("l2_rqsts.demand_data_rd_miss"),
                         "l2_rqsts.demand_data_rd_miss")
        self.assertRaises(ValueError, validate_event, "invalid;event")
        self.assertRaises(ValueError, validate_event, "cycles")


if __name__ == "__main__":
    unittest.main()
