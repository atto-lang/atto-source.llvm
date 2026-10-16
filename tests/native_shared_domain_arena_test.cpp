#include "oreslang/runtime/SharedDomainArena.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <iostream>
using oreslang::runtime::SharedDomainArena;
void require(bool cond) { if (!cond) throw std::runtime_error("shared domain test failed"); }
template<typename F> void reject(F&& f) {
    bool denied=false;
    try { f(); } catch (const std::exception&) { denied=true; }
    require(denied);
}
int main() {
    SharedDomainArena a;
    auto writerGrant=a.grant(101, true, true);
    auto readerGrant=a.grant(102, true, false);
    auto other=SharedDomainArena{};
    auto foreignGrant=other.grant(104,true,false);
    reject([&] { (void)a.read(foreignGrant); });
    SharedDomainArena::Handle h{};
    {
        auto writer=a.write(writerGrant);
        h=writer.allocate(sizeof(std::uint64_t));
        std::uint64_t x=0x1234567890abcdefull;
        writer.write(h, &x,sizeof(x));
        reject([&] { writer.read(h, &x,sizeof(x)-1); });
    }
    std::atomic<std::uint64_t> observed{0};
    std::thread otherCarrier([&] {
        auto r=a.read(readerGrant);
        std::uint64_t x=0;
        r.read(h,&x,sizeof(x));
        observed.store(x);
        reject([&] { r.write(h,&x,sizeof(x)); });
    });
    otherCarrier.join();
    require(observed == 0x1234567890abcdefull);
    reject([&] { (void)a.write(readerGrant); });
    a.revoke(readerGrant);
    reject([&] { (void)a.read(readerGrant); });
    // Retirement is nonblocking and does not free underneath an active lease.
    {
        auto pending=a.read(writerGrant);
        require(!a.try_retire());
        std::uint64_t x=0;
        reject([&] { pending.read(h,&x,sizeof(x)); });
    }
    require(a.try_retire());
    require(a.try_retire());
    reject([&] { (void)a.read(writerGrant); });
    reject([&] { (void)a.grant(3,true,true); });
    std::cout << "shared domain arena tests passed\n";
}
