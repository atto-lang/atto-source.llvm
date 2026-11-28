#include "FlatMachineTable.hpp"
#include <iostream>
using atto::machine::Edge;
using atto::machine::Node;
using atto::machine::Table;
static unsigned checks;
static void check(bool ok) { if (!ok) throw std::runtime_error("check failed"); ++checks; }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::invalid_argument& e) { check(std::string(e.what()) == "E_MACHINE_TABLE"); return; }
    throw std::runtime_error("accepted invalid graph");
}
static std::string id(const char* prefix, std::size_t i) { return prefix + std::to_string(1000 + i); }
int main() {
    const std::vector<Node> graph{{"Alpha", true, {}}, {"Zed", false, {{"AlphaEvent",0},{"Zulu",1}}}};
    const auto t = Table::verify(1, graph);
    check(t.initial() == 1);
    check(t.names() == std::vector<std::string>({"Alpha","Zed"}));
    check(t.events() == std::vector<std::string>({"AlphaEvent","Zulu"}));
    check(t.successor(1,0) == 0); check(t.successor(1,1) == 1); check(!t.successor(0,0));
    rejects([&]{ t.successor(2,0); }); rejects([&]{ t.successor(1,2); });
    rejects([&]{ Table::verify(2,graph); }); rejects([]{ Table::verify(0,{}); });
    auto bad = graph; std::reverse(bad.begin(),bad.end()); rejects([&]{ Table::verify(1,bad); });
    bad = graph; bad[0].edges.push_back({"X",1}); rejects([&]{ Table::verify(1,bad); });
    bad = graph; std::reverse(bad[1].edges.begin(),bad[1].edges.end()); rejects([&]{ Table::verify(1,bad); });
    bad = graph; bad[1].edges[0].target=9; rejects([&]{ Table::verify(1,bad); });
    bad = graph; bad[1].edges.erase(bad[1].edges.begin()); rejects([&]{ Table::verify(1,bad); });
    rejects([]{ Table::verify(0,{{"A",false,{{"E",0},{"E",0}}}}); });
    rejects([]{ Table::verify(0,{{"",true,{}}}); });
    rejects([]{ Table::verify(0,{{std::string(1025,'a'),true,{}}}); });
    rejects([]{ Table::verify(0,{{"\xc3\xa9",true,{}}}); });
    std::vector<Edge> edges;
    for (std::size_t i=0;i<256;++i) edges.push_back({id("E",i),0});
    check(Table::verify(0,{{"N",false,edges}}).events().size()==256);
    edges.push_back({id("E",256),0}); rejects([&]{ Table::verify(0,{{"N",false,edges}}); });
    std::vector<Node> nodes;
    for (std::size_t i=0;i<256;++i) nodes.push_back({id("N",i),i==255,i==255?std::vector<Edge>{}:std::vector<Edge>{{"Next",i+1}}});
    check(Table::verify(0,nodes).names().size()==256);
    nodes.push_back({id("N",256),true,{}}); rejects([&]{ Table::verify(0,nodes); });
    const auto cycle=Table::verify(0,{{"Idle",false,{{"Start",1}}},{"Running",false,{{"Stop",0}}}});
    check(cycle.successor(0,0)==1);check(cycle.successor(1,1)==0);check(!cycle.successor(1,0));
    std::cout << checks << " native graph-table checks passed (host only)\n";
}
