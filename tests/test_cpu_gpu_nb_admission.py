#!/usr/bin/env python3
"""Source-free tests: diagnostics must prove unsupported execution domains.

These tests validate only the classifier, not the real Oreslang compiler or
GPU execution. Keep the compiler-backed CTest separate and independently green.
"""
import unittest

from cpu_gpu_nb_admission import is_domain_refusal


class DomainRefusalEvidenceTest(unittest.TestCase):
    def test_unrelated_errors_are_not_evidence(self):
        for diagnostic in (
            "error: syntax error near gpu",
            "error: CPU input file could not be opened",
            "error: parse failed on nb",
            "error: unsupported integer syntax",
            "error: GPU driver file not found",
        ):
            with self.subTest(diagnostic=diagnostic):
                self.assertFalse(is_domain_refusal(diagnostic))

    def test_explicit_domain_refusals_are_evidence(self):
        for diagnostic in (
            "CPU/GPU execution domain and nb graph lowering unsupported; no implicit CPU fallback",
            "gpu source execution is not yet lowered",
            "nb CPU regions disabled",
            "execution-domain capability unavailable",
        ):
            with self.subTest(diagnostic=diagnostic):
                self.assertTrue(is_domain_refusal(diagnostic))


if __name__ == "__main__":
    unittest.main()
