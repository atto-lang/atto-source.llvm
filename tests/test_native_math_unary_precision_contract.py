import hashlib
import json
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contracts/v1/native-math-unary-precision-v1.json"
EXPECTED_BLOB = "3249d837ae6bcf6e2c2c23cac872becf92d0c8e0"
MIN = -(1 << 63)
MAX = (1 << 63) - 1


def git_blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


class NativeMathUnaryPrecisionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = CONTRACT.read_bytes()
        assert git_blob(raw) == EXPECTED_BLOB
        cls.contract = json.loads(raw)

    def test_contract_identity_and_unique_ids(self):
        self.assertEqual("oreslang.native-math-unary-precision.v1",
                         self.contract["contract"])
        groups = ("signed_i64_unary", "bigint_unary", "decimal_unary",
                  "binary64_unary", "precision_cases")
        ids = [case["id"] for group in groups for case in self.contract[group]]
        self.assertEqual(len(ids), len(set(ids)))

    def test_checked_signed_i64_reference(self):
        for case in self.contract["signed_i64_unary"]:
            value = int(case["operand"])
            self.assertGreaterEqual(value, MIN)
            self.assertLessEqual(value, MAX)
            if value == MIN:
                actual = {"error": "OVERFLOW"}
            elif case["operation"] == "Math.abs":
                actual = {"value": str(abs(value))}
            else:
                actual = {"value": str(-value)}
            self.assertEqual(case["expect"], actual, case["id"])

    def test_binary64_sign_reference_bits(self):
        for case in self.contract["binary64_unary"]:
            value = struct.unpack(">d", bytes.fromhex(case["operand_bits"]))[0]
            actual = abs(value) if case["operation"] == "Math.abs" else -value
            self.assertEqual(case["expect_bits"], struct.pack(">d", actual).hex(),
                             case["id"])

    def test_bigint_reference_never_narrows(self):
        for case in self.contract["bigint_unary"]:
            value = int(case["operand"])
            result = -value if case["operation"] == "Math.neg" else abs(value)
            self.assertEqual(case["expect"], str(result), case["id"])


if __name__ == "__main__":
    unittest.main()
