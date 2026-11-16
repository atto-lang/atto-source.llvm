// Host-only, real C++17 execution of 16 portable structural projection cases.
// Not a compiled AttoLang guest, LLVM IR emission or actor-state escape.
#include "../src/main/cpp/oreslang/frontend/ReadonlyShapeProjectionV1.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::frontend::shape_v1;

void check(bool value, const char* id) {
    if (!value) throw std::runtime_error(std::string("FAIL ") + id);
}
template <typename T> void reject(T fn, const char* id) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected rejection ") + id);
}
field f(std::string name, scalar type, int slot) {
    return {std::move(name),type,slot,true,true,true,false};
}
source src(origin k, std::vector<field> fields) {
    return {k,7,1,9,false,std::move(fields)};
}
plan p(const source& s) {
    return prove(s, {{"x", scalar::i64}});
}

int main() {
    auto a = p(src(origin::class_, {f("x",scalar::i64,0)}));
    check(a.get_field("x").source_slot==0,"SP001");
    auto b = p(src(origin::class_, {f("padding",scalar::i64,0),f("x",scalar::i64,1)}));
    check(b.get_field("x").source_slot==1,"SP002");
    const std::vector<std::int64_t> a_data{42};
    const std::vector<std::int64_t> b_data{10,42};
    check(a_data.at(static_cast<std::size_t>(a.get_field("x").source_slot))
        + b_data.at(static_cast<std::size_t>(b.get_field("x").source_slot))==84,"host84");
    check(a.source_nominal_id==7 && b.loan_id==9,"identity/loan");
    check(p(src(origin::record,{f("extra",scalar::i64,0),f("x",scalar::i64,1)}))
        .get_field("x").source_slot==1,"SP003 record");
    check(p(src(origin::struct_,{f("extra",scalar::i64,0),f("x",scalar::i64,1)}))
        .get_field("x").source_slot==1,"SP003 struct");
    reject([] { auto v=f("x",scalar::i64,0); v.is_public=false;
                (void)p(src(origin::class_,{v})); },"SP004");
    reject([] { (void)p(src(origin::class_,{f("y",scalar::i64,0)})); },"SP005");
    reject([] { (void)p(src(origin::class_,{f("x",scalar::boolean,0)})); },"SP006");
    reject([] { auto v=src(origin::class_,{f("x",scalar::i64,0)});
                v.sealed_actor_state=true; (void)p(v); },"SP007");
    reject([] { auto v=f("x",scalar::i64,0);v.sealed_actor_state=true;
                (void)p(src(origin::class_,{v})); },"SP007 field");
    reject([] { (void)p(src(origin::map,{f("x",scalar::i64,0)})); },"SP008 map");
    reject([] { (void)p(src(origin::actor,{f("x",scalar::i64,0)})); },"SP008 actor");
    reject([] { auto v=f("x",scalar::i64,0);v.immutable=false;
                (void)p(src(origin::class_,{v})); },"SP009");
    reject([] { auto v=src(origin::class_,{f("x",scalar::i64,0)});
                v.layout_epoch=0; (void)p(v); },"SP010");
    reject([] { auto v=src(origin::class_,{f("x",scalar::i64,0)});
                v.loan_id=0; (void)p(v); },"SP011");
    reject([] { (void)p(src(origin::class_,{f("x",scalar::i64,0),
                 f("x",scalar::i64,1)})); },"SP012");
    reject([] { (void)p(src(origin::class_,{f("x",scalar::i64,0),
                 f("y",scalar::i64,0)})); },"SP013");
    reject([] { (void)prove(src(origin::class_,{f("x",scalar::i64,0)}),{}); },"SP014");
    reject([] { auto v=src(origin::class_,{f("x",scalar::i64,0)});
                v.nominal_id=0; (void)p(v); },"SP015");
    reject([] { (void)prove(src(origin::class_,{f("x",scalar::i64,0)}),
                  {{"x",scalar::i64},{"x",scalar::i64}}); },"SP016");
    std::cout << "readonly shape LLVM C++ HOST_WITNESS 16/16; target i64 oracle 84; native guest NOT_EXECUTED\n";
}
