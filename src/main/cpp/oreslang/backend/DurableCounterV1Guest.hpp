#pragma once
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang {
// Restricted, independent frontend island (like GPU-G1), not general
// actor lowering. No comments/directives, strings or other syntax admitted.
inline std::string emit_durable_counter_v1_ir(std::string_view src) {
    auto deny=[](){throw std::invalid_argument("E_DURABLE_COUNTER_ACTOR_ABI");};
    if(src.empty()||src.size()>16384)deny();
    std::vector<std::string_view> tokens;
    auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    auto digit=[](char c){return c>='0'&&c<='9';};
    for(std::size_t i=0;i<src.size();) {
        char c=src[i];
        if(c==' '||c=='\n'||c=='\r'||c=='\t'){++i;continue;}
        auto start=i++;
        if(alpha(c)){while(i<src.size()&&(alpha(src[i])||digit(src[i])))++i;}
        else if(digit(c)){while(i<src.size()&&digit(src[i]))++i;}
        else if(c=='-'&&i<src.size()&&src[i]=='>')++i;
        else if(std::string_view(":;()<>.={}+").find(c)==std::string_view::npos)deny();
        tokens.push_back(src.substr(start,i-start));
        if(tokens.size()>128)deny();
    }
    std::size_t i=0;
    auto peek=[&](){if(i>=tokens.size())deny();return tokens[i];};
    auto take=[&](std::initializer_list<std::string_view> words){
        for(auto w:words){if(peek()!=w)deny();++i;}
    };
    take({"define","durable","actor","Counter","as","state"});
    auto field=peek();
    if(field!="count"&&field!="balance")deny();
    take({field,":","int","=","0",";","receive","(","ActorMail","<","int",">"});
    auto mail=peek();
    if(mail!="mail"&&mail!="event")deny();
    take({mail,")","->","void","{","self",".",field,"=","self",".",field,"+",mail,".","value",";","}","end"});
    if(i!=tokens.size())deny();
    auto fnv=[](std::string_view bytes){
        std::uint64_t h=UINT64_C(0xcbf29ce484222325);
        for(unsigned char b:bytes){h^=b;h*=UINT64_C(0x100000001b3);}
        return h;
    };
    auto le=[](std::string& out,std::uint64_t n,unsigned width){
        for(unsigned j=0;j<width;++j)out.push_back(static_cast<char>(static_cast<unsigned char>(n>>(8*j))));
    };
    std::string sig="atto.actor.state.schema.v1";
    sig.push_back(0);le(sig,7,4);sig+="Counter";le(sig,field.size(),4);sig+=field;sig.push_back(1);
    std::string checkpoint="ATTOST01";
    checkpoint.push_back(1);le(checkpoint,fnv(sig),8);le(checkpoint,1,2);checkpoint.push_back(1);
    le(checkpoint,0,8);le(checkpoint,fnv(checkpoint),8);
    std::string d;
    d.push_back(1);d.push_back(7);d+="Counter";d.push_back(static_cast<char>(field.size()));
    d+=field;d.push_back(1);d.push_back(36);d+=checkpoint;
    const auto n=std::to_string(d.size());
    std::string ir="@atto_durable_descriptor = private constant ["+n+" x i8] [";
    for(std::size_t j=0;j<d.size();++j){
        if(j)ir+=", ";
        ir+="i8 "+std::to_string(static_cast<unsigned char>(d[j]));
    }
    ir+="]\n";
    ir+="define i64 @atto_durable_state_size_v1() { ret i64 "+n+" }\n";
    ir+=R"IR(define ptr @atto_durable_state_v1() { ret ptr @atto_durable_descriptor }
define i64 @atto_durable_transition_v1(i64 %s, i64 %op, i64 %d) {
entry:
  %low = icmp slt i64 %s, 0
  %high = icmp sgt i64 %s, 1000000000
  %outside = or i1 %low, %high
  br i1 %outside, label %reject, label %dispatch
dispatch:
  switch i64 %op, label %reject [ i64 1, label %increment
                                  i64 2, label %read ]
read:
  %zero = icmp eq i64 %d, 0
  br i1 %zero, label %readok, label %reject
readok:
  ret i64 %s
increment:
  %nonpos = icmp sle i64 %d, 0
  %large = icmp sgt i64 %d, 1000000000
  %bad = or i1 %nonpos, %large
  br i1 %bad, label %reject, label %bounds
bounds:
  %remaining = sub i64 1000000000, %s
  %overflow = icmp sgt i64 %d, %remaining
  br i1 %overflow, label %reject, label %add
add:
  %next = add i64 %s, %d
  ret i64 %next
reject:
  ret i64 -1
}
)IR";
    return ir;
}
} // namespace oreslang
