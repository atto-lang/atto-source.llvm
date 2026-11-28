#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
extern "C" uint32_t atto_bounded_fields_v2();
extern "C" uint64_t atto_bounded_schema_v2();
extern "C" int32_t atto_bounded_encode_v2(const uint8_t*,uint32_t,uint8_t*,uint32_t);
extern "C" int32_t atto_bounded_decode_v2(const uint8_t*,uint32_t,uint8_t*,uint32_t);
using B=std::vector<uint8_t>;
static void put(B& b,size_t p,uint64_t v,unsigned n=8){for(unsigned j=0;j<n;++j)b[p+j]=static_cast<uint8_t>(v>>(8*j));}
static B slots(int64_t n,bool yes,const B& s,const B& data){B b(64+s.size()+data.size());put(b,0,static_cast<uint64_t>(n));put(b,16,yes?1:0);put(b,32,64);put(b,40,s.size());put(b,48,64+s.size());put(b,56,data.size());std::copy(s.begin(),s.end(),b.begin()+64);std::copy(data.begin(),data.end(),b.begin()+64+s.size());return b;}
static void rehash(B& b){uint64_t h=UINT64_C(0xcbf29ce484222325);for(size_t i=0;i<b.size()-8;++i){h^=b[i];h*=UINT64_C(0x100000001b3);}put(b,b.size()-8,h);}
static B from_hex(std::string s){B b;for(size_t i=0;i<s.size();i+=2)b.push_back(static_cast<uint8_t>(std::stoul(s.substr(i,2),nullptr,16)));return b;}
int main(){
 assert(atto_bounded_fields_v2()==4&&atto_bounded_schema_v2()==UINT64_C(0xdb14dea8243c195b));
 const auto golden=from_hex("4154544f53543032025b193c24a8de14db0400010000000000000080020103080000004100c3a9f09f9880040300000000ff8074c4cc420e041ac0");
 B in=slots(INT64_MIN,true,{65,0,195,169,240,159,152,128},{0,255,128});B out(65536,0),decoded(69632,0);
 assert(atto_bounded_encode_v2(in.data(),in.size(),out.data(),out.size())==59&&std::equal(golden.begin(),golden.end(),out.begin()));
 assert(atto_bounded_decode_v2(golden.data(),golden.size(),decoded.data(),decoded.size())==static_cast<int32_t>(in.size())&&std::equal(in.begin(),in.end(),decoded.begin()));
 for(int64_t i=-5000;i<=5000;++i){in=slots(i,i%2==0,B(static_cast<size_t>((i+5000)%257),'a'),{static_cast<uint8_t>(i),0,255});auto n=atto_bounded_encode_v2(in.data(),in.size(),out.data(),out.size());assert(n>0);assert(atto_bounded_decode_v2(out.data(),n,decoded.data(),decoded.size())==static_cast<int32_t>(in.size())&&std::equal(in.begin(),in.end(),decoded.begin()));}
 B sentinel(80,99);auto deny=[&](const B& b,uint32_t n){B actual=sentinel;assert(atto_bounded_decode_v2(b.data(),n,actual.data(),actual.size())==-1&&actual==sentinel);};
 for(size_t i=0;i<golden.size();++i){deny(golden,i);for(unsigned j=0;j<8;++j){auto bad=golden;bad[i]^=static_cast<uint8_t>(1u<<j);deny(bad,bad.size());}}
 for(size_t i:{8,29,31,37}){auto bad=golden;bad[i]=255;rehash(bad);deny(bad,bad.size());}
 in=slots(0,false,{0xc0,0x80},{});B actual=sentinel;assert(atto_bounded_encode_v2(in.data(),in.size(),actual.data(),actual.size())==-1&&actual==sentinel);
 in=slots(0,false,{1},{});auto original=in;assert(atto_bounded_encode_v2(in.data(),in.size(),in.data(),in.size())==-1&&in==original);assert(atto_bounded_encode_v2(in.data(),in.size(),actual.data(),1)==-1&&actual==sentinel);
 in=slots(0,false,B(16384,'a'),B(16384,255));auto n=atto_bounded_encode_v2(in.data(),in.size(),out.data(),out.size());assert(n>0&&atto_bounded_decode_v2(out.data(),n,decoded.data(),decoded.size())==static_cast<int32_t>(in.size())&&std::equal(in.begin(),in.end(),decoded.begin()));
 in=slots(0,false,B(16385,'a'),{});assert(atto_bounded_encode_v2(in.data(),in.size(),out.data(),out.size())==-1);
 std::cout<<"LLVM_BRIDGE_NATIVE_V2_PASS roundtrips=10001 bitflips=472 truncations=59\n";
}
