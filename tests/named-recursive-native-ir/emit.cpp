#include "NamedRecursiveScalarIr.hpp"
#include <cassert>
#include <charconv>
#include <iostream>
#include <string_view>
using namespace oreslang::backend::named_recursive;
int main(int argc,char** argv){
    assert(argc==2||argc==3);
    const Stage fib{{{Op::parameter},{Op::literal,0,0,0},{Op::literal,0,0,1},{Op::literal,0,0,2},{Op::sub,0,3},{Op::self,4},{Op::sub,0,2},{Op::self,6},{Op::add,5,7}},{{0,1},{1,2},{std::nullopt,8}}};
    const Stage count{{{Op::parameter},{Op::literal,0,0,0},{Op::literal,0,0,1},{Op::sub,0,2},{Op::self,3},{Op::add,4,2}},{{0,1},{std::nullopt,5}}};
    const Stage plus{{{Op::parameter},{Op::literal,0,0,1},{Op::add,0,1}},{{std::nullopt,2}}};
    const Stage order{{{Op::parameter},{Op::literal,0,0,9223372036854775807LL},{Op::literal,0,0,1},{Op::add,1,2},{Op::self,0},{Op::add,3,4}},{{std::nullopt,5}}};
    const std::string_view mode=argv[1];
    if(mode=="invalid"){
        auto bad=fib;bad.nodes[5].left=5;assert(!emit(bad));
        bad=fib;bad.arms.back().key=3;assert(!emit(bad));
        bad=fib;bad.arms[1].key=0;assert(!emit(bad));
        bad=fib;bad.nodes[0].op=static_cast<Op>(255);assert(!emit(bad));
        bad=fib;bad.arms[0].body=999;assert(!emit(bad));
        bad={{{Op::parameter}},{{std::nullopt,0}}};
        for(std::size_t i=0;i<14;i++)bad.nodes.push_back({Op::add,i,i});
        bad.arms[0].body=14;assert(valid(bad));assert(!emit(bad));
        return 0;
    }
    const Stage* stage=mode=="fib"?&fib:mode=="count"?&count:mode=="plus"?&plus:mode=="order"?&order:nullptr;
    if(!stage)return 2;
    std::optional<std::int64_t> input;
    if(argc==3){
        std::string_view word=argv[2];std::int64_t n=0;
        const auto parsed=std::from_chars(word.data(),word.data()+word.size(),n);
        assert(parsed.ec==std::errc{}&&parsed.ptr==word.data()+word.size());input=n;
    }
    auto ir=emit(*stage,input);assert(ir);std::cout<<*ir;
}
