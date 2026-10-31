#include "oreslang/runtime/DbWalReplayV1.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using oreslang::runtime::DbWalReplayV1;
static void u16(std::vector<std::uint8_t>& f, unsigned n, unsigned v) {
    f[n] = static_cast<std::uint8_t>(v);
    f[n + 1] = static_cast<std::uint8_t>(v >> 8);
}
static void u32(std::vector<std::uint8_t>& f, unsigned n, unsigned v) {
    u16(f, n, v); u16(f, n + 2, v >> 16);
}
static std::vector<std::uint8_t> frame(unsigned seq, unsigned op,
        unsigned tenant = 7, unsigned generation = 4) {
    std::vector<std::uint8_t> f(31);
    f[0]=79; f[1]=68; f[2]=66; f[3]=49; f[4]=1; f[5]=1;
    u32(f,6,tenant); u32(f,10,generation); u32(f,14,seq); u32(f,18,op);
    u16(f,22,1); u16(f,24,0); f[26]=static_cast<std::uint8_t>(op);
    unsigned a=1,b=0;
    for(unsigned i=0;i<27;++i){ a=(a+f[i])%65521U; b=(b+a)%65521U; }
    u16(f,27,a); u16(f,29,b);
    return f;
}
static void append(int fd,const std::vector<std::uint8_t>& f,
        std::size_t n = 31) {
    assert(n <= f.size());
    assert(::write(fd,f.data(),n) == static_cast<ssize_t>(n));
}
template<class F> static void rejects(F&& f) {
    bool failed=false;
    try { f(); } catch (const std::exception&) { failed=true; }
    assert(failed);
}
int main(){
    char name[]="/tmp/ores-wal-replay-XXXXXX";
    int fd=::mkstemp(name); assert(fd>=0);
    auto first=frame(1,41), second=frame(2,42), third=frame(3,43);
    // Process A writes and syncs, exits without C++ destructors.
    pid_t pid=::fork(); assert(pid>=0);
    if(pid==0){
        append(fd,first); append(fd,second);
        ::_exit(::fdatasync(fd)==0 ? 0:5);
    }
    int status=0;
    assert(::waitpid(pid,&status,0)==pid);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    unsigned visited=0;
    auto visit=[&](const std::uint8_t* bytes,std::size_t n,std::uint64_t off){
        assert(n==31 && off==31U*visited);
        assert(bytes[26]==41U+visited); ++visited;
    };
    auto ok=DbWalReplayV1::scan(fd,7,4,visit);
    assert(ok.records==2 && ok.valid_bytes==62 && !ok.incomplete_tail && visited==2);
    append(fd,third,15);
    auto tail=DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){});
    assert(tail.records==2 && tail.valid_bytes==62 && tail.incomplete_tail);
    assert(::ftruncate(fd,62)==0); // explicit exclusive test repair, NOT scanner behavior
    auto damaged=second; damaged[26]^=1;
    assert(::pwrite(fd,damaged.data(),damaged.size(),31)==31);
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    assert(::pwrite(fd,second.data(),second.size(),31)==31);
    auto duplicate=frame(2,41);
    assert(::pwrite(fd,duplicate.data(),duplicate.size(),31)==31);
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    auto gap=frame(4,42);
    assert(::pwrite(fd,gap.data(),gap.size(),31)==31);
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    assert(::pwrite(fd,second.data(),second.size(),31)==31);
    rejects([&]{ DbWalReplayV1::scan(fd,8,4,[](auto,auto,auto){}); });
    rejects([&]{ DbWalReplayV1::scan(fd,7,5,[](auto,auto,auto){}); });
    auto repaired=DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){});
    assert(repaired.records==2 && repaired.valid_bytes==62);
    assert(::close(fd)==0); assert(::unlink(name)==0);
}
