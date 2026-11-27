#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <set>

namespace oreslang {
// Bounded schema-only frontend and direct LLVM lowering. No live actors, IO,
// reflection or guest-selected storage authority. Trusted caller owns buffers.
inline std::string emit_durable_scalar_v1_ir(std::string_view source) {
    const auto deny=[](){throw std::invalid_argument("E_DURABLE_SCALAR_SCHEMA_V1");};
    const auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    const auto digit=[](char c){return c>='0'&&c<='9';};
    if(source.size()>16384)deny();
    std::vector<std::string_view> ts;
    for(std::size_t i=0;i<source.size();){
        char c=source[i];if(c==' '||c=='\t'||c=='\r'||c=='\n'){++i;continue;}
        auto start=i++;
        if(alpha(c)){while(i<source.size()&&(alpha(source[i])||digit(source[i])))++i;}
        else if(digit(c)){while(i<source.size()&&digit(source[i]))++i;}
        else if(std::string_view(":;=-").find(c)==std::string_view::npos)deny();
        if(ts.size()>=4096)deny();
        ts.push_back(source.substr(start,i-start));
    }
    std::size_t i=0;
    auto peek=[&](){return i<ts.size()?ts[i]:std::string_view{};};
    auto next=[&](){auto t=peek();if(t.empty())deny();++i;return t;};
    auto take=[&](std::string_view t){if(next()!=t)deny();};
    const std::string_view reserved=" define pub private durable persistent actor as state end int bool true false self receive init fnc mtd let val mut const return await suspends susp async critical using with import namespace module class struct contract enum new spawn stop if else match on default void ";
    auto name=[&](){auto n=next();if(n.empty()||n.size()>128||!alpha(n.front())||reserved.find(" "+std::string(n)+" ")!=std::string_view::npos)deny();return n;};
    auto le=[](std::string& b,std::uint64_t n,unsigned w){for(unsigned j=0;j<w;++j)b.push_back(static_cast<char>(static_cast<unsigned char>(n>>(8*j))));};
    auto hash=[](std::string_view b){std::uint64_t h=UINT64_C(0xcbf29ce484222325);for(unsigned char x:b){h^=x;h*=UINT64_C(0x100000001b3);}return h;};
    std::string sig="atto.actor.state.schema.v1";sig.push_back(0);
    auto sized=[&](std::string_view n){le(sig,n.size(),4);sig+=n;};
    take("define");take("durable");take("actor");sized(name());take("as");
    std::set<std::string_view> seen;
    struct Field {unsigned tag;std::int64_t value;};std::vector<Field> fields;
    while(peek()=="state"){
        if(fields.size()>=256)deny();
        take("state");auto n=name();if(!seen.insert(n).second)deny();sized(n);take(":");auto type=next();take("=");
        if(type=="int"){
            bool negative=peek()=="-";if(negative)next();auto lit=next();
            if(lit.empty()||lit.size()>19||!digit(lit.front())||(lit.size()>1&&lit.front()=='0'))deny();
            std::string s=(negative?"-":"")+std::string(lit);std::int64_t value=0;
            auto r=std::from_chars(s.data(),s.data()+s.size(),value);if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size())deny();
            fields.push_back({1,value});sig.push_back(1);
        }else if(type=="bool"){
            auto b=next();if(b!="true"&&b!="false")deny();fields.push_back({2,b=="true"?1:0});sig.push_back(2);
        }else deny();take(";");
    }
    take("end");if(!peek().empty())deny();
    std::string initial="ATTOST01";initial.push_back(1);le(initial,hash(sig),8);le(initial,fields.size(),2);
    for(auto f:fields){initial.push_back(static_cast<char>(f.tag));le(initial,static_cast<std::uint64_t>(f.value),f.tag==1?8:1);}
    le(initial,hash(initial),8);
    const auto size=std::to_string(initial.size()),end=std::to_string(initial.size()-8),count=std::to_string(fields.size());
    std::string ir="@initial = private constant ["+size+" x i8] [";
    for(std::size_t j=0;j<initial.size();++j){if(j)ir+=", ";ir+="i8 "+std::to_string(static_cast<unsigned char>(initial[j]));}ir+="]\n";
    ir+=R"IR(define internal void @copy(ptr %dst, ptr %src, i64 %n) {
entry: br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp eq i64 %i, %n
  br i1 %done, label %exit, label %body
body:
  %s = getelementptr i8, ptr %src, i64 %i
  %d = getelementptr i8, ptr %dst, i64 %i
  %x = load i8, ptr %s, align 1
  store i8 %x, ptr %d, align 1
  %next = add i64 %i, 1
  br label %loop
exit: ret void
}
define internal i64 @hash(ptr %b, i64 %n) {
entry: br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %h = phi i64 [-3750763034362895579, %entry], [%mul, %body]
  %done = icmp eq i64 %i, %n
  br i1 %done, label %exit, label %body
body:
  %p = getelementptr i8, ptr %b, i64 %i
  %x = load i8, ptr %p, align 1
  %u = zext i8 %x to i64
  %xor = xor i64 %h, %u
  %mul = mul i64 %xor, 1099511628211
  %next = add i64 %i, 1
  br label %loop
exit: ret i64 %h
}
define internal void @put64(ptr %p, i64 %v) {
entry: br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %done = icmp eq i64 %i, 8
  br i1 %done, label %exit, label %body
body:
  %shift = mul i64 %i, 8
  %r = lshr i64 %v, %shift
  %x = trunc i64 %r to i8
  %at = getelementptr i8, ptr %p, i64 %i
  store i8 %x, ptr %at, align 1
  %next = add i64 %i, 1
  br label %loop
exit: ret void
}
define internal i64 @get64(ptr %p) {
entry: br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %body]
  %v = phi i64 [0, %entry], [%combined, %body]
  %done = icmp eq i64 %i, 8
  br i1 %done, label %exit, label %body
body:
  %at = getelementptr i8, ptr %p, i64 %i
  %x = load i8, ptr %at, align 1
  %u = zext i8 %x to i64
  %shift = mul i64 %i, 8
  %r = shl i64 %u, %shift
  %combined = or i64 %v, %r
  %next = add i64 %i, 1
  br label %loop
exit: ret i64 %v
}
)IR";
    ir+="define i64 @atto_scalar_size_v1() { ret i64 "+size+" }\ndefine i64 @atto_scalar_fields_v1() { ret i64 "+count+" }\n";
    // All buffers are trusted, valid, caller-owned ranges; no concurrency.
    // Scratch staging permits aliasing and prevents partial output on reject.
    ir+="define i64 @atto_scalar_initial_v1(ptr %out, i64 %cap) {\nentry:\n  %null = icmp eq ptr %out, null\n  %short = icmp slt i64 %cap, "+size+"\n  %bad = or i1 %null, %short\n  br i1 %bad, label %reject, label %ok\nok:\n  call void @copy(ptr %out, ptr @initial, i64 "+size+")\n  ret i64 "+size+"\nreject: ret i64 -1\n}\n";
    ir+="define i64 @atto_scalar_encode_v1(ptr %values, i64 %count, ptr %out, i64 %cap) {\nentry:\n  %scratch = alloca ["+size+" x i8], align 8\n  %null = icmp eq ptr %out, null\n  %short = icmp slt i64 %cap, "+size+"\n  %countbad = icmp ne i64 %count, "+count+"\n  %nullvalues = icmp eq ptr %values, null\n  %bad0 = or i1 %null, %short\n  %bad1 = or i1 %countbad, %nullvalues\n  %bad = or i1 %bad0, %bad1\n  br i1 %bad, label %reject, label %begin\nbegin:\n  call void @copy(ptr %scratch, ptr @initial, i64 "+size+")\n";
    std::size_t pos=19;
    for(std::size_t j=0;j<fields.size();++j){auto n=std::to_string(j),at=std::to_string(pos+1);auto f=fields[j];
        ir+="  %src"+n+" = getelementptr i64, ptr %values, i64 "+n+"\n  %v"+n+" = load i64, ptr %src"+n+", align 1\n  %dst"+n+" = getelementptr i8, ptr %scratch, i64 "+at+"\n";
        if(f.tag==1)ir+="  call void @put64(ptr %dst"+n+", i64 %v"+n+")\n";
        else ir+="  %bool"+n+" = icmp ule i64 %v"+n+", 1\n  br i1 %bool"+n+", label %boolok"+n+", label %reject\nboolok"+n+":\n  %byte"+n+" = trunc i64 %v"+n+" to i8\n  store i8 %byte"+n+", ptr %dst"+n+", align 1\n";
        pos+=f.tag==1?9:2;
    }
    ir+="  %checksum = call i64 @hash(ptr %scratch, i64 "+end+")\n  %tail = getelementptr i8, ptr %scratch, i64 "+end+"\n  call void @put64(ptr %tail, i64 %checksum)\n  call void @copy(ptr %out, ptr %scratch, i64 "+size+")\n  ret i64 "+size+"\nreject: ret i64 -1\n}\n";
    ir+="define i64 @atto_scalar_decode_v1(ptr %input, i64 %len, ptr %out, i64 %cap) {\nentry:\n  %scratch = alloca ["+size+" x i8], align 8\n  %null = icmp eq ptr %input, null\n  %nullout = icmp eq ptr %out, null\n  %lenbad = icmp ne i64 %len, "+size+"\n  %short = icmp slt i64 %cap, "+count+"\n  %bad0 = or i1 %null, %nullout\n  %bad1 = or i1 %lenbad, %short\n  %bad = or i1 %bad0, %bad1\n  br i1 %bad, label %reject, label %begin\nbegin:\n  call void @copy(ptr %scratch, ptr %input, i64 "+size+")\n  %checksum = call i64 @hash(ptr %scratch, i64 "+end+")\n  %tail = getelementptr i8, ptr %scratch, i64 "+end+"\n  %stored = call i64 @get64(ptr %tail)\n  %valid = icmp eq i64 %stored, %checksum\n  br i1 %valid, label %head0, label %reject\n";
    for(unsigned j=0;j<19;++j){auto n=std::to_string(j);ir+="head"+n+":\n  %hp"+n+" = getelementptr i8, ptr %scratch, i64 "+n+"\n  %hb"+n+" = load i8, ptr %hp"+n+", align 1\n  %hc"+n+" = icmp eq i8 %hb"+n+", "+std::to_string(static_cast<unsigned char>(initial[j]))+"\n  br i1 %hc"+n+", label %head"+std::to_string(j+1)+", label %reject\n";}
    ir+="head19:\n";pos=19;
    for(std::size_t j=0;j<fields.size();++j){auto n=std::to_string(j);auto f=fields[j];
        ir+="  %tagp"+n+" = getelementptr i8, ptr %scratch, i64 "+std::to_string(pos)+"\n  %tag"+n+" = load i8, ptr %tagp"+n+", align 1\n  %tagok"+n+" = icmp eq i8 %tag"+n+", "+std::to_string(f.tag)+"\n  br i1 %tagok"+n+", label %read"+n+", label %reject\nread"+n+":\n  %vp"+n+" = getelementptr i8, ptr %scratch, i64 "+std::to_string(pos+1)+"\n";
        if(f.tag==1)ir+="  %v"+n+" = call i64 @get64(ptr %vp"+n+")\n";
        else ir+="  %bv"+n+" = load i8, ptr %vp"+n+", align 1\n  %bc"+n+" = icmp ule i8 %bv"+n+", 1\n  br i1 %bc"+n+", label %boolok"+n+", label %reject\nboolok"+n+":\n  %v"+n+" = zext i8 %bv"+n+" to i64\n";
        pos+=f.tag==1?9:2;
    }
    for(std::size_t j=0;j<fields.size();++j){auto n=std::to_string(j);ir+="  %dst"+n+" = getelementptr i64, ptr %out, i64 "+n+"\n  store i64 %v"+n+", ptr %dst"+n+", align 1\n";}
    ir+="  ret i64 "+count+"\nreject: ret i64 -1\n}\n";
    return ir;
}
} // namespace oreslang
