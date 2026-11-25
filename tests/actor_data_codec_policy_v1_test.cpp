#include "oreslang/runtime/ActorDataCodecPolicyV1.hpp"
#include <iostream>
#include <stdexcept>
using namespace oreslang::actor_codec_policy_v1;
template<class F> void rejected(Reject want, F action) {
    try { action(); }
    catch (const PolicyError& ex) {
        if (ex.reason != want) throw;
        return;
    }
    throw std::runtime_error("expected rejection");
}
int main() {
    Shape h{}; h.fill(7);
    std::string schema = "order.v1";
    const auto b = Binding::select(std::nullopt,15,1,2,schema,h);
    b.require_match("messagepack",2,"order.v1",h);
    Binding::select("protobuf",15,2,1,"s",h).require_match("protobuf",1,"s",h);
    Binding::select("cbor",4,4,1,"s",h).require_match("cbor",1,"s",h);
    Binding::select("flatbuffers",8,8,1,"s",h).require_match("flatbuffers",1,"s",h);
    rejected(Reject::Denied,[&]{ Binding::select(std::nullopt,2,2,1,"s",h); });
    rejected(Reject::Unsupported,[&]{ Binding::select("protobuf",15,1,1,"s",h); });
    rejected(Reject::UnknownCodec,[&]{ Binding::select("CBOR",15,15,1,"s",h); });
    rejected(Reject::UnknownCodec,[&]{ Binding::select("",15,15,1,"s",h); });
    rejected(Reject::InvalidMask,[&]{ Binding::select(std::nullopt,16,1,1,"s",h); });
    rejected(Reject::InvalidBinding,[&]{ Binding::select(std::nullopt,1,1,0,"s",h); });
    rejected(Reject::InvalidBinding,[&]{ Binding::select(std::nullopt,1,1,0x100000000ULL,"s",h); });
    for (const auto& s : {std::string{},std::string("a b"),std::string("\xc3\xa9"),std::string(257,'x')})
        rejected(Reject::InvalidBinding,[&]{ Binding::select(std::nullopt,1,1,2,s,h); });
    Binding::select(std::nullopt,1,1,2,std::string(256,'x'),h);
    rejected(Reject::Mismatch,[&]{ b.require_match("protobuf",2,"order.v1",h); });
    rejected(Reject::Mismatch,[&]{ b.require_match("messagepack",1,"order.v1",h); });
    rejected(Reject::Mismatch,[&]{ b.require_match("messagepack",2,"order.v2",h); });
    rejected(Reject::Mismatch,[&]{ b.require_match("MESSAGEPACK",2,"order.v1",h); });
    h[0]=8; schema[0]='X';
    rejected(Reject::Mismatch,[&]{ b.require_match("messagepack",2,"order.v1",h); });
    h[0]=7;
    b.require_match("messagepack",2,"order.v1",h);
    std::cout << "ACTOR_DATA_CODEC_POLICY_V1_HOST_PASS\n";
}
