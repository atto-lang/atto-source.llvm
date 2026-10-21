// Identical deterministic actor streams run through the two real host
// runtimes. Callbacks use the compiler scalar ABI; no guest codegen claimed.
#if defined(ORES_PARITY_FORGE)
#include "runtime/scalar_actor.hpp"
#else
#include "oreslang/runtime/SourceActorBootstrap.h"
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

static std::array<std::atomic<unsigned>,8> counts{};
static std::array<std::atomic<std::int64_t>,8> results{};
static void require(bool yes,const char* why) { if (!yes) throw std::runtime_error(why); }
template<unsigned I> static void initialize(OresActorI64Heap* heap,std::uint64_t domain,std::uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap,domain,epoch,0,I+1);
    ores_actor_i64_heap_field_init(heap,domain,epoch,1,I);
}
static std::int64_t message(OresActorI64Heap* heap,std::uint64_t domain,std::uint64_t epoch,std::int64_t n) {
    auto ordinal=ores_actor_i64_heap_field_read(heap,domain,epoch,1);
    if (ordinal<0 || ordinal>=8) std::abort();
    const auto index=static_cast<unsigned>(ordinal);
    const auto previous=ores_actor_i64_heap_field_read(heap,domain,epoch,0);
    // Bounded arithmetic; order-sensitive and no signed overflow.
    const auto value=(previous*131+n)%1000000007;
    ores_actor_i64_heap_field_write(heap,domain,epoch,0,value);
    results[index].store(value);
    ++counts[index];
    return value;
}
#if defined(ORES_PARITY_FORGE)
using Actor=oresforge::runtime::ScalarActor;
using Initializer=Actor::Initializer;
#else
using Initializer=OresSourceActorInit;
#endif
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected seed");
        auto random=std::stoull(argv[1]);
        require(random!=0,"zero seed");
        auto next=[&]() { random^=random<<13; random^=random>>7; random^=random<<17; return random; };
        const Initializer initializers[]={initialize<0>,initialize<1>,initialize<2>,initialize<3>,initialize<4>,initialize<5>,initialize<6>,initialize<7>};
        std::uint64_t digest=1469598103934665603ULL, delivered=0;
        for (unsigned round=0;round<3;++round) {
            std::array<unsigned,8> expected_counts{};
            std::array<std::int64_t,8> expected{};
            std::array<unsigned,8> limits{}, stop_order{0,1,2,3,4,5,6,7};
#if defined(ORES_PARITY_FORGE)
            std::array<std::unique_ptr<Actor>,8> actors;
#else
            std::array<void*,8> actors{};
#endif
            for (unsigned i=0;i<8;++i) {
                counts[i]=0; results[i]=0; expected[i]=i+1;
                limits[i]=static_cast<unsigned>(next()%65);
#if defined(ORES_PARITY_FORGE)
                // Same numeric id intentionally: storage and authority must
                // remain per instance, independently of user-visible ids.
                actors[i]=std::make_unique<Actor>(17,2,128,initializers[i],message);
#else
                actors[i]=oreslang_source_actor_spawn(initializers[i],message,2);
#endif
            }
            for (unsigned turn=0;turn<64;++turn) {
                const auto start=static_cast<unsigned>(next()%8);
                for (unsigned offset=0;offset<8;++offset) {
                    const auto i=(start+offset)%8;
                    if (turn>=limits[i]) continue;
                    const auto payload=static_cast<std::int64_t>(next()%256);
                    expected[i]=(expected[i]*131+payload)%1000000007;
                    ++expected_counts[i];
#if defined(ORES_PARITY_FORGE)
                    require(actors[i]->try_send(actors[i]->ref(),payload)==Actor::Send::Accepted,"bounded actor send refused");
                    if ((turn+offset)%3==0) (void)actors[i]->run_one();
#else
                    oreslang_source_actor_send(actors[i],payload);
#endif
                }
            }
            for (unsigned i=7;i>0;--i) {
                const auto j=static_cast<unsigned>(next()%(i+1));
                std::swap(stop_order[i],stop_order[j]);
            }
            for (const auto i:stop_order) {
#if defined(ORES_PARITY_FORGE)
                actors[i]->stop();
                while (!actors[i]->done()) (void)actors[i]->run_one();
#else
                oreslang_source_actor_stop(actors[i]);
#endif
                require(counts[i].load()==expected_counts[i],"accepted message lost or duplicated");
                require(results[i].load()==(expected_counts[i] ? expected[i] : 0),"FIFO/state-isolation mismatch");
                delivered+=expected_counts[i];
                digest=(digest^i)*1099511628211ULL;
                digest=(digest^expected_counts[i])*1099511628211ULL;
                digest=(digest^static_cast<std::uint64_t>(results[i].load()))*1099511628211ULL;
            }
        }
        std::printf("%s %llu %llu\n",argv[1],static_cast<unsigned long long>(delivered),static_cast<unsigned long long>(digest));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 70; }
}
