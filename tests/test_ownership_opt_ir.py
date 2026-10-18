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
  self.reject("load i64, ptr %p, !invariant.load !0\n")
  self.reject("store i64 1, ptr %p, !invariant.group !0\n")
  self.reject("load i64, ptr %p, !llvm.access.group !0\n")
  self.reject("br label %loop, !llvm.mem.parallel_loop_access !0\n")

 def test_unproven_vector_or_float_promises(self):
  self.reject("br label %loop, !llvm.loop !1\n")
  self.reject("%r = fadd fast float %a, %b\n")
  self.reject("%r = fmul reassoc float %a, %b\n")
  self.reject("%r = fneg fast float %a\n")
  self.reject("%r = call fast float @llvm.sqrt.f32(float %a)\n")
  self.reject("%r = invoke nnan float @sqrt(float %a) to label %ok unwind label %bad\n")
  self.reject("%r = select fast i1 %pred, float %a, float %b\n")

 def test_bounded_admission(self):
  self.reject("a"*(1024*1024+1))


if __name__=="__main__":
 unittest.main()
