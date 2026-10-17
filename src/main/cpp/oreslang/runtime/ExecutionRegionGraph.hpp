#pragma once
// Experimental typed CpuRegion/GpuRegion AST + ordered Host/Device graph IR.
// Standalone parity reference, NOT a native source parser or a GPU provider.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::execution_region {
enum class Type { Void, I64, HostF32, GpuF32, FutureVoid, FutureI64, FutureHostF32, FutureGpuF32 };
enum class Domain { CPU, GPU, HOST, TRANSFER };
enum class Effect { Host, Device, Transfer, Suspend };
enum class Op { ConstI64, AddI64, Param, Upload, Download, DeviceKernel,
                DeviceFence, DeviceAdmission, Submit, Await };
inline Type future(Type type) {
 switch(type){
 case Type::Void:return Type::FutureVoid;
 case Type::I64:return Type::FutureI64;
 case Type::HostF32:return Type::FutureHostF32;
 case Type::GpuF32:return Type::FutureGpuF32;
 default:throw std::invalid_argument("nested futures require explicit composition");
 }
}
inline Type awaited(Type type) {
 switch(type){
 case Type::FutureVoid:return Type::Void;
 case Type::FutureI64:return Type::I64;
 case Type::FutureHostF32:return Type::HostF32;
 case Type::FutureGpuF32:return Type::GpuF32;
 default:throw std::invalid_argument("await requires Future<T>");
 }
}
inline bool is_owned(Type t){return t==Type::HostF32 || t==Type::GpuF32;}
inline bool valid_name(const std::string& s) {
 if(s.empty()||s.size()>128)return false;
 auto first=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
 if(!first(s[0]))return false;
 for(char c:s)if(!first(c)&&!(c>='0'&&c<='9'))return false;
 return true;
}
struct Expr {
 enum class Kind { CpuRegion,GpuRegion,I64,Add,Param,Upload,Download,Kernel,Await,Seq };
 Kind kind;
 std::vector<Expr> children{};
 std::string name{};
 Type type{Type::Void};
 std::int64_t literal{0};
 bool nb{false};
 static Expr cpu(Expr child,bool nb=false){return {Kind::CpuRegion,{std::move(child)}, {},Type::Void,0,nb};}
 static Expr gpu(Expr child,bool nb=false){return {Kind::GpuRegion,{std::move(child)}, {},Type::Void,0,nb};}
 static Expr i64(std::int64_t v){return {Kind::I64,{}, {},Type::I64,v,false};}
 static Expr add(Expr l,Expr r){return {Kind::Add,{std::move(l),std::move(r)}};}
 static Expr param(std::string n,Type t){return {Kind::Param,{},std::move(n),t};}
 static Expr upload(Expr v){return {Kind::Upload,{std::move(v)}};}
 static Expr download(Expr v){return {Kind::Download,{std::move(v)}};}
 static Expr kernel(std::string n,Expr v){return {Kind::Kernel,{std::move(v)},std::move(n)};}
 static Expr await(Expr v){return {Kind::Await,{std::move(v)}};}
 static Expr seq(std::vector<Expr> values){return {Kind::Seq,std::move(values)};}
};
struct Step {
 std::size_t id;
 Op opcode;
 Domain domain;
 Type type;
 std::vector<std::size_t> dependencies;
 std::int64_t left{-1},right{-1},literal{0};
 std::string detail{};
};
struct Graph {
 std::vector<Step> steps;
 Type result{Type::Void};
 std::int64_t return_id{-1};
 std::set<Effect> effects;
 std::size_t device_kernels{0};
};
class Lowerer {
 struct Value {Type type;std::int64_t id;};
 std::map<std::string,Type> params_;
 std::set<std::string> moved_;
 Graph graph_{};
 Value emit(Op opcode,Domain domain,Type type,
            std::int64_t left=-1,std::int64_t right=-1,
            std::int64_t literal=0,std::string detail={}) {
  if(graph_.steps.size()>=2048)throw std::invalid_argument("graph node budget");
  const auto id=graph_.steps.size();
  std::set<std::size_t> deps;
  if(left>=0)deps.insert(static_cast<std::size_t>(left));
  if(right>=0)deps.insert(static_cast<std::size_t>(right));
  if(id>0)deps.insert(id-1);
  if(!deps.empty()&&*deps.rbegin()>=id)throw std::invalid_argument("graph dependency cycle");
  graph_.steps.push_back({id,opcode,domain,type,{deps.begin(),deps.end()},left,right,literal,std::move(detail)});
  return {type,static_cast<std::int64_t>(id)};
 }
 Value visit(const Expr& x,Domain domain,std::size_t depth) {
  if(depth>64)throw std::invalid_argument("execution AST depth");
  auto operand=[&]()->Value{
   if(x.children.size()!=1)throw std::invalid_argument("invalid AST unary operand count");
   return visit(x.children[0],domain,depth+1);
  };
  switch(x.kind){
   case Expr::Kind::I64:
    if(domain!=Domain::CPU)throw std::invalid_argument("host scalar in GPU region");
    graph_.effects.insert(Effect::Host);
    return emit(Op::ConstI64,Domain::CPU,Type::I64,-1,-1,x.literal);
   case Expr::Kind::Param: {
    auto it=params_.find(x.name);
    if(!valid_name(x.name)||it==params_.end()||it->second!=x.type)
      throw std::invalid_argument("unknown/mistyped parameter");
    if(is_owned(x.type)&&!moved_.insert(x.name).second)
      throw std::invalid_argument("use of moved buffer parameter");
    return emit(Op::Param,Domain::HOST,x.type,-1,-1,0,x.name);
   }
   case Expr::Kind::Add: {
    if(domain!=Domain::CPU||x.children.size()!=2)
      throw std::invalid_argument("host Add requires CPU and two inputs");
    const Value l=visit(x.children[0],domain,depth+1),r=visit(x.children[1],domain,depth+1);
    if(l.type!=Type::I64||r.type!=Type::I64)throw std::invalid_argument("Add requires i64");
    graph_.effects.insert(Effect::Host);
    return emit(Op::AddI64,Domain::CPU,Type::I64,l.id,r.id);
   }
   case Expr::Kind::Upload:case Expr::Kind::Download: {
    if(domain!=Domain::CPU)throw std::invalid_argument("transfer requires host continuation");
    const Value v=operand();
    const bool upload=x.kind==Expr::Kind::Upload;
    if(v.type!=(upload?Type::HostF32:Type::GpuF32))
      throw std::invalid_argument("host/device residency mismatch");
    graph_.effects.insert(Effect::Transfer);graph_.effects.insert(Effect::Suspend);
    return emit(upload?Op::Upload:Op::Download,Domain::TRANSFER,
                upload?Type::GpuF32:Type::HostF32,v.id);
   }
   case Expr::Kind::Kernel: {
    if(domain!=Domain::GPU||!valid_name(x.name))
      throw std::invalid_argument("kernel requires GPU region and checked name");
    const Value v=operand();
    if(v.type!=Type::GpuF32)throw std::invalid_argument("device kernel input residency");
    graph_.effects.insert(Effect::Device);graph_.effects.insert(Effect::Suspend);
    ++graph_.device_kernels;
    return emit(Op::DeviceKernel,Domain::GPU,Type::GpuF32,v.id,-1,0,x.name);
   }
   case Expr::Kind::Await: {
    if(domain!=Domain::CPU)throw std::invalid_argument("await requires host continuation");
    const Value v=operand();
    const Type result=awaited(v.type);
    graph_.effects.insert(Effect::Suspend);
    return emit(Op::Await,Domain::CPU,result,v.id);
   }
   case Expr::Kind::Seq: {
    Value result{Type::Void,-1};
    for(const auto& child:x.children)result=visit(child,domain,depth+1);
    return result;
   }
   case Expr::Kind::CpuRegion:case Expr::Kind::GpuRegion: {
    if(x.children.size()!=1)throw std::invalid_argument("region requires one body");
    const Domain next=x.kind==Expr::Kind::CpuRegion?Domain::CPU:Domain::GPU;
    if(domain==Domain::GPU&&next==Domain::CPU){
     graph_.effects.insert(Effect::Suspend);
     emit(Op::DeviceFence,Domain::TRANSFER,Type::Void);
    }
    if(domain==Domain::CPU&&next==Domain::GPU){
     graph_.effects.insert(Effect::Suspend);
     emit(Op::DeviceAdmission,Domain::TRANSFER,Type::Void);
    }
    const auto kernels=graph_.device_kernels;
    const Value v=visit(x.children[0],next,depth+1);
    if(next==Domain::GPU&&kernels==graph_.device_kernels)
      throw std::invalid_argument("empty GPU region has no kernel effect");
    if(!x.nb)return v;
    graph_.effects.insert(Effect::Suspend);
    return emit(Op::Submit,Domain::HOST,future(v.type),v.id);
   }
  }
  throw std::invalid_argument("unknown execution AST kind");
 }
 public:
  explicit Lowerer(std::map<std::string,Type> params):params_(std::move(params)) {
   if(params_.size()>64)throw std::invalid_argument("parameter budget");
   for(const auto& [name,t]:params_)if(!valid_name(name)||t==Type::Void||t==Type::FutureVoid||t==Type::FutureI64||t==Type::FutureHostF32||t==Type::FutureGpuF32)
    throw std::invalid_argument("invalid parameter declaration");
  }
  Graph lower(const Expr& e) {
   const auto v=visit(e,Domain::CPU,0);
   graph_.result=v.type;graph_.return_id=v.id;
   return std::move(graph_);
  }
};
inline Graph lower(const Expr& root,std::map<std::string,Type> params={}) {
 return Lowerer(std::move(params)).lower(root);
}
inline std::int64_t execute_cpu_i64(const Graph& g) {
 if(g.result!=Type::I64||g.return_id<0||static_cast<std::size_t>(g.return_id)>=g.steps.size()||g.steps.size()>2048)
  throw std::invalid_argument("CPU oracle needs valid i64 result");
 std::vector<std::int64_t> values(g.steps.size());
 for(std::size_t i=0;i<g.steps.size();++i){
  const auto& s=g.steps[i];
  if(s.id!=i||s.domain!=Domain::CPU)
   throw std::invalid_argument("CPU oracle cannot execute GPU, transfer, async");
  for(auto dep:s.dependencies)if(dep>=i)throw std::invalid_argument("cyclic dependency");
  if(s.opcode==Op::ConstI64){
   if(s.type!=Type::I64)throw std::invalid_argument("untyped constant");
   values[i]=s.literal;
  }else if(s.opcode==Op::AddI64){
   if(s.type!=Type::I64||s.left<0||s.right<0||s.left>=static_cast<std::int64_t>(i)||s.right>=static_cast<std::int64_t>(i)||
      std::find(s.dependencies.begin(),s.dependencies.end(),static_cast<std::size_t>(s.left))==s.dependencies.end()||
      std::find(s.dependencies.begin(),s.dependencies.end(),static_cast<std::size_t>(s.right))==s.dependencies.end()||
      g.steps[static_cast<std::size_t>(s.left)].type!=Type::I64||
      g.steps[static_cast<std::size_t>(s.right)].type!=Type::I64)
    throw std::invalid_argument("untyped SSA add operands");
   const auto lhs=values[static_cast<std::size_t>(s.left)],rhs=values[static_cast<std::size_t>(s.right)];
   if((rhs>0&&lhs>std::numeric_limits<std::int64_t>::max()-rhs)||
      (rhs<0&&lhs<std::numeric_limits<std::int64_t>::min()-rhs))
    throw std::overflow_error("checked i64 addition overflow");
   values[i]=lhs+rhs;
  }else throw std::invalid_argument("CPU-only oracle unknown opcode");
 }
 return values[static_cast<std::size_t>(g.return_id)];
}
} // namespace oreslang::execution_region
