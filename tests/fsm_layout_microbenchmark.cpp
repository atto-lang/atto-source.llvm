// Non-normative host scalar microbenchmark: not guest Oreslang codegen or SIMD.
#include "oreslang/runtime/DopLayout.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using oreslang::dop::Layout;
using oreslang::dop::Plan;
std::uint32_t get(const Plan& p,const std::vector<std::uint8_t>& b,
                  std::uint64_t row,std::uint32_t f) {
    std::uint32_t v=0;
    const auto o=p.byte_offset(row,f);
    if(o+4>b.size())throw std::runtime_error("bad offset");
    std::memcpy(&v,b.data()+static_cast<std::size_t>(o),4);
    return v;
}
void put(const Plan& p,std::vector<std::uint8_t>& b,std::uint64_t row,
         std::uint32_t f,std::uint32_t v) {
    const auto o=p.byte_offset(row,f);
    if(o+4>b.size())throw std::runtime_error("bad offset");
    std::memcpy(b.data()+static_cast<std::size_t>(o),&v,4);
}
struct Result { std::uint64_t digest;double ns_per_row;std::uint64_t bytes; };
Result run(Layout layout,std::uint64_t rows,std::uint32_t reps,bool column,std::uint32_t lanes) {
    const Plan p(layout,rows,3,4,16,layout==Layout::AoSoA?lanes:1);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(p.byte_length()),0);
    for(std::uint64_t i=0;i<rows;++i) {
        put(p,buffer,i,0,static_cast<std::uint32_t>(i%3));
        put(p,buffer,i,1,1);
        put(p,buffer,i,2,static_cast<std::uint32_t>(i%4));
    }
    const auto start=std::chrono::steady_clock::now();
    for(std::uint32_t rep=0;rep<reps;++rep)
        for(std::uint64_t i=0;i<rows;++i) {
            if(column) {
                const auto ticks=get(p,buffer,i,2);
                put(p,buffer,i,2,(ticks+1)%4);
            }else{
                std::uint32_t state=get(p,buffer,i,0);
                std::uint32_t version=get(p,buffer,i,1);
                std::uint32_t ticks=get(p,buffer,i,2);
                const std::uint32_t event=static_cast<std::uint32_t>((i+rep)%4);
                bool accepted=false;
                if(state==0 && event==0) {state=1;ticks=0;accepted=true;}
                else if(state==1 && event==1 && ticks<3) {++ticks;accepted=true;}
                else if(state==1 && event==2 && ticks>=2) {state=2;accepted=true;}
                else if(state==2 && event==3) {state=0;ticks=0;accepted=true;}
                if(accepted) {put(p,buffer,i,0,state);put(p,buffer,i,1,version+1);put(p,buffer,i,2,ticks);}
            }
        }
    const auto finish=std::chrono::steady_clock::now();
    std::uint64_t digest=1469598103934665603ull;
    for(std::uint64_t i=0;i<rows;++i)
        for(std::uint32_t f=0;f<3;++f) {
            digest^=get(p,buffer,i,f);
            digest*=1099511628211ull;
        }
    const auto ns=std::chrono::duration<double,std::nano>(finish-start).count();
    return {digest,ns/(static_cast<double>(rows)*reps),p.byte_length()};
}
const char* name(Layout l) {
    switch(l){case Layout::AoS:return "AoS";case Layout::SoA:return "SoA";case Layout::AoSoA:return "AoSoA";}
    throw std::runtime_error("unknown layout");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::invalid_argument("usage: benchmark ROWS REPS");
        const auto rows=std::stoull(argv[1]);
        const auto reps=std::stoul(argv[2]);
        if(rows<1 || rows>1000000 || reps<1 || reps>100)
            throw std::invalid_argument("bounds: rows 1..1000000, reps 1..100");
        std::cout<<"layout,mode,rows,reps,lanes,bytes,ns_per_entity_update,digest\n";
        for(bool column:{false,true}) {
            std::uint64_t reference=0;
            bool first=true;
            for(Layout l:{Layout::AoS,Layout::SoA,Layout::AoSoA}) {
                const auto result=run(l,rows,static_cast<std::uint32_t>(reps),column,4);
                if(!first && result.digest!=reference)throw std::runtime_error("layout digest mismatch");
                reference=result.digest;first=false;
                std::cout<<name(l)<<","<<(column?"column":"fsm_full")<<","<<rows<<","<<reps
                         <<","<<(l==Layout::AoSoA?4:1)<<","<<result.bytes<<","<<result.ns_per_row
                         <<","<<result.digest<<"\n";
            }
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
