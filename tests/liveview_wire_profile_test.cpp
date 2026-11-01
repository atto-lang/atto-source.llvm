// Standalone host decoder gate; no Oreslang guest / socket execution.
#include "../src/main/cpp/oreslang/runtime/LiveViewWireProfile.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using oreslang::liveview::Decoder;
using oreslang::liveview::Error;
using oreslang::liveview::Event;
using oreslang::liveview::Kind;
struct Vector {const char* id;const char* hex;int close_code;const char* emitted;};
static const Vector vectors[]={
    {"masked-hello","81850102030469676f686e",0,"text:hello|"},
    {"masked-unicode","818701020304696b23f49e8e93",0,"text:hi 🌐|"},
    {"fragmented-unicode","018201020304f19d8082010203048d92",0,"text:🌐|"},
    {"interleaved-control","0183010203046e6c668981010203042080830102030475756c",0,"ping:21|text:onetwo|"},
    {"ping-and-pong","898201020304696b8a8301020304637b66",0,"ping:6869|pong:627965|"},
    {"masked-close","88820102030402ea",0,"close:03e8|"},
    {"coalesced-text","8181010203044081810102030443",0,"text:A|text:B|"},
    {"unmasked-rejected","810141",1002,""},
    {"rsv-rejected","c18001020304",1002,""},
    {"binary-rejected","828601020304636b6d65737b",1003,""},
    {"invalid-utf8-rejected","818101020304fe",1007,""},
    {"invalid-close-rejected","88820102030402ef",1002,""},
    {"orphan-continuation-rejected","8082010203046f6d",1002,""},
    {"non-minimal-length-rejected","81fe00010102030400",1002,""},
    {"empty-masked-text","818001020304",0,"text:|"},
    {"empty-masked-ping","898001020304",0,"ping:|"},
    {"empty-masked-pong","8a8001020304",0,"pong:|"},
    {"fragmented-euro-utf8","018201020304e380808101020304ad",0,"text:€|"},
    {"valid-close-reason","88850102030402ea617d64",0,"close:03e8627965|"},
    {"fragment-empty-then-text","01800102030480810102030440",0,"text:A|"},
    {"large-masked-control","89fd010203045b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b58595e5b",0,"ping:5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a|"},
    {"unmasked-ping-rejected","8900",1002,""},
    {"reserved-opcode-rejected","83810102030440",1002,""},
    {"fragmented-ping-rejected","09810102030440",1002,""},
    {"fragmented-close-rejected","08820102030402ea",1002,""},
    {"close-one-byte-rejected","88810102030402",1002,""},
    {"close-code-1005-rejected","88820102030402ef",1002,""},
    {"close-code-1006-rejected","88820102030402ec",1002,""},
    {"close-code-1015-rejected","88820102030402f5",1002,""},
    {"close-code-2000-rejected","88820102030406d2",1002,""},
    {"close-code-999-rejected","88820102030402e5",1002,""},
    {"invalid-close-reason-utf8","88830102030402eac0",1007,""},
    {"invalid-text-overlong-utf8","818201020304c182",1007,""},
    {"invalid-text-surrogate-utf8","818301020304eca283",1007,""},
    {"invalid-text-incomplete-utf8","818201020304e380",1007,""},
    {"unexpected-new-text-during-fragment","0181010203044081810102030443",1002,""},
    {"invalid-control-length-126","89fe007e01020304",1002,""},
    {"nonminimal-length-126","81fe007d01020304",1002,""},
    {"nonminimal-length-127","81ff000000000000ffff01020304",1002,""},
    {"high-bit-64bit-length","81ff800000000000000001020304",1002,""},
    {"oversized-64bit-length","81ff000000000001000101020304",1009,""}
};
static std::vector<std::uint8_t> decode_hex(std::string_view h) {
  if(h.size()%2) throw std::runtime_error("odd hex");
  auto nibble=[](char c)->unsigned{
    if(c>='0'&&c<='9')return unsigned(c-'0');
    if(c>='a'&&c<='f')return unsigned(c-'a'+10);
    throw std::runtime_error("invalid hex");
  };
  std::vector<std::uint8_t> out;
  for(std::size_t i=0;i<h.size();i+=2)out.push_back(std::uint8_t((nibble(h[i])<<4)|nibble(h[i+1])));
  return out;
}
static std::string hex(const std::uint8_t* p,std::size_t n) {
  static constexpr char digits[]="0123456789abcdef";
  std::string out;out.reserve(n*2);
  for(std::size_t i=0;i<n;++i){out+=digits[p[i]>>4];out+=digits[p[i]&15];}
  return out;
}
static bool verify(const Vector& v,int mode) {
  const auto raw=decode_hex(v.hex);
  Decoder decoder;
  std::string observed;
  auto consume=[&](Event event) {
    switch(event.kind) {
      case Kind::text:observed+="text:";observed.append(reinterpret_cast<const char*>(event.data),event.size);break;
      case Kind::ping:observed+="ping:"+hex(event.data,event.size);break;
      case Kind::pong:observed+="pong:"+hex(event.data,event.size);break;
      case Kind::close:observed+="close:"+hex(event.data,event.size);break;
    }
    observed+="|";
  };
  Error got=Error::none;std::size_t i=0;unsigned chunks=0;
  static constexpr std::size_t fibonacci[]={1,2,3,5,8,13};
  while(i<raw.size()) {
    const std::size_t limit=mode==0?raw.size():mode==1?1:mode==2?7:fibonacci[chunks%6];
    const std::size_t length=std::min(limit,raw.size()-i);
    got=decoder.feed(raw.data()+i,length,consume);
    if(got!=Error::none)break;
    i+=length;++chunks;
  }
  const bool valid=(int(got)==v.close_code)&&
    (v.close_code!=0 || (!decoder.fragmented()&&observed==v.emitted));
  if(!valid) std::fprintf(stderr,"FAIL %s mode=%d: code=%d expected=%d output=%s\n",
    v.id,mode,int(got),v.close_code,observed.c_str());
  return valid;
}
int main(){
  static_assert(sizeof(vectors)/sizeof(vectors[0])==41);
  int passed=0;
  for(const Vector& v:vectors)for(int mode=0;mode<4;++mode){
    if(!verify(v,mode))return 1;
    ++passed;
  }
  std::printf("LIVEVIEW_CPP_HOST_PASS vectors=41 modes=4 observations=%d\n",passed);
}
