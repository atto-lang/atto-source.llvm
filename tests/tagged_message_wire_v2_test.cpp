#include "oreslang/runtime/MessagePackWireV2.hpp"
#include <openssl/evp.h>
#include <stdexcept>
#include <iostream>
#include <string>
#include <vector>

using namespace oreslang::wire;

static std::string sha256hex(const std::string& text) {
    unsigned char digest[32];
    unsigned int len = 0;
    require(EVP_Digest(text.data(),text.size(),digest,&len,EVP_sha256(),nullptr) == 1
            && len == 32,"no SHA256 provider");
    return hex(std::vector<unsigned char>(digest,digest+32));
}

int main() {
    try {
        // Independently reconstitute the compiler's domain/case wire
        // identity. No Java class or process-local pointer is required.
        const std::vector<std::string> alternatives = {
            "Status.Message1", "Status.Message2", "Status.Message3"
        };
        std::string domain = "oreslang.tagged.domain.v1/Messages/";
        for (const auto& entry : alternatives) {
            domain += std::to_string(entry.size()) + ":" + entry + ";";
        }
        std::string id = "oreslang.enum." + sha256hex(domain) + "."
                       + sha256hex("Status.Message2#2");
        auto shape=Shape::tuple({Shape::string(),Shape::integer(),Shape::string()});
        auto value=Value(Value::Items{Value("Status"),Value(2),Value("hello")});
        auto encoded=encode(id,shape,value);
        if (!(decode(id,shape,encoded)==value)) {
            throw std::runtime_error("native tagged enum decode roundtrip failed");
        }
        // The nominally distinct case must reject a reused scalar payload.
        bool rejected=false;
        try {
            (void) decode(id,Shape::tuple({
                Shape::integer(), Shape::integer(), Shape::string()}),encoded);
        } catch(const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("schema confusion not rejected");
        std::cout << hex(encoded) << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
