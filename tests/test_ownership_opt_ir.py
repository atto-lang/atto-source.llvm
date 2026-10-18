import unittest
from scripts.verify_ownership_optimization_ir import inspect, UnsafeOptimizationMetadata

SAFE = "define i64 @identity(i64 %value) {\nentry:\n ret i64 %value\n}\n"


class IRGuardTests(unittest.TestCase):
 def reject(self, ir):
  with self.assertRaises(UnsafeOptimizationMetadata):
   inspect(ir)

 def test_minimal_ir_is_not_vector_proof(self):
  result=inspect(SAFE)
  self.assertFalse(result["actual_vectorization_verified"])
  self.assertFalse(result["alias_disjointness_verified"])

 def test_ir_comments_do_not_count_as_annotations(self):
  result=inspect(SAFE+"; !tbaa !1 and fast-math example only\n")
  self.assertEqual(result["status"],"UNPROVEN_OPTIMIZATIONS_ABSENT")

 def test_unsafe_alias_metadata(self):
  self.reject("store i32 1, ptr %p, !tbaa !1\n")
  self.reject("load i32, ptr %p, !alias.scope !2\n")
  self.reject("store i32 1, ptr %p, !noalias !3\n")
  self.reject("define void @f(ptr noalias %p) { ret void }\n")

 def test_unproven_vector_or_float_promises(self):
  self.reject("br label %loop, !llvm.loop !1\n")
  self.reject("%r = fadd fast float %a, %b\n")
  self.reject("%r = fmul reassoc float %a, %b\n")

 def test_bounded_admission(self):
  self.reject("a"*(1024*1024+1))


if __name__=="__main__":
 unittest.main()
