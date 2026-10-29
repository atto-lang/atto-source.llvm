#include "oreslang/frontend/PackageNamespaceGraph.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;
using namespace oreslang::frontend;

static Program make_source(const std::string& ns, const std::string& name, bool exported=true) {
    Program program;
    program.namespace_name = ns;
    Function fn;
    fn.name = name;
    fn.exported = exported;
    program.functions.push_back(std::move(fn));
    return program;
}

template<typename F>
static void must_reject(F&& op, const std::string& message) {
    try {
        op();
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()).find(message) != std::string::npos);
        return;
    }
    assert(false && "expected namespace package linker to reject invalid graph");
}

int main() {
    // Each Program owns movable native AST. std::map gives stable references.
    std::map<std::string, Program> programs;
    const std::vector<std::pair<std::string,std::string>> input = {
        {"compute/math/add", "a"}, {"compute/stats/double", "a"},
        {"other/aggregation/aggregate", "a"}, {"data/model", "b"},
        {"data/queries", "b"}, {"data/storage", "b"},
        {"data/validation", "b"}, {"api/routes", "c"},
        {"api/handlers", "c"}, {"api/responses", "c"}
    };
    for (const auto& entry : input) {
        const auto& file = entry.first;
        const std::string symbol = file.substr(file.rfind('/') + 1);
        programs.emplace(file, make_source(entry.second, symbol));
    }
    programs.emplace("app/main", Program{});

    std::vector<NamespaceOwnedSource> units;
    for (auto& [path, program] : programs)
        units.push_back({path, path == "app/main" ? "app@1" : "foo@1", &program});
    const PackageAliasPins pins{{"app@1", {{"foo", "foo@1"}}}};
    const NamespaceImportRequest request{"app/main", "foo", "a"};

    // 1. Ten source units split into three package-local namespaces.
    auto graph = link_package_namespaces(units, pins, {request});
    assert(graph.namespaces.size() == 3);
    assert(graph.namespaces.at({"foo@1", "a"}).source_units.size() == 3);
    assert(graph.namespaces.at({"foo@1", "b"}).source_units.size() == 4);
    assert(graph.namespaces.at({"foo@1", "c"}).source_units.size() == 3);
    assert(graph.imports.at({"app/main", "a"}).package_instance_id == "foo@1");
    assert(graph.namespaces.at({"foo@1", "a"}).exports.size() == 3);

    // 2. Source declarations in a second package sharing "a" cannot merge.
    Program other = make_source("a", "foreign");
    auto another = units;
    another.push_back({"other/a", "other@1", &other});
    auto separate = link_package_namespaces(another, pins, {request});
    assert(separate.namespaces.size() == 4);
    assert(separate.namespaces.at({"foo@1", "a"}).exports.size() == 3);
    assert(separate.namespaces.at({"other@1", "a"}).exports.size() == 1);

    // 3. Duplicate public symbol/signature inside same package fails closed.
    Program duplicate = make_source("a", "add");
    another = units;
    another.push_back({"foo/duplicate", "foo@1", &duplicate});
    must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                "duplicate public namespace");

    // 4. Missing pin and wrong package version cannot fall back by spelling.
    must_reject([&] { (void)link_package_namespaces(units, {}, {request}); },
                "undeclared pinned package alias");
    must_reject([&] {
        (void)link_package_namespaces(units, {{"app@1", {{"foo", "foo@2"}}}}, {request});
    }, "verified package inventory");
    must_reject([&] {
        (void)link_package_namespaces(units, pins, {{"app/main", "foo", "missing"}});
    }, "no cross-package fallback");

    // 5. Private declarations are not exposed by any namespace import.
    Program private_helper = make_source("a", "private_helper", false);
    another = units;
    another.push_back({"foo/private", "foo@1", &private_helper});
    auto private_graph = link_package_namespaces(another, pins, {request});
    assert(private_graph.namespaces.at({"foo@1", "a"}).source_units.size() == 4);
    assert(private_graph.namespaces.at({"foo@1", "a"}).exports.size() == 3);

    std::cout << "native package namespace source-only graph: 5/5 cases passed\n";
}
