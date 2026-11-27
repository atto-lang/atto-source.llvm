#pragma once
// Candidate closed scalar MIR to LLVM IR. No parser or runtime authority.
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <utility>
namespace oreslang::backend::named_recursive {
enum class Op { literal, parameter, self, add, sub, mul };
struct Node { Op op; std::size_t left=0,right=0; std::int64_t value=0; };
struct Arm { std::optional<std::int64_t> key; std::size_t body; };
struct Stage { std::vector<Node> nodes; std::vector<Arm> arms; };
inline bool valid(const Stage& s) {
    if(s.nodes.empty()||s.nodes.size()>128||s.arms.empty()||s.arms.size()>16)return false;
    std::vector<unsigned> depths(s.nodes.size(),1);
    for(std::size_t i=0;i<s.nodes.size();++i){
        const auto& n=s.nodes[i];
        switch(n.op){
        case Op::literal: if(n.left||n.right)return false;break;
        case Op::parameter: if(n.left||n.right||n.value)return false;break;
        case Op::self:
            if(n.left>=i||n.right||n.value)return false;
            depths[i]=depths[n.left]+1;break;
        case Op::add:case Op::sub:case Op::mul:
            if(n.left>=i||n.right>=i||n.value)return false;
            depths[i]=1+(depths[n.left]>depths[n.right]?depths[n.left]:depths[n.right]);break;
        default:return false;
        }
        if(depths[i]>16)return false;
    }
    std::set<std::int64_t> keys;
    for(std::size_t i=0;i<s.arms.size();++i){
        const auto& a=s.arms[i];
        if(a.body>=s.nodes.size())return false;
        if(a.key){if(!keys.insert(*a.key).second)return false;}
        else if(i+1!=s.arms.size())return false;
    }
    return !s.arms.back().key;
}
// ABI: i32 status (0 success, 1 overflow, 6 depth), output written only
// on success. Each native recursive invocation has its own depth and scratch.
// No dynamic allocation, shared global depth, or host callback.
inline std::optional<std::string> emit(const Stage& s){
    if(!valid(s))return {};
    std::string ir="declare {i64,i1} @llvm.sadd.with.overflow.i64(i64,i64)\n"
        "declare {i64,i1} @llvm.ssub.with.overflow.i64(i64,i64)\n"
        "declare {i64,i1} @llvm.smul.with.overflow.i64(i64,i64)\n"
        "define internal i32 @stage(i64 %arg, i32 %depth, ptr %out) {\nentry:\n"
        "  %scratch = alloca i64, align 8\n  %limit = icmp ugt i32 %depth, 1024\n"
        "  br i1 %limit, label %stack, label %dispatch\nstack:\n  ret i32 6\n"
        "overflow:\n  ret i32 1\ndispatch:\n";
    unsigned serial=0,work=0;
    bool exhausted=false;
    auto fresh=[&](){return std::string("v")+std::to_string(serial++);};
    auto line=[&](const std::string& x){ir+="  "+x+"\n";};
    auto expr=[&](auto&& visit,std::size_t id)->std::string{
        if(++work>2048){exhausted=true;return "0";}
        const auto& n=s.nodes[id];
        if(n.op==Op::literal)return std::to_string(n.value);
        if(n.op==Op::parameter)return "%arg";
        auto a=visit(visit,n.left);
        if(exhausted)return "0";
        if(n.op==Op::self){
            auto d=fresh(),status=fresh(),ok=fresh(),pass=fresh(),fail=fresh(),value=fresh();
            line("%"+d+" = add i32 %depth, 1");
            line("%"+status+" = call i32 @stage(i64 "+a+", i32 %"+d+", ptr %scratch)");
            line("%"+ok+" = icmp eq i32 %"+status+", 0");
            line("br i1 %"+ok+", label %"+pass+", label %"+fail);
            ir+=fail+":\n";line("ret i32 %"+status);
            ir+=pass+":\n";line("%"+value+" = load i64, ptr %scratch, align 8");
            return "%"+value;
        }
        auto b=visit(visit,n.right);
        if(exhausted)return "0";
        auto pair=fresh(),value=fresh(),bad=fresh(),pass=fresh();
        std::string op=n.op==Op::add?"sadd":n.op==Op::sub?"ssub":"smul";
        line("%"+pair+" = call {i64,i1} @llvm."+op+".with.overflow.i64(i64 "+a+", i64 "+b+")");
        line("%"+value+" = extractvalue {i64,i1} %"+pair+", 0");
        line("%"+bad+" = extractvalue {i64,i1} %"+pair+", 1");
        line("br i1 %"+bad+", label %overflow, label %"+pass);
        ir+=pass+":\n";return "%"+value;
    };
    line("switch i64 %arg, label %arm"+std::to_string(s.arms.size()-1)+" [");
    for(std::size_t i=0;i+1<s.arms.size();++i)
        line("i64 "+std::to_string(*s.arms[i].key)+", label %arm"+std::to_string(i));
    line("]");
    for(std::size_t i=0;i<s.arms.size();++i){
        ir+="arm"+std::to_string(i)+":\n";
        auto v=expr(expr,s.arms[i].body);if(exhausted)return {};
        line("store i64 "+v+", ptr %out, align 8");line("ret i32 0");
    }
    ir+="}\ndefine i32 @atto_named_scalar(i64 %arg, ptr %out) {\nentry:\n"
        "  %status = call i32 @stage(i64 %arg, i32 0, ptr %out)\n  ret i32 %status\n}\n";
    if(ir.size()>512*1024)return {};
    return ir;
}
}
