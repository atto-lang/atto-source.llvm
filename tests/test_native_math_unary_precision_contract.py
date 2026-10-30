import hashlib
import json
import struct
import unittest
from decimal import Decimal, Context, ROUND_HALF_EVEN, localcontext
from fractions import Fraction
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



class BigIntExactReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / "contracts/v1/bigint-exact-v1.json"
        raw = path.read_bytes()
        assert git_blob(raw) == "b49c7a7d9395f7298e5534c4a6e5b90d4a2a518c"
        cls.contract = json.loads(raw)

    def test_div_rem_contract(self):
        self.assertEqual("oreslang.bigint-exact.v1", self.contract["contract"])
        for item in self.contract["division_cases"]:
            a, b = int(item["left"]), int(item["right"])
            if item.get("error"):
                self.assertEqual(0, b)
                continue
            q = abs(a) // abs(b)
            if (a < 0) != (b < 0):
                q = -q
            r = a - q * b
            self.assertEqual(item["quot"], str(q), item["id"])
            self.assertEqual(item["rem"], str(r), item["id"])
            self.assertEqual(a, q * b + r, item["id"])


class DecimalPrecisionReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / "contracts/v1/decimal-precision-v1.json"
        raw = path.read_bytes()
        assert git_blob(raw) == "3bbb44277f3816add53da40a1e161e466ab162b1"
        cls.contract = json.loads(raw)

    def test_exact_and_context_decimal_reference(self):
        self.assertEqual("oreslang.decimal-precision.v1", self.contract["contract"])
        for item in self.contract["exact_cases"]:
            a, b = Decimal(item["left"]), Decimal(item["right"])
            if item["operation"] == "add":
                actual = a + b
            elif item["operation"] == "sub":
                actual = a - b
            elif item["operation"] == "mul":
                actual = a * b
            else:
                if b == 0:
                    self.assertEqual("DIVISOR_ZERO", item["error"], item["id"])
                    continue
                fraction = Fraction(a) / Fraction(b)
                denominator = fraction.denominator
                while denominator % 2 == 0: denominator //= 2
                while denominator % 5 == 0: denominator //= 5
                if denominator != 1:
                    self.assertEqual("NONTERMINATING_REQUIRES_CONTEXT",
                                     item["error"], item["id"])
                    continue
                actual = a / b
            self.assertEqual(Decimal(item["expect"]), actual, item["id"])

        for item in self.contract["context_cases"]:
            ctx = Context(prec=item["precision"], rounding=ROUND_HALF_EVEN)
            with localcontext(ctx):
                actual = (Decimal(item["left"]) / Decimal(item["right"])
                          if item["operation"] == "div"
                          else +Decimal(item["operand"]))
            self.assertEqual(Decimal(item["expect"]), actual, item["id"])

if __name__ == "__main__":
    unittest.main()
