#include "oreslang/frontend/ClientContractHeader.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

static void reject(std::string_view source) {
    try {
        (void)oreslang::parse_client_contract_header(source);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("unsafe or unsupported contract was accepted");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 2;
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    auto parsed = oreslang::parse_client_contract_header(text);
    if (parsed.name != "ClientContract" || parsed.methods.size() != 1 ||
        parsed.methods[0].name != "list_items" ||
        parsed.methods[0].parameters.size() != 1 ||
        parsed.methods[0].parameters[0] != "page_size") {
        throw std::runtime_error("generated golden contract declaration mismatch");
    }
    reject("pub define contract C as fnc async() => String; end");
    for (const auto* word : {"over", "structural", "gpu", "quantum", "borrow", "int"}) {
        reject(std::string("pub define contract C as fnc ") + word + "() => String; end");
        reject(std::string("pub define contract C as fnc ok(String ") + word + ") => String; end");
    }
    reject("pub define contract C as fnc a(String x, String x) => String; end");
    reject("pub define contract C as fnc a() => String; fnc a() => String; end");
    reject("pub define contract C as async fnc a() => String; end");
    reject("pub define contract C as fnc a() => int; end");
    reject("pub define contract C as fnc a(String x = null) => String; end");
    reject("pub define contract C as fnc a() => String; end fnc malicious() => String;");
    reject("pub define contract C as fnc a(String x) => String;");
    auto empty = oreslang::parse_client_contract_header("define contract Empty as end");
    if (!empty.methods.empty()) throw std::runtime_error("empty contract mismatch");
    return 0;
}
