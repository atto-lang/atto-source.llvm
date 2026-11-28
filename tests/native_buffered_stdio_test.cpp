#include "oreslang/runtime/NativeBufferedStdio.hpp"
#include <cassert>
#include <iostream>
using Q=oreslang::runtime::NativeBufferedStdio;
using R=Q::Result;
int main(){
    Q::Owner a{1,1},b{1,2};
    Q q(4,4,4,8);
    assert(q.admit(a,"123").result==R::Accepted);
    auto first=q.take_sink();assert(first && first->payload=="123");
    auto head=q.admit(a,"XX"),tail=q.admit(b,"Y");
    assert(head.result==R::Pending && tail.result==R::Pending);
    assert(q.take_ready(b,tail.ticket)==R::Pending);
    assert(!q.cancel(b,head.ticket) && q.cancel(a,head.ticket));
    assert(q.take_ready(a,tail.ticket)==R::Stale);
    assert(q.take_ready(b,tail.ticket)==R::Accepted);
    assert(q.take_ready(b,tail.ticket)==R::Stale && !q.cancel(b,tail.ticket));
    assert(q.accepted_bytes()==4 && q.finish_sink(first->ticket,true)==R::Accepted);
    auto last=q.take_sink();assert(last && last->payload=="Y");
    assert(q.finish_sink(last->ticket,true)==R::Accepted && q.drained());
    std::cout<<"PASS: FIFO, cancellation, byte cap and generation\n";
    Q x(1,4,1,4),y(1,4,1,4);
    assert(x.admit(a,"1234").result==R::Accepted && y.admit(a,"1234").result==R::Accepted);
    auto tx=x.admit(a,"a"),ty=y.admit(a,"b");
    assert(tx.result==R::Pending && ty.result==R::Pending);
    assert(!y.cancel(a,tx.ticket) && y.take_ready(a,tx.ticket)==R::Stale);
    auto sx=x.take_sink(),sy=y.take_sink();assert(sx && sy);
    assert(y.finish_sink(sx->ticket,true)==R::Stale && y.finish_sink(sy->ticket,true)==R::Accepted);
    assert(y.admit(b,"z").result==R::Quota);
    y.retire(a);assert(y.take_ready(a,ty.ticket)==R::Stale);
    auto next=y.admit(b,"z");assert(next.result==R::Pending);
    y.retire(a);assert(y.take_ready(b,next.ticket)==R::Pending);
    sy=y.take_sink();assert(sy && y.finish_sink(sy->ticket,true)==R::Accepted);
    assert(y.take_ready(b,next.ticket)==R::Accepted);
    x.abort();assert(x.take_ready(a,tx.ticket)==R::Failed && x.accepted_bytes()==4 && !x.drained());
    assert(x.finish_sink(sx->ticket,true)==R::Failed && x.drained() && x.accepted_bytes()==0);
    std::cout<<"PASS: descriptor fences, ready quotas, retirement and abort\n";
    Q z(1,4,2,2);
    for(auto bad:{"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\x80"})
        assert(z.admit(a,bad).result==R::Invalid);
    assert(z.admit(a,"\xf0\x9f\x98\x80x").result==R::Oversize);
    assert(z.admit(a,"").result==R::Accepted);
    auto p=z.admit(a,"yy");assert(p.result==R::Pending);
    assert(z.admit(b,"z").result==R::Quota);
    z.seal();assert(z.take_ready(a,p.ticket)==R::Closed && z.admit(b,"z").result==R::Closed);
    auto empty=z.take_sink();assert(empty && empty->payload.empty());
    assert(z.finish_sink(empty->ticket,true)==R::Accepted && z.drained());
    std::cout<<"PASS: strict UTF-8, empty records, seal and quotas\n";
}
