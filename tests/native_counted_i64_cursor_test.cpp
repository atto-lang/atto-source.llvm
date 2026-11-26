#include "oreslang/runtime/NativeCountedI64Cursor.hpp"
#include <cassert>
#include <iostream>
using Cursor=oreslang::runtime::NativeCountedI64Cursor<4,4,8>;
using Channel=Cursor::Channel;
using Future=Cursor::Future;
using State=Cursor::State;
static void round(Cursor& c){c.begin_round();assert(c.pump());}
static void exact(){
    auto a=std::make_shared<Channel>();
    for(auto n:{10,20,30,40})assert(a->try_write(n));
    Cursor c(3,{a});round(c);round(c);round(c);
    assert((c.result()==std::vector<std::int64_t>{10,20,30}));
    auto copy=c.result();copy[0]=99;assert(c.result()[0]==10);
    assert(a->try_read().value()==40&&a->pending_readers()==0&&!c.cancel());
}
static void mixed(){
    auto a=std::make_shared<Channel>(),b=std::make_shared<Channel>();
    auto f=std::make_shared<Future>();assert(f->resolve(20));
    auto alias=std::make_shared<Future>(*f);
    assert(a->try_write(10)&&a->try_write(40)&&b->try_write(30)&&b->try_write(50));
    Cursor c(4,{a,f,b,alias});for(int i=0;i<4;++i)round(c);
    assert((c.result()==std::vector<std::int64_t>{10,20,30,40}));
    assert(b->try_read().value()==50&&f->pending_waiters()==0);
    auto g=std::make_shared<Future>();assert(g->resolve(11));assert(a->try_write(22));
    Cursor priority(2,{g,a},false);round(priority);round(priority);
    assert((priority.result()==std::vector<std::int64_t>{11,22}));
}
static void wake_and_cancel(){
    auto f=std::make_shared<Future>();Cursor c(1,{f});c.begin_round();
    assert(!c.pump()&&!c.dirty()&&f->pending_waiters()==1);
    std::thread producer([&]{assert(f->resolve(42));});producer.join();
    assert(c.dirty()&&c.pump()&&c.result()[0]==42&&f->pending_waiters()==0);
    auto g=std::make_shared<Future>();Cursor pending(1,{g});pending.begin_round();
    assert(pending.cancel()&&g->pending_waiters()==0&&g->resolve(9));
    assert(pending.state()==State::Cancelled);
}
static void eof_and_exhaustion(){
    auto a=std::make_shared<Channel>(),b=std::make_shared<Channel>();
    assert(a->try_write(1)&&a->close()&&b->try_write(2));
    Cursor c(2,{a,a,b},false);round(c);round(c);round(c);
    assert((c.result()==std::vector<std::int64_t>{1,2}));
    assert(a->pending_readers()==0&&b->pending_readers()==0);
    auto f=std::make_shared<Future>();assert(f->resolve(1));
    Cursor short_batch(2,{f,f});round(short_batch);assert(short_batch.state()==State::Failed);
    try{std::rethrow_exception(short_batch.failure());assert(false);}
    catch(const std::runtime_error& e){assert(std::string(e.what())=="SelectExhausted");}
    try{(void)short_batch.result();assert(false);}catch(const std::logic_error&){}
    assert(f->pending_waiters()==0&&Cursor(0,{}).result().empty());
}
static void rollback_and_owner(){
    auto a=std::make_shared<Channel>();
    Cursor c(2,{a,a,a,a,a});
    try{c.begin_round();assert(false);}catch(const std::overflow_error&){}
    assert(c.state()==State::Failed&&a->pending_readers()==0);
    Cursor p(1,{a});bool foreign=false;
    std::thread t([&]{try{p.begin_round();}catch(const std::logic_error&){foreign=true;}});t.join();
    assert(foreign&&p.cancel()&&a->pending_readers()==0);
    {Cursor abandoned(1,{a});abandoned.begin_round();assert(a->pending_readers()==1);}
    assert(a->pending_readers()==0);
}
static void producer_close_while_waiting(){
    auto a=std::make_shared<Channel>(),b=std::make_shared<Channel>();
    Cursor c(1,{a,b});c.begin_round();assert(!c.pump());
    std::thread t([&]{assert(a->close());assert(b->try_write(42));});t.join();
    assert(c.dirty()&&c.pump()&&c.state()==State::Ready);
    round(c);assert(c.result()[0]==42);
    assert(a->pending_readers()==0&&b->pending_readers()==0);
}
int main(){exact();mixed();wake_and_cancel();eof_and_exhaustion();rollback_and_owner();producer_close_while_waiting();std::cout<<"NATIVE_COUNTED_I64_CURSOR_PASS\n";}
