#include "oreslang/types/OwnershipCfg.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace oreslang::ownership;
using K=Operation::Kind;
Operation op(K k,const char* p="x",
    std::optional<std::uint64_t> loan=std::nullopt,
    std::optional<std::uint64_t> write=std::nullopt) {
    return {k,p,loan,write};
}
Graph make(std::vector<Operation> ops) {
    Graph g;g.entry={{"x",PlaceState{kOwned}},{"y",PlaceState{kOwned}}};
    g.blocks={{std::move(ops),{}}};return g;
}
bool denied(const std::function<void()>& f) {
    try {f();}catch(const std::runtime_error& e){
        return std::string(e.what()).find("ownership violation:")!=std::string::npos;
    }return false;
}
void check(bool passed,unsigned& n) {
    ++n;if(!passed)throw std::runtime_error("failed case "+std::to_string(n));
}
int main() {
    unsigned n=0;
    try {
        check(denied([]{verify(make({
            op(K::BorrowShared,"x",41),op(K::ReleaseShared,"x",41),
            op(K::Move),op(K::Reinitialize,"x",std::nullopt,41)}));}),n);
        check(denied([]{verify(make({
            op(K::BorrowExclusive,"x",42),op(K::ReleaseExclusive,"x",42),
            op(K::Move),op(K::Reinitialize,"x",std::nullopt,42)}));}),n);
        check(denied([]{
            constexpr std::uint64_t site=(std::uint64_t{1}<<32)|1;
            verify(make({op(K::BorrowShared),op(K::ReleaseShared),
                op(K::Move),op(K::Reinitialize,"x",std::nullopt,site)}));
        }),n);
        check(denied([]{
            auto g=make({op(K::ReleaseShared,"y",77),op(K::Move),
                op(K::Reinitialize,"x",std::nullopt,77)});
            g.entry["y"]=PlaceState{kOwned,1,false,{77}};verify(g);
        }),n);
        check(denied([]{
            auto g=make({op(K::Move),op(K::Reinitialize,"x",std::nullopt,95)});
            g.blocks.push_back({{op(K::BorrowShared,"y",95)},{}});
            verify(g);
        }),n);
        check((verify(make({op(K::BorrowShared,"x",61),
            op(K::UseSharedLoan,"x",61),op(K::ReleaseShared,"x",61),
            op(K::Move),op(K::Reinitialize,"x",std::nullopt,62)})),true),n);
        if(n!=6) return 13;
        std::cout<<"PASS 6/6 loan/write ticket namespace cases (HOST_CPP17_ONLY; NO GUEST SSA)\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAILED "<<n<<": "<<e.what()<<'\n';return 1;
    }
}
