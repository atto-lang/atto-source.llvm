#include <cstdint>
#include <cstring>
#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <limits>
extern "C" std::int64_t atto_scalar_size_v1(),atto_scalar_fields_v1(),atto_scalar_initial_v1(void*,std::int64_t),atto_scalar_encode_v1(const void*,std::int64_t,void*,std::int64_t),atto_scalar_decode_v1(const void*,std::int64_t,void*,std::int64_t);
using I=std::int64_t;
static std::string hex(const unsigned char* b,std::size_t n){std::string s;for(std::size_t i=0;i<n;++i){s+="0123456789abcdef"[b[i]>>4];s+="0123456789abcdef"[b[i]&15];}return s;}
static void rehash(std::array<unsigned char,47>& b){std::uint64_t h=UINT64_C(0xcbf29ce484222325);for(std::size_t i=0;i<b.size()-8;++i){h^=b[i];h*=UINT64_C(0x100000001b3);}for(unsigned i=0;i<8;++i)b[b.size()-8+i]=static_cast<unsigned char>(h>>(8*i));}
int main(){
 assert(atto_scalar_size_v1()==47&&atto_scalar_fields_v1()==3);
 std::array<unsigned char,47> b{};std::array<I,3> v{},out{};
 assert(atto_scalar_initial_v1(b.data(),b.size())==47);
 assert(hex(b.data(),b.size())=="4154544f5354303101f9b8eff04a5474590300010000000000000080020101ffffffffffffff7f4191e1156e4df645");
 assert(atto_scalar_decode_v1(b.data(),b.size(),out.data(),out.size())==3);
 assert((out==std::array<I,3>{std::numeric_limits<I>::min(),1,std::numeric_limits<I>::max()}));
 for(I i=-5000;i<=5000;++i){v={i,i%2==0?1:0,-i};assert(atto_scalar_encode_v1(v.data(),3,b.data(),47)==47);assert(atto_scalar_decode_v1(b.data(),47,out.data(),3)==3&&out==v);}
 auto valid=b;out.fill(999);const auto sentinel=out;
 for(std::size_t i=0;i<b.size();++i){assert(atto_scalar_decode_v1(b.data(),i,out.data(),3)==-1&&out==sentinel);for(unsigned bit=0;bit<8;++bit){auto bad=valid;bad[i]^=static_cast<unsigned char>(1u<<bit);assert(atto_scalar_decode_v1(bad.data(),47,out.data(),3)==-1&&out==sentinel);}}
 auto bad=valid;bad[29]=2;rehash(bad);assert(atto_scalar_decode_v1(bad.data(),47,out.data(),3)==-1&&out==sentinel);
 v[1]=2;assert(atto_scalar_encode_v1(v.data(),3,b.data(),47)==-1&&b==valid);assert(atto_scalar_encode_v1(v.data(),2,b.data(),47)==-1&&b==valid);assert(atto_scalar_initial_v1(b.data(),46)==-1&&b==valid);
 assert(atto_scalar_decode_v1(b.data(),47,out.data(),2)==-1&&out==sentinel);
 std::array<unsigned char,64> alias{};v={-7,1,99};std::memcpy(alias.data(),v.data(),sizeof(v));assert(atto_scalar_encode_v1(alias.data(),3,alias.data(),64)==47);assert(atto_scalar_decode_v1(alias.data(),47,alias.data(),3)==3);std::memcpy(out.data(),alias.data(),sizeof(out));assert(out==v);
 std::cout<<"LLVM_NATIVE_SCALAR_CODEC_PASS roundtrips=10001 bitflips=376\n";
}
