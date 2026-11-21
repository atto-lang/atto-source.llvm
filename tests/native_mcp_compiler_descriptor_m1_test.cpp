#include "oreslang/runtime/McpCompilerDescriptorsV1.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace oreslang::runtime::mcp_m1;
namespace {
struct Parameter {
    enum class Mode { Value, Borrow, BorrowMut };
    std::string class_name, generic_type, sum_type, pending_type_alias;
    Mode mode = Mode::Value;
};
struct Function {
    std::string name;
    bool exported = false, native_internal = false;
    std::string method_owner;
    std::vector<Parameter> parameters;
    std::vector<int> generic_parameters;
    bool scalar_while = false, async_source_i64 = false;
    std::vector<int> source_tasks, future_pipelines, actor_spawns,
        actor_sends, actor_stops, actor_actions, actor_field_writes,
        caught_throw_defers;
    std::string ready_await_actor, done_await_actor, ready_future_name,
        return_class, return_sum, pending_return_type_alias;
    bool has_declared_effects = false, caught_throw_value = false,
        returns_void = false;
};
struct Program {
    std::string module, namespace_name;
    std::vector<Function> functions;
    std::vector<int> actors, classes, cooperative_loops, generators;
};
int checks=0;
void check_at(bool b, int line) {
    ++checks;
    if (!b) { std::cerr << "descriptor check failed at line " << line << '\n'; std::abort(); }
}
#define check(v) check_at((v),__LINE__)
Function fn(std::string name, std::size_t arity, bool exported) {
    Function f;
    f.name=std::move(name);f.exported=exported;
    f.parameters.resize(arity);
    return f;
}
bool rejects(const Program& p) {
    try { (void)emit_json(p); return false; }
    catch(const std::runtime_error&) { return true; }
}
}
int main() {
    Program p;
    p.functions.push_back(fn("zeta",2,true));
    p.functions.push_back(fn("hidden",0,false));
    p.functions.push_back(fn("alpha",0,true));
    const auto first=emit_json(p);
    const auto second=emit_json(p);
    check(first==second);
    check(first.find("atto.mcp.aot-descriptors.m1.v1")!=std::string::npos);
    check(first.find("\"backend\":\"llvm\"")!=std::string::npos);
    check(first.find("\"retention\":\"opt_in\"")!=std::string::npos);
    check(first.find("\"executed_guest\":false")!=std::string::npos);
    check(first.find("\"abi\":\"atto.mcp.native.descriptor.v1\"")!=std::string::npos);
    check(first.find("\"generation_binding\":\"host_runtime_required\"")!=std::string::npos);
    check(first.find("\"module_id\":\"")!=std::string::npos);
    check(first.find("hidden")==std::string::npos);
    check(first.find("alpha")<first.find("zeta"));
    check(first.find("\"arity\":2")!=std::string::npos);
    check(first.find("\"public\":true")!=std::string::npos);
    check(first.find("pointer")==std::string::npos);
    check(first.find("source_path")==std::string::npos);
    check(hex_id(0)=="0000000000000000");
    check(hex_id(0xffffffffffffffffULL)=="ffffffffffffffff");
    check(name_ok("main") && name_ok("_foo123"));
    check(!name_ok("../etc/passwd") && !name_ok("bad\"name") && !name_ok("é"));
    auto reversed=Program{};
    reversed.functions.push_back(fn("alpha",0,true));
    reversed.functions.push_back(fn("hidden",0,false));
    reversed.functions.push_back(fn("zeta",2,true));
    check(emit_json(reversed)==first);
    p.functions.push_back(fn("zeta",2,true));
    check(rejects(p));
    p.functions.pop_back();
    p.functions.push_back(fn("bad-name",0,true));
    check(rejects(p));
    p.functions.pop_back();
    p.functions.push_back(fn("zeta",3,true));
    check(!rejects(p)); // distinct arity: legal value-arity overloading
    p.functions.pop_back();
    p.functions.push_back(fn("ok",33,true));
    check(rejects(p));
    p.functions.pop_back();
    p.functions.push_back(fn("generic",0,true));
    p.functions.back().generic_parameters.push_back(1);
    check(rejects(p));
    p.functions.pop_back();
    p.functions.push_back(fn("internal",0,true));
    p.functions.back().native_internal=true;
    check(rejects(p));
    p.functions.pop_back();
    p.functions.push_back(fn("runtime",1,true));
    auto& advanced = p.functions.back();
    advanced.async_source_i64=true;
    check(rejects(p));
    advanced.async_source_i64=false;
    advanced.source_tasks.push_back(1);
    check(rejects(p));
    advanced.source_tasks.clear();
    advanced.actor_spawns.push_back(1);
    check(rejects(p));
    advanced.actor_spawns.clear();
    advanced.has_declared_effects=true;
    check(rejects(p));
    advanced.has_declared_effects=false;
    advanced.returns_void=true;
    check(rejects(p));
    advanced.returns_void=false;
    advanced.return_class="Secret";
    check(rejects(p));
    advanced.return_class.clear();
    advanced.parameters.front().class_name="Secret";
    check(rejects(p));
    advanced.parameters.front().class_name.clear();
    advanced.parameters.front().mode=Parameter::Mode::Borrow;
    check(rejects(p));
    advanced.parameters.front().mode=Parameter::Mode::Value;
    advanced.parameters.front().generic_type="T";
    check(rejects(p));
    advanced.parameters.front().generic_type.clear();
    advanced.scalar_while=true;
    check(rejects(p));
    advanced.scalar_while=false;
    advanced.ready_await_actor="actor";
    check(rejects(p));
    advanced.ready_await_actor.clear();
    advanced.caught_throw_value=true;
    check(rejects(p));
    advanced.caught_throw_value=false;
    check(!rejects(p));
    p.functions.pop_back();
    p.module="bad/name";
    check(rejects(p));
    p.module="demo";
    check(!rejects(p));
    check(emit_json(p)!=first); // namespace/module participate in stable ID
    check(emit_json(p).find("\"module_id\":\"")!=std::string::npos);
    p.module.clear();
    p.namespace_name="another";
    check(emit_json(p)!=first);
    p.namespace_name.clear();
    check(emit_json(p)==first);
    for(int i=0;i<31;i++) p.functions.push_back(fn("f"+std::to_string(i),0,true));
    check(rejects(p)); // 32-entry compiler retention budget
    p.functions.pop_back();
    check(!rejects(p));
    p.actors.resize(1);
    check(rejects(p));
    p.actors.clear();
    p.classes.resize(1);
    check(rejects(p));
    std::cout << "LLVM compiler-owned M1 descriptor host contract passed " << checks
              << " checks (compiler CLI and guest not executed)\n";
}
