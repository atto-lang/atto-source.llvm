// C++20 standalone HOST serializer/digest test; not an Oreslang guest runtime.
// OpenSSL is used only by this source-free test harness.
#include <openssl/evp.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

static void validate_utf8(std::string_view s) {
  for(size_t i=0;i<s.size();) {
    auto b=static_cast<unsigned char>(s[i]);
    if(b<0x80){++i;continue;}
    int n=b>=0xC2&&b<=0xDF?2:b>=0xE0&&b<=0xEF?3:b>=0xF0&&b<=0xF4?4:0;
    if(!n||s.size()-i<static_cast<size_t>(n))throw std::runtime_error("invalid UTF-8");
    auto c1=static_cast<unsigned char>(s[i+1]);
    if(c1<0x80||c1>0xBF)throw std::runtime_error("bad UTF-8 continuation");
    if(n>=3){
      if((b==0xE0&&c1<0xA0)||(b==0xED&&c1>=0xA0)||
         (b==0xF0&&c1<0x90)||(b==0xF4&&c1>0x8F))
        throw std::runtime_error("invalid UTF-8 range");
      auto c2=static_cast<unsigned char>(s[i+2]);
      if(c2<0x80||c2>0xBF)throw std::runtime_error("bad UTF-8 continuation");
    }
    if(n==4){auto c3=static_cast<unsigned char>(s[i+3]);
      if(c3<0x80||c3>0xBF)throw std::runtime_error("bad UTF-8 continuation");}
    i+=n;
  }
}
static std::string base64(std::string_view encoded) {
  constexpr std::string_view abc="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(encoded.size()%4!=0)throw std::runtime_error("bad base64 length");
  std::string out;unsigned val=0;int bits=-8;size_t pad=0;
  for(size_t i=0;i<encoded.size();i++) {
    char ch=encoded[i];
    if(ch=='='){
      if(i<encoded.size()-2)throw std::runtime_error("bad base64 padding");
      ++pad;continue;
    }
    if(pad)throw std::runtime_error("nonterminal base64 padding");
    auto idx=abc.find(ch);
    if(idx==std::string_view::npos)throw std::runtime_error("bad base64 alphabet");
    val=(val<<6)|static_cast<unsigned>(idx);bits+=6;
    if(bits>=0){out+=static_cast<char>((val>>bits)&255);bits-=8;}
  }
  if(pad>2)throw std::runtime_error("bad base64 padding count");
  // Roundtrip ensures canonical encoding, including zero trailing pad bits.
  const size_t encoded_length=4*((out.size()+2)/3);
  // EVP_EncodeBlock also writes a trailing NUL: reserve that extra byte.
  std::string recoded(encoded_length+1,'\0');
  if(!out.empty()&&EVP_EncodeBlock(reinterpret_cast<unsigned char*>(recoded.data()),
      reinterpret_cast<const unsigned char*>(out.data()),static_cast<int>(out.size()))<0)
    throw std::runtime_error("base64 encode failed");
  recoded.resize(encoded_length);
  if(recoded!=encoded)throw std::runtime_error("noncanonical base64");
  validate_utf8(out);
  return out;
}
static std::string quote(std::string_view s) {
  constexpr char hexdigits[]="0123456789abcdef";
  std::string out="\"";
  for(unsigned char c:s) {
    switch(c) {
      case '"':out+="\\\"";break;
      case '\\':out+="\\\\";break;
      case '\b':out+="\\b";break;
      case '\f':out+="\\f";break;
      case '\n':out+="\\n";break;
      case '\r':out+="\\r";break;
      case '\t':out+="\\t";break;
      default: if(c<32){out+="\\u00";out+=hexdigits[c>>4];out+=hexdigits[c&15];}
      else out+=static_cast<char>(c);
    }
  }
  return out+'"';
}
static std::string digest(std::string_view user,std::string_view event,uint64_t rev,std::string_view canonical) {
  if(rev>9007199254740991ULL)throw std::runtime_error("unsafe revision");
  validate_utf8(user);validate_utf8(event);validate_utf8(canonical);
  auto envelope="["+quote(user)+","+quote(event)+","+std::to_string(rev)+","+quote(canonical)+"]";
  std::array<unsigned char,EVP_MAX_MD_SIZE> hash{}; unsigned int size=0;
  if(EVP_Digest(envelope.data(),envelope.size(),hash.data(),&size,EVP_sha256(),nullptr)!=1||size!=32)
    throw std::runtime_error("sha256 failed");
  std::ostringstream out;
  for(unsigned i=0;i<size;i++)out<<std::hex<<std::setfill('0')<<std::setw(2)<<static_cast<unsigned>(hash[i]);
  return out.str();
}
static std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> fields;size_t pos=0;
  for(;;){size_t end=line.find('\t',pos);fields.emplace_back(line.substr(pos,end==std::string_view::npos?end:end-pos));
    if(end==std::string_view::npos)break;
    pos=end+1;
  }
  return fields;
}
int main(int argc,char**argv) {
  try {
    if(argc!=2)throw std::runtime_error("expected vector file");
    std::ifstream input(argv[1]);if(!input)throw std::runtime_error("cannot read vectors");
    int count=0;std::string line;
    while(std::getline(input,line)) {
      if(line.empty()||line[0]=='#')continue;
      auto f=split(line);if(f.size()!=6)throw std::runtime_error("malformed TSV row");
      const auto actual=digest(base64(f[1]),base64(f[2]),std::stoull(f[3]),base64(f[4]));
      if(actual!=f[5])throw std::runtime_error("mismatched vector "+f[0]+" "+actual);
      count++;
    }
    if(count!=8)throw std::runtime_error("expected exactly 8 vectors");
    bool rejected=false;try{digest("user","event",9007199254740992ULL,"{}");}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("unsafe revision accepted");
    for(auto bad : {std::string("\xC0\x80",2),std::string("\xED\xA0\x80",3),
      std::string("\xF4\x90\x80\x80",4),std::string("\xF0\x9F",2)}){
      bool denied=false;try{digest(bad,"event",0,"{}");}catch(const std::runtime_error&){denied=true;}
      if(!denied)throw std::runtime_error("invalid Unicode accepted");
    }
    for(auto bad : {"a", "YQ=", "YQ=!", "YQ===", "YQ=A", "YR=="}){
      bool denied=false;try{base64(bad);}catch(const std::runtime_error&){denied=true;}
      if(!denied)throw std::runtime_error("invalid base64 accepted");
    }
    std::cout<<"C++ HOST LiveView fingerprint: "<<count<<"/8 vectors passed; 11 negative checks passed\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
