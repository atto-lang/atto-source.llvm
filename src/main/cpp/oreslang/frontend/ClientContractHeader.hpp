#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace oreslang {
struct ClientContractMethod {
    std::string name;
    std::vector<std::string> parameters;
};
struct ClientContractHeader {
    std::string name;
    std::vector<ClientContractMethod> methods;
};

// Admission for declaration-only generated Oreslang String RPC surfaces.
// This does not implement native String objects or executable method lowering.
ClientContractHeader parse_client_contract_header(std::string_view source);
} // namespace oreslang
