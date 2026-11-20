// Host-only trusted WAL shard writer integration, not a guest durability ACK.
#include "oreslang/runtime/DbDurableWalFile.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
using namespace oreslang::runtime;

static void u16(std::array<std::uint8_t,31>& f,int at,unsigned x) {
    f[at]=static_cast<std::uint8_t>(x);
    f[at+1]=static_cast<std::uint8_t>(x>>8);
}
static void u32(std::array<std::uint8_t,31>& f,int at,unsigned x) {
    u16(f,at,x); u16(f,at+2,x>>16);
}
static std::array<std::uint8_t,31> frame(unsigned tenant,unsigned gen,
        unsigned seq,unsigned op) {
    std::array<std::uint8_t,31> f{};
    f[0]=79; f[1]=68; f[2]=66; f[3]=49; f[4]=1; f[5]=1;
    u32(f,6,tenant); u32(f,10,gen); u32(f,14,seq); u32(f,18,op);
    u16(f,22,1); u16(f,24,0); f[26]=107;
    unsigned a=1,b=0;
    for(unsigned i=0;i<27;++i){a=(a+f[i])%65521U;b=(b+a)%65521U;}
    u16(f,27,a); u16(f,29,b);return f;
}
template<class Fn> static void denies(Fn&& f){
    bool rejected=false;
    try{f();}catch(const std::exception&){rejected=true;}
    assert(rejected);
}
int main(){
    char name[]="/tmp/ores-db-wal-authorization-XXXXXX";
    char* dir=::mkdtemp(name);assert(dir);
    int root=::open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    assert(root>=0);
    auto wal=DbDurableWalFile::open_trusted(root,"wal.odb",4);
    int identity_fd=::openat(root,"wal.odb",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    assert(identity_fd>=0);
    struct stat info{};
    assert(::fstat(identity_fd,&info)==0);assert(::close(identity_fd)==0);
    using Registry=DbWalShardGrantRegistry<2>;
    Registry registry(55);
    DbWalOwnerRetirementGate gate(101,4);
    Registry::FileIdentity identity{static_cast<std::uint64_t>(info.st_dev),
                                   static_cast<std::uint64_t>(info.st_ino)};
    auto issued=registry.issue(55,7,3,101,4,identity,gate);
    assert(issued);
    auto first=frame(7,4,1,42);
    auto wrong_tenant=frame(8,4,1,42);
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        56,7,3,101,4,42,first.data(),first.size());});
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,3,101,5,42,first.data(),first.size());});
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,8,3,101,4,42,first.data(),first.size());});
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,4,101,4,42,first.data(),first.size());});
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,3,102,4,42,first.data(),first.size());});
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,3,101,4,42,wrong_tenant.data(),wrong_tenant.size());});
    assert(wal->file_size()==0 && !gate.active());
    auto receipt=wal->append_authorized_and_sync(registry,*issued,
        55,7,3,101,4,42,first.data(),first.size());
    assert(receipt.before==0 && receipt.after==31);
    assert(receipt.operation_id==42 && !gate.active() && !wal->uncertain());

    // A poisoned ODB1 payload is rejected before bytes are changed and
    // its reserved physical slot is retired even after worker admission.
    auto corrupt=frame(7,4,2,43);
    corrupt[26]^=1;
    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,3,101,4,43,corrupt.data(),corrupt.size());});
    assert(!gate.active() && wal->file_size()==31);

    // Linearize actual physical append admission against concurrent drain.
    // A write that wins must finish fdatasync before physical retirement;
    // a drain that wins must prevent the second write entirely.
    auto second=frame(7,4,2,43);
    std::atomic<bool> start{false};
    std::atomic<bool> appended{false};
    std::thread writer([&]{
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            auto r=wal->append_authorized_and_sync(registry,*issued,
                55,7,3,101,4,43,second.data(),second.size());
            assert(r.before==31 && r.after==62);
            appended.store(true,std::memory_order_release);
        } catch(const std::logic_error&) {
            // Valid only if the drain won admission.
        }
    });
    std::thread drainer([&]{
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        (void)registry.drain(*issued,55,101,4);
    });
    start.store(true,std::memory_order_release);
    writer.join();
    drainer.join();
    assert(!gate.active());
    assert(registry.drain(*issued,55,101,4));
    assert(wal->file_size()==(appended.load() ? 62U : 31U));

    denies([&]{(void)wal->append_authorized_and_sync(registry,*issued,
        55,7,3,101,4,43,corrupt.data(),corrupt.size());});
    assert(registry.begin_close(*issued,55,101,4));
    wal->close();
    gate.physical_close_retired(101,4);
    assert(registry.retire_after_physical_close(*issued,gate));
    assert(registry.outstanding()==0);
    wal.reset();
    assert(::unlinkat(root,"wal.odb",0)==0);
    assert(::close(root)==0);assert(::rmdir(dir)==0);
}
