#include "oreslang/frontend/ImportPathVariables.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using oreslang::frontend::expand_static_import_path;

static std::string interpolate(
        std::string_view text, const std::map<std::string, std::string>& environment) {
    return expand_static_import_path(text, [&](std::string_view name) -> std::optional<std::string> {
        const auto found = environment.find(std::string(name));
        if (found == environment.end()) return std::nullopt;
        return found->second;
    });
}

static void equal(std::string_view raw, const std::map<std::string, std::string>& environment,
                  std::string_view expected) {
    const auto value = interpolate(raw, environment);
    if (value != expected) throw std::runtime_error("import variable interpolation mismatch");
}

static void denied(std::string_view raw, const std::map<std::string, std::string>& environment) {
    try {
        (void)interpolate(raw, environment);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("unsafe import interpolation unexpectedly accepted");
}

int main() {
    const std::map<std::string, std::string> vars = {
        {"repo", "/work/app"}, {"LIB_ROOT", "/work/libs"},
        {"MODULE", "src"}, {"VERSION", "v2"}
    };
    equal("${repo}/src/config", vars, "/work/app/src/config");
    equal("${LIB_ROOT}/${VERSION}/math", vars, "/work/libs/v2/math");
    equal("+/src/config", vars, "+/src/config");
    equal("+/${MODULE}/config", vars, "+/src/config");
    equal("$repo/src/config", vars, "$repo/src/config");
    equal("./helper", vars, "./helper");
    equal("../helper", vars, "../helper");

    denied("${HOME}/secret", vars);
    denied("${repo", vars);
    denied("${}", vars);
    denied("${1repo}", vars);
    denied("${bad-name}", vars);
    denied("${repo:-default}", vars);
    denied("${repo}/foo", {{"repo", ""}});
    denied("${repo}/foo", {{"repo", "${OTHER}"}});
    denied("${repo}/foo", {{"repo", "a\nline"}});
    denied("${repo}/foo", {{"repo", "a\tpath"}});
    denied("${repo}/foo", {{"repo", std::string("a\0b", 3)}});
    denied("${repo}/foo", {{"repo", std::string("\xC2\x85", 2)}});
    denied("${repo}/foo", {{"repo", std::string(4097, 'x')}});
    denied("${repo}/foo", {{"repo", std::string(2049, 'x') + std::string(2049, 'x')}});
    denied(std::string(4097, 'x'), {});
    return 0;
}
