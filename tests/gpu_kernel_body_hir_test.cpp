#include "oreslang/frontend/GpuKernelBodyHir.hpp"
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace oreslang::gpu::body_hir;
static std::uint64_t checks = 0;
static void expect(bool value, const char* detail) {
    ++checks;
    if (!value) throw std::runtime_error(detail);
}
template <typename F>
static void reject_as(F f, const char* code) {
    bool rejected = false;
    try { (void)f(); }
    catch (const std::invalid_argument& e) {
        rejected = std::string(e.what()).rfind(code, 0) == 0;
    }
    expect(rejected, code);
}
static std::vector<Parameter> saxpy(Type n = Type::I64) {
    return {{"alpha",Type::F32},{"x",Type::ReadF32},{"y",Type::ReadF32},
            {"out",Type::WriteF32},{"n",n}};
}
static std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> result;
    for (std::size_t i=0;i<text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\n' || c == '\t') { ++i; continue; }
        const auto begin = i;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            ++i;
            while (i<text.size() &&
                    ((text[i]>='a'&&text[i]<='z') ||
                     (text[i]>='A'&&text[i]<='Z') ||
                     (text[i]>='0'&&text[i]<='9') || text[i]=='_')) ++i;
            result.push_back({TokenKind::Identifier,std::string(text.substr(begin,i-begin))});
        } else if(c>='0'&&c<='9') {
            ++i;
            while (i<text.size() && text[i]>='0' && text[i]<='9') ++i;
            result.push_back({TokenKind::Integer,std::string(text.substr(begin,i-begin))});
        } else {
            ++i;
            if(c=='>' && i<text.size() && text[i]=='=') ++i;
            result.push_back({TokenKind::Symbol,std::string(text.substr(begin,i-begin))});
        }
    }
    return result;
}
static const char* signed_good =
    "const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } fi";
static const char* unsigned_good =
    "const u64 lane = gpu.global_id(0); if lane < n { out[lane] = alpha * x[lane] + y[lane]; }";
static const char* early_good =
    "const int i = gpu.global_id(0); if i >= n { return; } out[i] = alpha * x[i] + y[i]; return;";
int main() {
    const auto a=check(saxpy(),tokenize(signed_good));
    expect(a.source_body_typed() && !a.executable() &&
           !a.device_code_available() && !a.provider_loans_verified() &&
           a.guard==Guard::LessInside &&
           a.instructions.size()==7, "signed SAXPY source body");
    expect(a.instructions.front().opcode==Opcode::GlobalId &&
           a.instructions.back().opcode==Opcode::StoreF32, "typed HIR instruction order");
    const auto b=check(saxpy(Type::U64),tokenize(unsigned_good));
    expect(b.guard==Guard::LessInside && b.lane=="lane","unsigned SAXPY body");
    const auto c=check(saxpy(),tokenize(early_good));
    expect(c.guard==Guard::GreaterEqualEarlyReturn && c.instructions.size()==8,
           "early-return guard");
    const auto noop=check({},tokenize("return;"));
    expect(noop.guard==Guard::NoOp && !noop.executable(),"no-op body");
    for (const auto& [body, code] : std::vector<std::pair<std::string,const char*>>{
        {"out[i] = alpha * x[i] + y[i];","GPU-LAUNCH-008"},
        {"const int i = gpu.global_id(0); out[i] = alpha * x[i] + y[i];","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(1); if i < n { out[i] = alpha * x[i] + y[i]; }","GPU-LAUNCH-003"},
        {"const u64 i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; }","GPU-LAUNCH-002"},
        {"const int alpha = gpu.global_id(0); if alpha < n { out[alpha] = alpha * x[alpha] + y[alpha]; }","GPU-LAUNCH-001"},
        {"const int i = gpu.global_id(0); if i > n { out[i] = alpha * x[i] + y[i]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { x[i] = alpha * x[i] + y[i]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha * out[i] + y[i]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[n] = alpha * x[i] + y[i]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[n] + y[i]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[n]; }","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha + x[i] + y[i]; }","GPU-LAUNCH-002"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] - y[i]; }","GPU-LAUNCH-002"},
        {"const int i = gpu.global_id(0); if i >= n { return; } out[i] = alpha * x[n] + y[i];","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i >= n { } out[i] = alpha * x[i] + y[i];","GPU-LAUNCH-005"},
        {"const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } out[i] = alpha * x[i] + y[i];","GPU-LAUNCH-008"}
    }) {
        reject_as([&]{return check(saxpy(),tokenize(body));},code);
    }
    auto wrong=saxpy();
    wrong[3].type=Type::ReadF32;
    reject_as([&]{return check(wrong,tokenize(signed_good));},"GPU-LAUNCH-008");
    wrong=saxpy();wrong[0].type=Type::I64;
    reject_as([&]{return check(wrong,tokenize(signed_good));},"GPU-LAUNCH-008");
    wrong=saxpy();wrong[4].name="alpha";
    reject_as([&]{return check(wrong,tokenize(signed_good));},"GPU-LAUNCH-001");

    // Deterministically mutate the two guaranteed type/bounds sites.
    std::mt19937_64 rng{0xA770B0D111ULL};
    for (int i=0;i<4096;++i) {
        auto t=tokenize(signed_good);
        const unsigned variant=static_cast<unsigned>(rng()%4);
        if(variant==0) {
            for(auto& v:t) if(v.text=="global_id"){v.text="host_global_id";break;}
            reject_as([&]{return check(saxpy(),t);},"GPU-LAUNCH-008");
        } else if(variant==1) {
            for(std::size_t j=0;j+1<t.size();++j) if(t[j].text=="if") {
                t[j+2].text=">";break;
            }
            reject_as([&]{return check(saxpy(),t);},"GPU-LAUNCH-005");
        } else if(variant==2) {
            for(std::size_t j=0;j<t.size();++j) if(t[j].text=="out") {
                t[j].text="x";break;
            }
            reject_as([&]{return check(saxpy(),t);},"GPU-LAUNCH-005");
        } else {
            for(std::size_t j=0;j<t.size();++j) if(t[j].text=="+" ) {
                t[j].text="-";break;
            }
            reject_as([&]{return check(saxpy(),t);},"GPU-LAUNCH-002");
        }
    }
    std::cout << "PASS " << checks
              << " typed SAXPY source-body assertions; 4096 deterministic negative mutations; "
              << "gpu_guest_executed=false\n";
}
