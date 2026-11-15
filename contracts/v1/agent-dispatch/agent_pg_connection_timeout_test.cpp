#include "PostgresActorDispatchClaim.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace oreslang::agent_dispatch;
static bool saw_params=false;
extern "C" PGconn* __wrap_PQconnectdbParams(const char* const* keys, const char* const* values, int expand_dbname) {
    assert(expand_dbname==1);
    assert(std::strcmp(keys[0],"dbname")==0);
    assert(std::strcmp(keys[1],"connect_timeout")==0);
    assert(std::strcmp(values[1],"3")==0);
    assert(std::strcmp(keys[2],"options")==0);
    assert(std::strstr(values[2],"statement_timeout=10000")!=nullptr);
    assert(std::strstr(values[2],"lock_timeout=5000")!=nullptr);
    assert(keys[3]==nullptr);
    saw_params=true;
    return nullptr;
}
int main(){
    Scope s{"tenant","worker",std::string(40,'a'),"dispatch",std::string(64,'b'),7,9,9999999999999ULL,1024,24};
    PostgresActorDispatchClaim test("host=127.0.0.1 connect_timeout=0 options='-c statement_timeout=0'");
    assert(!test.claim_once(s));
    assert(saw_params);
    std::cout<<"LIBPQ_TIMEOUT_PARAMETERS_VERIFIED_FAIL_CLOSED\n";
}
