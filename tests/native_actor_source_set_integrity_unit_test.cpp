#include "oreslang/frontend/ActorSourceSetAdmission.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

static void check(bool ok, const char* description) {
    if (!ok) throw std::runtime_error(description);
}

int main() {
    using namespace oreslang::frontend;
    try {
        check(source_set_git_blob_sha("") == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", "empty Git blob");
        check(source_set_git_blob_sha("hello\n") == "ce013625030ba8dba906f756967f9e9ca394464a", "Git SHA test vector");
        check(source_set_git_blob_sha("source code\n") == "5ef7ecc5423c1f9f9ba75d5f74a975e67d105350", "Git SHA multiword");
        check(source_set_identifier("CounterContract") && source_set_identifier("Counter_1"), "identifiers");
        check(!source_set_identifier("1Counter") && !source_set_identifier("#Counter") &&
              !source_set_identifier("Counter.Name"), "forbidden identifier syntax");
        for (std::string_view path : {"../src/counter/actor.atto", "/tmp/x.atto", "src//x.atto",
                                      "src/./x.atto", "src/../x.atto", "src\\x.atto",
                                      "C:/x.atto", "src/x.atto/", "src/xyz\n.atto"})
            check(!source_set_path(path), "unsafe path admitted");
        check(source_set_path(".atto.pkg.toml") && source_set_path("src/counter/actor.atto"), "valid path denied");
        check(source_set_hex_sha("ce013625030ba8dba906f756967f9e9ca394464a"), "valid SHA");
        check(!source_set_hex_sha("CE013625030BA8DBA906F756967F9E9CA394464A") &&
              !source_set_hex_sha("ce013625030ba8dba906f756967f9e9ca394464"), "bad SHA admitted");
        std::cout << "ACTOR_SOURCE_SET_HOST_ADMISSION_UNIT_PASS NO_AST_NO_IR_NO_GUEST\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ACTOR_SOURCE_SET_HOST_ADMISSION_UNIT_FAILED " << e.what() << '\n';
        return 1;
    }
}
