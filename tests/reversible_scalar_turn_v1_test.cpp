#include "oreslang/runtime/ReversibleScalarTurnV1.h"
#include <cstdlib>
#include <iostream>
using namespace oreslang::runtime;
using K = TurnKind;
using S = TurnStatus;
using B = RetryBackoff;
using D = RetryDecision;
void check(bool ok) { if (!ok) std::abort(); }
int main() {
    ReversibleScalarTurnV1<2,8> t(100);
    auto first = t.apply(1,0,K::commit,0);
    check(first.status == S::ok && t.value() == 0);
    check(t.apply(2,1,K::commit,7).status == S::ok);
    auto duplicate = t.apply(1,0,K::commit,0);
    check(duplicate.status == first.status && duplicate.version == first.version && duplicate.value == first.value);
    check(t.apply(1,0,K::commit,9).status == S::id_reuse);
    check(t.apply(3,1,K::undo,2).status == S::conflict);
    check(t.apply(3,2,K::undo,1).status == S::invalid_history);
    check(t.apply(3,2,K::commit,11).status == S::quota && t.version() == 2 && t.value() == 7);
    check(t.apply(3,2,K::undo,2).status == S::ok && t.value() == 0);
    check(t.apply(4,3,K::undo,1).status == S::ok && t.value() == 100);
    check(t.apply(5,4,K::redo,1).status == S::ok && t.value() == 0);
    check(t.apply(6,5,K::commit,42).status == S::ok);
    check(t.apply(7,6,K::redo,2).status == S::invalid_history && t.version() == 6 && t.value() == 42);
    check(t.apply(0,6,K::commit,1).status == S::invalid_command);
    check(t.apply(8,6,static_cast<K>(99),1).status == S::invalid_command);
    ReversibleScalarTurnV1<1,2> edge(INT64_MIN);
    check(edge.apply(1,0,K::commit,INT64_MAX).status == S::ok);
    check(edge.apply(2,1,K::undo,1).status == S::ok && edge.value() == INT64_MIN);
    check(edge.apply(3,2,K::redo,1).status == S::quota && edge.version() == 2);
    check(edge.apply(1,0,K::commit,INT64_MAX).value == INT64_MAX);
    auto p = [](int completed, std::int64_t elapsed, bool eligible=true, bool cancelled=false) {
        return retry_after_failure(5,completed,100,250,1000,elapsed,B::exponential,eligible,cancelled);
    };
    check(p(1,0).delay == 100 && p(2,100).delay == 200 && p(3,300).delay == 250);
    check(p(5,800).decision == D::exhausted && p(4,750).decision == D::deadline);
    check(p(1,0,true,true).decision == D::cancelled && p(1,0,false).decision == D::ineligible);
    check(p(0,0).decision == D::invalid);
    check(retry_after_failure(INT32_MAX,INT32_MAX-1,1,INT64_MAX,INT64_MAX,0,B::exponential,true,false).decision == D::deadline);
    check(retry_after_failure(2,1,0,0,1,0,B::fixed,true,false).decision == D::wait);
    check(retry_after_failure(2,1,0,0,1,1,B::fixed,true,false).decision == D::deadline);
    std::cout << "reversible scalar turn + retry admission host checks passed\n";
}
