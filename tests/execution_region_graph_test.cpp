#include "ExecutionRegionGraph.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace oreslang::execution_region;
template<class F> void rejects(F f){
 bool failed=false;
 try{f();}catch(const std::exception&){failed=true;}
 assert(failed);
}
int main(){
 const auto cpu=lower(Expr::cpu(Expr::add(Expr::i64(40),Expr::i64(2))));
 assert(execute_cpu_i64(cpu)==42 && cpu.result==Type::I64);
 assert(execute_cpu_i64(lower(Expr::cpu(Expr::seq({
  Expr::i64(400),Expr::add(Expr::i64(40),Expr::i64(2))}))))==42);
 rejects([]{execute_cpu_i64(lower(Expr::cpu(
  Expr::add(Expr::i64(std::numeric_limits<std::int64_t>::max()),Expr::i64(1)))));});
 const auto nb=lower(Expr::cpu(Expr::i64(1),true));
 assert(nb.result==Type::FutureI64);
 rejects([]{lower(Expr::cpu(Expr::cpu(Expr::i64(1),true),true));});
 rejects([&]{execute_cpu_i64(nb);});
 rejects([]{lower(Expr::gpu(Expr::seq({})));});
 const auto gpu=lower(Expr::gpu(
  Expr::kernel("scale",Expr::param("dev",Type::GpuF32))),{{"dev",Type::GpuF32}});
 assert(gpu.result==Type::GpuF32&&gpu.device_kernels==1);
 rejects([&]{execute_cpu_i64(gpu);});
 rejects([]{lower(Expr::gpu(
  Expr::kernel("bad",Expr::param("host",Type::HostF32))),{{"host",Type::HostF32}});});
 rejects([]{lower(Expr::gpu(Expr::seq({
  Expr::kernel("once",Expr::param("dev",Type::GpuF32)),
  Expr::kernel("twice",Expr::param("dev",Type::GpuF32))})),{{"dev",Type::GpuF32}});});
 const auto nested=lower(Expr::gpu(Expr::seq({
  Expr::kernel("first",Expr::param("a",Type::GpuF32)),
  Expr::cpu(Expr::seq({
   Expr::download(Expr::param("b",Type::GpuF32)),
   Expr::gpu(Expr::kernel("second",Expr::param("c",Type::GpuF32)))}))})),
  {{"a",Type::GpuF32},{"b",Type::GpuF32},{"c",Type::GpuF32}});
 assert(nested.device_kernels==2);
 bool saw_fence=false;
 for(const auto& step:nested.steps)
  if(step.opcode==Op::DeviceFence)saw_fence=true;
 assert(saw_fence);
 std::cout<<"native CPU/GPU graph reference tests passed\n";
}
