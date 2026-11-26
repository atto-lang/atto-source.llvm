#pragma once
// Explicit bounded ready-only source to static C++ AOT profile.
// No raw source identifiers reach emitted code; no general scheduler ABI.
#include <charconv>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace oreslang::counted_ready {
class Compiler {
    struct Value { std::string type, code; };
    struct Slot { std::string type, code; int cap=0; bool moved=false, mut=false; };
    std::vector<std::string> tokens;
    std::map<std::string,Slot> slots;
    std::size_t pos=0,budget=4096,depth=0,sites=0;
    std::string pending;
    bool pure=false;
    [[noreturn]] static void bad(std::string s){throw std::runtime_error("E_COUNTED_READY_PROFILE: "+s);}
    static bool alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
    static bool digit(char c){return c>='0'&&c<='9';}
    const std::string& peek()const{return tokens.at(pos);}
    bool eat(std::string_view s){if(peek()!=s)return false;++pos;return true;}
    void need(std::string_view s){if(!eat(s))bad("expected "+std::string(s));}
    std::string name(){
        static const std::set<std::string> reserved={"pub","suspends","fnc","main","int","bool","val","let","const","Channel","Future","Array","new","writech","readch","select","nb","await","when","fair","first","random","default","over","end","return","if","then","fi","true","false"};
        auto n=peek();if(n.empty()||!alpha(n[0])||reserved.count(n))bad("lexical name required");
        for(char c:n)if(!alpha(c)&&!digit(c))bad("lexical name required");
        ++pos;return n;
    }
    Slot& get(const std::string& n,std::string_view t){auto i=slots.find(n);if(i==slots.end()||i->second.moved||i->second.type!=t)bad("expected live lexical "+std::string(t));return i->second;}
    std::int64_t literal(){auto t=peek();std::int64_t v=0;auto p=std::from_chars(t.data(),t.data()+t.size(),v);if(t.empty()||!digit(t[0])||p.ec!=std::errc{}||p.ptr!=t.data()+t.size())bad("integer literal");++pos;return v;}
    int small(int max){auto n=literal();if(n>max)bad("literal limit");return static_cast<int>(n);}
    static std::string check(const Value& v,std::string_view t){if(v.type!=t)bad("expected "+std::string(t));return v.code;}
    std::string type(){
        if(eat("int"))return "int";
        if(eat("Channel")){need("<");need("int");need(">");return "channel";}
        if(eat("Future")){need("<");if(eat("Array")){need("<");need("int");need(">");need(">");return "futureArray";}need("int");need(">");return "future";}
        if(eat("Array")){bool b=eat("[");if(!b)need("<");need("int");need(b?"]":">");return "array";}
        bad("unsupported type");
    }
    static int prec(const std::string& s){if(s=="||")return 1;if(s=="&&")return 2;if(s=="=="||s=="!=")return 3;if(s=="<"||s==">"||s=="<="||s==">=")return 4;if(s=="+"||s=="-")return 5;if(s=="*")return 6;return -1;}
    Value expr(int min=0,std::string binder=""){
        if(!budget--||++depth>64)bad("expression budget");
        auto lhs=primary(binder);
        while(prec(peek())>=min){
            auto op=peek();++pos;auto rhs=expr(prec(op)+1,binder);
            if(op=="&&"||op=="||")lhs={"bool","(("+check(lhs,"bool")+")"+op+"("+check(rhs,"bool")+"))"};
            else{
                auto a=check(lhs,"int"),b=check(rhs,"int");
                auto calc=(op=="+"?"add(a,b)":op=="-"?"sub(a,b)":op=="*"?"mul(a,b)":"a"+op+"b");
                lhs={op=="+"||op=="-"||op=="*"?"int":"bool","([&](){auto a="+a+";auto b="+b+";return "+calc+";}())"};
            }
        }--depth;return lhs;
    }
    Value primary(const std::string& binder){
        if(eat("(")){auto v=expr(0,binder);need(")");return v;}
        if(eat("-"))return {"int","neg("+check(expr(7,binder),"int")+")"};
        if(!peek().empty()&&digit(peek()[0]))return {"int","std::int64_t{"+std::to_string(literal())+"}"};
        if(!binder.empty()){auto n=name();if(binder=="@count")return {"int",get(n,"int").code};if(n!=binder)bad("projection binding only");return {"int","x"};}
        if(eat("await")){
            if(pure)bad("effect in condition");
            bool nb=eat("nb");if(eat("select"))return counted(true);if(nb)bad("nb requires select");
            auto n=name();auto i=slots.find(n);
            if(i!=slots.end()&&i->second.type=="futureArray"&&!i->second.moved&&pending==i->second.code){i->second.moved=true;pending.clear();return {"array","finish("+i->second.code+")"};}
            if(!pending.empty())bad("await batch first");
            auto& f=get(n,"future");f.moved=true;return {"int","observe("+f.code+")"};
        }
        if(eat("readch")){if(pure||!pending.empty())bad("channel access not admitted");return {"int","read("+get(name(),"channel").code+")"};}
        auto n=name();auto i=slots.find(n);if(i==slots.end()||i->second.moved)bad("unknown/moved binding");auto& s=i->second;
        if(s.type=="int")return {"int",s.code};
        if(s.type=="array"){if(eat("[")){auto idx=small(15);need("]");return {"int","at("+s.code+","+std::to_string(idx)+")"};}need(".");need("size");return {"int","static_cast<std::int64_t>("+s.code+".size())"};}
        bad("capability escape");
    }
    static std::string join(const std::vector<std::string>& v){std::string r;for(auto& s:v){if(!r.empty())r+=",";r+=s;}return r;}
    Value counted(bool direct){
        if(!pending.empty())bad("await batch first");if(eat("<")){need("int");need(">");}
        need("(");auto begin=pos;auto n=check(expr(0,"@count"),"int");auto end=pos;need(")");
        bool zero=false;
        if(end==begin+1&&digit(tokens[begin][0])){
            std::int64_t constant=0;const auto& tok=tokens[begin];
            auto parsed=std::from_chars(tok.data(),tok.data()+tok.size(),constant);
            if(parsed.ec!=std::errc{}||constant>16)bad("SelectCountInvalid");
            zero=constant==0;
        }
        if(end==begin+2&&tokens[begin]=="-"&&digit(tokens[begin+1][0]))bad("SelectCountInvalid");
        bool fair=eat("fair");if(!fair)eat("first");bool over=eat("over");if(!over)need("{");
        std::vector<std::string> sources,projects;std::set<std::string> futures;
        while(eat("when")){
            if(sources.size()==8)bad("arm quota");std::string kind=eat("readch")?"channel":eat("await")?"future":"";if(kind.empty())bad("read/Future arm required");
            auto nm=name();auto& s=get(nm,kind);if(kind=="future")futures.insert(nm);
            need(":");need("int");auto b=name();need("->");need("{");need("return");
            projects.push_back("[](std::int64_t x){return "+check(expr(0,b),"int")+";}");need(";");need("}");sources.push_back(s.code);
        }
        if(sources.empty())bad("at least one arm required");need(over?"end":"}");++sites;
        if(!zero)for(auto& f:futures)slots.at(f).moved=true;
        auto plan=zero?"[](){return Plan{};}":"[&](){return Plan{{"+join(sources)+"},{"+join(projects)+"}};}";
        auto start="start("+n+","+plan+","+(fair?"true":"false")+")";
        return direct?Value{"array","finish("+start+")"}:Value{"futureArray",start};
    }
    std::string bind(bool mut){
        auto t=type(),n=name();if(slots.count(n)||slots.size()>=128||(mut&&t!="int"))bad("invalid binding");need("=");std::string code;int cap=0;
        if(t=="channel"){if(!pending.empty())bad("await batch first");need("Channel");need(".");need("new");need("<");need("int");need(">");need("(");cap=small(64);if(!cap)bad("zero capacity");need(")");code="std::make_shared<Channel>()";}
        else if(t=="future"){if(!pending.empty())bad("await batch first");need("select");need("<");need("int");need(">");bool over=eat("over");if(!over)need("{");need("default");need(":");need("->");need("{");need("return");code="ready([](){return "+check(expr(0,"$none"),"int")+";})";need(";");need("}");need(over?"end":"}");}
        else if(t=="futureArray"){eat("nb");need("select");code=check(counted(false),t);}
        else code=check(expr(),t);
        need(";");auto slot="v"+std::to_string(slots.size());slots.emplace(n,Slot{t,slot,cap,false,mut});if(t=="futureArray")pending=slot;
        return "auto "+slot+"="+code+";\n";
    }
public:
    explicit Compiler(std::string_view source){
        if(source.size()>65536)bad("source limit");
        for(char c:source)if(static_cast<unsigned char>(c)>127||(c<32&&c!='\t'&&c!='\r'&&c!='\n')||c==127)bad("unsupported source character");
        for(std::size_t i=0;i<source.size();){
            char c=source[i];if(c==' '||c=='\t'||c=='\r'||c=='\n'){++i;continue;}
            if(c=='/'&&i+1<source.size()&&source[i+1]=='/'){while(i<source.size()&&source[i]!='\n')++i;continue;}
            auto start=i++;
            if(alpha(c)){while(i<source.size()&&(alpha(source[i])||digit(source[i])))++i;}
            else if(digit(c)){while(i<source.size()&&digit(source[i]))++i;}
            else if(i<source.size()&&((c=='-'&&source[i]=='>')||((c=='='||c=='!'||c=='<'||c=='>')&&source[i]=='=')||(c=='&'&&source[i]=='&')||(c=='|'&&source[i]=='|')))++i;
            else if(std::string_view("(){}[]:;,+-*<>=.").find(c)==std::string_view::npos)bad("unsupported token");
            if(i-start>128||tokens.size()>=8192)bad("token limit");tokens.emplace_back(source.substr(start,i-start));
        }tokens.emplace_back("<eof>");
    }
    std::string parse(){
        eat("pub");need("suspends");need("fnc");need("main");need("(");need(")");need(":");need("int");need("{");
        std::string body;int statements=0;
        while(peek()!="return"){
            if(++statements>256)bad("statement quota");
            if(eat("val"))body+=bind(false);else if(eat("let"))body+=bind(true);
            else if(eat("writech")){if(!pending.empty())bad("await batch first");auto& s=get(name(),"channel");need(",");auto v=check(expr(),"int");need(";");body+="write("+s.code+","+std::to_string(s.cap)+","+v+");\n";}
            else if(eat("if")){if(!pending.empty())bad("await batch first");pure=true;auto condition=check(expr(),"bool");pure=false;auto before=slots;need("then");need("return");auto v=check(expr(),"int");need(";");need("fi");slots=std::move(before);body+="if("+condition+")return "+v+";\n";}
            else if(pos+1<tokens.size()&&tokens[pos+1]=="="){auto& s=get(name(),"int");if(!s.mut)bad("immutable binding");need("=");auto v=check(expr(),"int");need(";");body+=s.code+"="+v+";\n";}
            else bad("unsupported statement");
        }
        need("return");body+="return "+check(expr(),"int")+";\n";need(";");need("}");if(peek()!="<eof>"||!sites||!pending.empty())bad("one closed-world counted main with no escaped batch required");return body;
    }
};
inline std::string compile(std::string_view source){
    auto body=Compiler(source).parse();
    return R"ATTO(#include "oreslang/runtime/NativeCountedI64Cursor.hpp"
#include <iostream>
using Cursor=oreslang::runtime::NativeCountedI64Cursor<64,64,8>;
using Channel=Cursor::Channel;
using Future=Cursor::Future;
using I=std::int64_t;
[[maybe_unused]] static I add(I a,I b){I r;if(__builtin_add_overflow(a,b,&r))throw std::overflow_error("E_I64_OVERFLOW");return r;}
[[maybe_unused]] static I sub(I a,I b){I r;if(__builtin_sub_overflow(a,b,&r))throw std::overflow_error("E_I64_OVERFLOW");return r;}
[[maybe_unused]] static I mul(I a,I b){I r;if(__builtin_mul_overflow(a,b,&r))throw std::overflow_error("E_I64_OVERFLOW");return r;}
[[maybe_unused]] static I neg(I a){return sub(0,a);}
[[maybe_unused]] static void write(const std::shared_ptr<Channel>& c,std::size_t cap,I v){if(c->size()>=cap||!c->try_write(v))throw std::runtime_error("E_READY_ONLY: write would suspend");}
[[maybe_unused]] static I read(const std::shared_ptr<Channel>& c){auto r=c->try_read();if(!r)throw std::runtime_error("E_READY_ONLY: read would suspend");return *r;}
[[maybe_unused]] static I at(const std::vector<I>& a,std::size_t i){return a.at(i);}
template<class F> static auto ready(F p){auto f=std::make_shared<Future>();try{f->resolve(p());}catch(...){f->fail(std::current_exception());}return f;}
[[maybe_unused]] static I observe(const std::shared_ptr<Future>& f){auto token=f->subscribe_ready_advisory([](const auto&){});struct Guard{Future::Subscription t;~Guard(){t.close();}} guard{token};Future::Result r;if(!f->try_commit(token,r))throw std::runtime_error("E_READY_ONLY: Future would suspend");if(r.status!=Future::Status::Value){if(r.failure)std::rethrow_exception(r.failure);throw std::runtime_error("Future failed");}return r.value;}
struct Plan{std::vector<Cursor::Source> sources;std::vector<Cursor::Projection> projections;};
struct Pending{std::unique_ptr<Cursor> cursor;std::size_t limit;};
template<class F> static Pending start(I n,F p,bool fair){if(n<0||n>16)throw std::range_error("SelectCountInvalid");auto plan=n?p():Plan{};return {std::make_unique<Cursor>(static_cast<std::size_t>(n),plan.sources,fair,plan.projections),2*(static_cast<std::size_t>(n)+plan.sources.size())+2};}
static std::vector<I> finish(Pending& pending){auto owned=std::move(pending.cursor);if(!owned)throw std::logic_error("counted batch already consumed");auto& c=*owned;for(std::size_t i=0;i<pending.limit;++i){switch(c.state()){case Cursor::State::Success:return c.result();case Cursor::State::Failed:std::rethrow_exception(c.failure());case Cursor::State::Ready:c.begin_round();break;case Cursor::State::Waiting:if(!c.pump())throw std::runtime_error("E_READY_ONLY: selection would suspend");break;default:throw std::runtime_error("cancelled");}}throw std::runtime_error("E_READY_ONLY: pump budget");}
[[maybe_unused]] static std::vector<I> finish(Pending&& p){return finish(p);}
static I guest(){
)ATTO"+body+R"ATTO(
}
int main(){try{auto v=guest();std::cout<<v<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
)ATTO";
}
}
