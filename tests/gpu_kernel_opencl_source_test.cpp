#include "oreslang/frontend/GpuKernelOpenClSource.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace oreslang::gpu;
static_assert(!opencl_source::Preview{}.compiler_device_binary());
static_assert(!opencl_source::Preview{}.authenticated_device());
static_assert(!opencl_source::Preview{}.executable());
static std::size_t checks=0;
static void require(bool ok, const char* detail) {
    ++checks;
    if (!ok) throw std::runtime_error(detail);
}
template<class F> static void denied(F f, const char* code) {
    bool caught=false;
    try { f(); } catch (const std::invalid_argument& e) {
        caught=std::string_view(e.what()).substr(0,std::char_traits<char>::length(code))==code;
        if (!caught) std::cerr << "expected " << code << " got " << e.what() << "\n";
    }
    require(caught,code);
}
static std::vector<body_hir::Parameter> params(bool signed_count=true) {
    using body_hir::Type;
    return {{"alpha",Type::F32},{"x",Type::ReadF32},{"y",Type::ReadF32},
            {"out",Type::WriteF32},{"n",signed_count?Type::I64:Type::U64}};
}
static std::vector<body_hir::Token> body(bool signed_count=true,bool early=false) {
    using body_hir::Token;
    using body_hir::TokenKind;
    std::vector<Token> t;
    auto id=[&](const char* s){t.push_back({TokenKind::Identifier,s});};
    auto sym=[&](const char* s){t.push_back({TokenKind::Symbol,s});};
    auto num=[&](const char* s){t.push_back({TokenKind::Integer,s});};
    id("const");id(signed_count?"int":"u64");id("i");sym("=");id("gpu");sym(".");
    id("global_id");sym("(");num("0");sym(")");sym(";");
    id("if");id("i");sym(early?">=":"<");id("n");sym("{");
    if(early){id("return");sym(";");sym("}");}
    id("out");sym("[");id("i");sym("]");sym("=");id("alpha");sym("*");
    id("x");sym("[");id("i");sym("]");sym("+");id("y");
    sym("[");id("i");sym("]");sym(";");
    if(!early) sym("}");
    return t;
}
int main(int argc,char** argv) {
    const auto s=opencl_source::emit_saxpy(params(),body());
    const auto u=opencl_source::emit_saxpy(params(false),body(false));
    const auto e=opencl_source::emit_saxpy(params(),body(true,true));
    require(s.signed_extent && !u.signed_extent && e.signed_extent,"explicit extent signedness");
    require(s.source.find("long count")!=std::string::npos &&
            u.source.find("ulong count")!=std::string::npos,"signed vs unsigned OpenCL ABI");
    require(s.source.find("count <= 0 || lane >= (ulong)count")!=std::string::npos,
            "signed negative bounds skip dispatch");
    require(u.source.find("lane >= count")!=std::string::npos &&
            u.source.find("count <= 0")==std::string::npos,"unsigned bounds skip dispatch");
    require(s.source==e.source,"guard and early-return canonicalized safely");
    require(s.source.find("restrict")==std::string::npos,"no unauthenticated exclusivity promise");
    require(s.source.find("get_global_id(0)")!=std::string::npos &&
            s.source.find("output[lane] = scale * input_x[lane] + input_y[lane]")!=std::string::npos,
            "SAXPY only, bounded expression");
    require(!s.executable() && !s.authenticated_device() && !s.compiler_device_binary(),
            "source text never promotes device authority");

    denied([&]{auto t=body();t[0].kind=body_hir::TokenKind::Integer;
        (void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-008");
    denied([&]{auto t=body();t[8].kind=body_hir::TokenKind::Identifier;
        (void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-008");
    denied([&]{auto t=body();t[0].kind=static_cast<body_hir::TokenKind>(99);
        (void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-008");
    denied([&]{auto p=params();p[3].name="out;evil()";
        (void)opencl_source::emit_saxpy(p,body());},"GPU-LAUNCH-001");
    denied([&]{auto p=params();p[4].name=std::string("n\0inject",8);
        (void)opencl_source::emit_saxpy(p,body());},"GPU-LAUNCH-001");
    denied([&]{auto p=params();p[3].type=body_hir::Type::ReadF32;
        (void)opencl_source::emit_saxpy(p,body());},"GPU-LAUNCH-008");
    denied([&]{auto t=body();t[16].text="x";
        (void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-005");
    denied([&]{std::vector<body_hir::Token> t={{body_hir::TokenKind::Identifier,"return"},
        {body_hir::TokenKind::Symbol,";"}};
        (void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-008");

    std::mt19937_64 rng{0xA7700C1ULL};
    constexpr int n=2048;
    for(int i=0;i<n;++i){
        auto t=body();
        switch(rng()%4){
            case 0:t[0].kind=body_hir::TokenKind::Integer;break;
            case 1:t[8].kind=body_hir::TokenKind::Identifier;break;
            case 2:t[10].kind=body_hir::TokenKind::Integer;break;
            case 3:t[7].text="(;";break;
        }
        denied([&]{(void)opencl_source::emit_saxpy(params(),t);},"GPU-LAUNCH-008");
    }
    if(argc==2){
        std::ofstream file(argv[1],std::ios::binary|std::ios::trunc);
        if(!file)throw std::runtime_error("OpenCL artifact path unavailable");
        file<<s.source;
        file.flush();
        if(!file)throw std::runtime_error("OpenCL source write failed");
    }
    std::cout<<"PASS "<<checks<<" offline OpenCL source-only checks; "<<n
        <<" deterministic malformed-token mutations; gpu_executed=false\n";
}
