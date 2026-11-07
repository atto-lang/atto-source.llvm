#include "oreslang/types/OwnershipMirEvidence.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::ownership;
using K = Operation::Kind;
using UC = MirUseClass;
using SO = FieldStoreOpcode;

Operation op(K kind, const std::string& path = "x",
             std::optional<std::uint64_t> loan = std::nullopt,
             std::optional<std::uint64_t> write = std::nullopt) {
    return {kind,path,loan,write};
}
Graph graph(std::vector<Operation> ops) {
    Graph g;
    g.entry={{"x",PlaceState{kOwned}},{"y",PlaceState{kOwned}}};
    g.blocks={{std::move(ops),{}}};
    return g;
}
Graph loan() {
    return graph({op(K::BorrowShared,"x",31),op(K::UseSharedLoan,"x",31),
                  op(K::ReleaseShared,"x",31)});
}
Graph write() {
    return graph({op(K::Move),op(K::Reinitialize,"x",std::nullopt,71)});
}
TypedMirLoanUse u(UC kind=UC::Load) { return {41,1,31,"x",kind,51}; }
TypedMirFieldWrite w() { return {1,71,"x",61}; }
LoweredFieldStore s() { return {71,81,"x",61,SO::NativeStore}; }
MirReceiptCandidate inspect(const Graph& g,
    const std::vector<TypedMirLoanUse>& typed={},
    const std::vector<TypedMirLoanUse>& lowered={},
    const std::vector<TypedMirFieldWrite>& mir={},
    const std::vector<LoweredFieldStore>& stores={}) {
    return inspect_typed_mir_receipt_candidate_v1(g,typed,lowered,mir,stores);
}
bool denied(const std::function<void()>& fn, const std::string& why="") {
    try {fn();}
    catch(const std::runtime_error& e) {
        const std::string message(e.what());
        return message.find("ownership violation:")!=std::string::npos &&
            (why.empty() || message.find(why)!=std::string::npos);
    }
    return false;
}
void check(bool yes,const char* label,unsigned& count) {
    ++count;
    if(!yes)throw std::runtime_error(std::string("case ")+
        std::to_string(count)+": "+label);
}

int main() {
    unsigned n=0;
    try {
        check(denied([] {
            auto g=loan();g.blocks[0].operations.push_back(op(K::Read));
            inspect(g,{u()},{u()});
        },"opaque effect after borrow"),"read after release rejected",n);
        check(denied([] {
            auto g=graph({op(K::BorrowShared,"x",31),op(K::Read),
                op(K::UseSharedLoan,"x",31),op(K::ReleaseShared,"x",31)});
            auto use=u();use.cfg_operation=2;
            inspect(g,{use},{use});
        },"opaque effect after borrow"),"read while loan active rejected",n);
        check(denied([] {
            auto g=loan();g.blocks[0].operations.push_back(op(K::Read,"y"));
            inspect(g,{u()},{u()});
        },"opaque effect after borrow"),"unclassified independent read rejected",n);
        check(denied([] {
            auto g=loan();g.entry["p.field"]=PlaceState{kOwned};
            g.blocks[0].operations.push_back(op(K::ReadObject,"p"));
            inspect(g,{u()},{u()});
        },"opaque effect after borrow"),"whole object read rejected",n);
        check(denied([] {
            auto g=loan();g.blocks[0].operations.push_back(op(K::Suspend));
            inspect(g,{u()},{u()});
        },"opaque effect after borrow"),"suspend after release rejected",n);
        check([] {
            auto g=graph({op(K::Read),op(K::BorrowShared,"x",31),
                op(K::UseSharedLoan,"x",31),op(K::ReleaseShared,"x",31)});
            auto use=u();use.cfg_operation=2;
            auto result=inspect(g,{use},{use});
            return !result.verified && !result.compiler_backed &&
                result.classified_uses==1;
        }(),"pre-borrow read remains candidate-only",n);
        check([] {
            auto g=graph({op(K::Suspend),op(K::BorrowShared,"x",31),
                op(K::UseSharedLoan,"x",31),op(K::ReleaseShared,"x",31)});
            auto use=u();use.cfg_operation=2;
            auto result=inspect(g,{use},{use});
            return !result.verified && result.classified_uses==1;
        }(),"pre-borrow suspension remains candidate-only",n);
        check(denied([] {
            inspect(loan(),{u(UC::PhiEdge)},{u(UC::PhiEdge)});
        },"unproven or repeated typed SSA use"),"one-block phi refused",n);
        check([] {
            const auto x=inspect(loan(),{u(UC::ReturnValue)},{u(UC::ReturnValue)});
            return !x.verified && !x.compiler_backed && x.classified_uses==1;
        }(),"return use stays unverified",n);
        check(denied([] {
            auto g=graph({op(K::Read),op(K::ReleaseShared,"x",81)});
            g.entry["x"]=PlaceState{kOwned,1,false,{81}};
            inspect(g);
        },"opaque effect after borrow"),"entry loan hides an opaque read",n);
        check(denied([] {
            auto g=loan();
            for(unsigned i=0;i<4096;i++)
                g.entry.emplace("oversized"+std::to_string(i),PlaceState{kOwned});
            inspect(g,{u()},{u()});
        },"entry exceeds bound"),"bounded entry inventory",n);
        check(denied([] {
            auto name=std::string(257,'z');
            auto g=graph({op(K::Read,name)});
            g.entry.emplace(name,PlaceState{kOwned});
            inspect(g);
        },"operation place exceeds bound"),"bounded operation place",n);
        check(denied([] {
            auto typed=u();typed.place=std::string(257,'z');
            inspect(loan(),{typed},{typed});
        },"SSA use place exceeds bound"),"bounded typed use place",n);
        check(denied([] {
            auto lowered=u();lowered.place=std::string(257,'z');
            inspect(loan(),{u()},{lowered});
        },"lowered edge place exceeds bound"),"bounded lowered edge place",n);
        check(denied([] {
            auto mir=w();mir.place=std::string(257,'z');
            inspect(write(),{}, {},{mir},{s()});
        },"field-write place exceeds bound"),"bounded MIR write place",n);
        check(denied([] {
            auto store=s();store.place=std::string(257,'z');
            inspect(write(),{}, {},{w()},{store});
        },"store place exceeds bound"),"bounded reported store place",n);
        check(denied([] {
            std::vector<TypedMirLoanUse> many(4097,u());
            inspect(loan(),many,{u()});
        },"receipt exceeds bound"),"bounded SSA inventory",n);
        check([] {
            const auto x=inspect(graph({op(K::Read)}));
            return !x.verified && !x.compiler_backed &&
                x.classified_uses==0 && x.matched_field_stores==0;
        }(),"no-borrow read stays unverified",n);
        if(n!=18)throw std::runtime_error("unexpected number of test cases");
        std::cout<<"PASS 18/18 opaque effects and bounded typed-MIR inputs "
                    "(HOST_CPP17_ONLY; NOT GUEST SSA PROOF)\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAILED after case "<<n<<": "<<e.what()<<'\n';
        return 1;
    }
}
