// CTest must not silently skip assert-based WAL test oracles under -DNDEBUG.
// This source compiles actual checks even when a Release build defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "oreslang/runtime/DbWalReplayV1.hpp"
#include "oreslang/runtime/DbDurableWalFile.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
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
// Same deterministic ODB1 wire oracle as Java DbWalReplayV1Test.
static std::vector<std::uint8_t> boundary_frame(
        unsigned kind, unsigned seq, unsigned op,
        unsigned key_length, unsigned value_length) {
    std::vector<std::uint8_t> wire(26U + key_length + value_length + 4U);
    wire[0]=79;wire[1]=68;wire[2]=66;wire[3]=49;
    wire[4]=1;wire[5]=static_cast<std::uint8_t>(kind);
    u32(wire,6,7);u32(wire,10,4);u32(wire,14,seq);u32(wire,18,op);
    u16(wire,22,key_length);u16(wire,24,value_length);
    for (unsigned i=0;i<key_length;++i)
        wire[26U+i]=static_cast<std::uint8_t>(i*17U+3U);
    for (unsigned i=0;i<value_length;++i)
        wire[26U+key_length+i]=static_cast<std::uint8_t>(i*29U+11U);
    unsigned a=1,b=0;
    for (std::size_t i=0;i<wire.size()-4U;++i) {
        a=(a+wire[i])%65521U;
        b=(b+a)%65521U;
    }
    u16(wire,static_cast<unsigned>(wire.size()-4U),a);
    u16(wire,static_cast<unsigned>(wire.size()-2U),b);
    return wire;
}

static void check_maximum_wire_replay() {
    auto first=boundary_frame(1,1,41,1,0);
    auto maximum=boundary_frame(1,2,42,64,1024);
    auto deletion=boundary_frame(2,3,43,64,0);
    assert(first.size()==31 && maximum.size()==1118 && deletion.size()==94);
    assert(maximum[1114]==0x42 && maximum[1115]==0x1d &&
           maximum[1116]==0x02 && maximum[1117]==0x86);
    assert(deletion[90]==0x23 && deletion[91]==0x1f &&
           deletion[92]==0xdf && deletion[93]==0x0a);

    char name[]="/tmp/ores-wal-max-replay-XXXXXX";
    int fd=::mkstemp(name);
    assert(fd>=0);
    assert(::pwrite(fd,first.data(),first.size(),0)==31);
    assert(::pwrite(fd,maximum.data(),maximum.size(),31)==1118);
    assert(::pwrite(fd,deletion.data(),deletion.size(),1149)==94);
    unsigned visited=0;
    auto full=DbWalReplayV1::scan(fd,7,4,
        [&](const std::uint8_t* bytes,std::size_t n,std::uint64_t off) {
            if (visited==0) assert(n==31 && off==0 && bytes[5]==1);
            if (visited==1) assert(n==1118 && off==31 && bytes[5]==1);
            if (visited==2) assert(n==94 && off==1149 && bytes[5]==2);
            ++visited;
        });
    assert(visited==3 && full.records==3 && full.valid_bytes==1243);
    assert(full.last_sequence==3 && full.last_operation==43 &&
           !full.incomplete_tail);

    // Every partial length through the maximum supported ODB1 frame.
    for (std::size_t n=1;n<maximum.size();++n) {
        assert(::ftruncate(fd,31)==0);
        assert(::pwrite(fd,maximum.data(),n,31)==static_cast<ssize_t>(n));
        auto tail=DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){});
        assert(tail.records==1 && tail.valid_bytes==31 &&
               tail.incomplete_tail);
        struct stat info{};
        assert(::fstat(fd,&info)==0 &&
               info.st_size==static_cast<off_t>(31+n));
    }

    auto corrupt=maximum;
    corrupt.back() ^= 1;
    assert(::ftruncate(fd,31)==0);
    assert(::pwrite(fd,corrupt.data(),corrupt.size(),31)==1118);
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    struct stat info{};
    assert(::fstat(fd,&info)==0 && info.st_size==1149);

    auto bad_delete=boundary_frame(2,2,42,64,1);
    assert(::ftruncate(fd,31)==0);
    assert(::pwrite(fd,bad_delete.data(),bad_delete.size(),31)==
           static_cast<ssize_t>(bad_delete.size()));
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    assert(::fstat(fd,&info)==0 &&
           info.st_size==static_cast<off_t>(31+bad_delete.size()));

    auto oversized=boundary_frame(1,2,42,64,1025);
    assert(::ftruncate(fd,31)==0);
    assert(::pwrite(fd,oversized.data(),oversized.size(),31)==
           static_cast<ssize_t>(oversized.size()));
    rejects([&]{ DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
    assert(::fstat(fd,&info)==0 &&
           info.st_size==static_cast<off_t>(31+oversized.size()));
    assert(::close(fd)==0);
    assert(::unlink(name)==0);
}


static void check_rechecksummed_header_forgeries() {
    auto first = boundary_frame(1,1,41,1,0);
    auto valid = boundary_frame(1,2,42,1,1);
    char name[] = "/tmp/ores-wal-rechecksummed-XXXXXX";
    int fd = ::mkstemp(name);
    assert(fd >= 0);
    assert(::pwrite(fd,first.data(),first.size(),0)==
           static_cast<ssize_t>(first.size()));
    for (unsigned attempt=0; attempt<7; ++attempt) {
        auto changed = valid;
        switch (attempt) {
        case 0: changed[4]=2; break;
        case 1: changed[5]=2; break;
        case 2: changed[21]=128; break;
        case 3: changed[22]=0; break;
        case 4: changed[22]=65; break;
        case 5: changed[24]=1; changed[25]=4; break;
        case 6: changed[0]=80; break;
        default: std::abort();
        }
        unsigned a=1,b=0;
        for (std::size_t i=0;i<changed.size()-4U;++i) {
            a=(a+changed[i])%65521U;
            b=(b+a)%65521U;
        }
        u16(changed,static_cast<unsigned>(changed.size()-4U),a);
        u16(changed,static_cast<unsigned>(changed.size()-2U),b);
        assert(::ftruncate(fd,static_cast<off_t>(first.size()))==0);
        assert(::pwrite(fd,changed.data(),changed.size(),first.size())==
               static_cast<ssize_t>(changed.size()));
        rejects([&]{ (void)DbWalReplayV1::scan(fd,7,4,[](auto,auto,auto){}); });
        struct stat statbuf{};
        assert(::fstat(fd,&statbuf)==0 &&
               statbuf.st_size==static_cast<off_t>(first.size()+changed.size()));
    }
    assert(::close(fd)==0 && ::unlink(name)==0);
}

int main(){
    char name[]="/tmp/ores-wal-replay-XXXXXX";
    int fd=::mkstemp(name); assert(fd>=0);
    auto first=frame(1,41), second=frame(2,42), third=frame(3,43);
    // Process A writes and syncs, exits without C++ destructors.
    pid_t pid=::fork(); assert(pid>=0);
    if(pid==0){
        append(fd,first); append(fd,second);
        ::_exit(oreslang::runtime::db_wal_datasync(fd)==0 ? 0:5);
    }
    int status=0;
    assert(::waitpid(pid,&status,0)==pid);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    // Reopen through an independent descriptor: do not accidentally
    // conflate inherited fork state with process-restart file recovery.
    assert(::close(fd)==0);
    fd=::open(name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    assert(fd>=0);
    unsigned visited=0;
    auto visit=[&](const std::uint8_t* bytes,std::size_t n,std::uint64_t off){
        assert(n==31 && off==31U*visited);
        assert(bytes[26]==41U+visited); ++visited;
    };
    auto ok=DbWalReplayV1::scan(fd,7,4,visit);
    assert(ok.records==2 && ok.valid_bytes==62 && !ok.incomplete_tail && visited==2);
    // The reopened descriptor starts at offset zero. A torn tail must
    // append at EOF, not overwrite an acknowledged earlier frame.
    assert(::lseek(fd,0,SEEK_END)==62);
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
    // Legitimate final-frame prefixes remain repairable, without mutation.
    for (std::size_t n = 1; n < third.size(); ++n) {
        assert(::ftruncate(fd,62)==0);
        assert(::pwrite(fd,third.data(),n,62)==static_cast<ssize_t>(n));
        auto partial = DbWalReplayV1::scan(
                fd,7,4,[](auto,auto,auto){});
        assert(partial.records==2 && partial.valid_bytes==62);
        assert(partial.incomplete_tail);
        struct stat current{};
        assert(::fstat(fd,&current)==0 &&
                current.st_size==static_cast<off_t>(62+n));
    }
    assert(::ftruncate(fd,62)==0);
    std::uint8_t wrong_magic = 0;
    assert(::pwrite(fd,&wrong_magic,1,62)==1);
    rejects([&]{ DbWalReplayV1::scan(
            fd,7,4,[](auto,auto,auto){}); });

    for (unsigned field : {0U,4U,5U,6U,10U,14U,18U,22U,24U}) {
        auto damaged=third;
        switch (field) {
        case 0: damaged[0]=0; break;
        case 4: damaged[4]=0; break;
        case 5: damaged[5]=3; break;
        case 6: u32(damaged,6,8); break;
        case 10: u32(damaged,10,5); break;
        case 14: u32(damaged,14,4); break;
        case 18: u32(damaged,18,42); break;
        case 22: u16(damaged,22,0); break;
        case 24: u16(damaged,24,65535); break;
        default: std::abort();
        }
        assert(::ftruncate(fd,62)==0);
        assert(::pwrite(fd,damaged.data(),26,62)==26);
        rejects([&]{ DbWalReplayV1::scan(
                fd,7,4,[](auto,auto,auto){}); });
        struct stat current{};
        assert(::fstat(fd,&current)==0 && current.st_size==88);
    }
    // Check the 12 identity/sequence u32 prefix bytes before they form a
    // complete header field; otherwise a corrupt short tail is misclassified.
    for (unsigned position=6; position<=17; ++position) {
        auto damaged=third;
        damaged[position] ^= 1;
        const std::size_t n=position+1U;
        assert(::ftruncate(fd,62)==0);
        assert(::pwrite(fd,damaged.data(),n,62)==static_cast<ssize_t>(n));
        rejects([&]{ DbWalReplayV1::scan(
                fd,7,4,[](auto,auto,auto){}); });
        struct stat current{};
        assert(::fstat(fd,&current)==0 &&
                current.st_size==static_cast<off_t>(62+n));
    }
    assert(::ftruncate(fd,62)==0);
    // Exercise the production-style trusted directory-relative locked
    // owner wrapper as a separate generation after full process reopen.
#if defined(__APPLE__)
    // /tmp is a symlink to /private/tmp on macOS; O_NOFOLLOW needs the real directory.
    int root=::open("/private/tmp", O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
#else
    int root=::open("/tmp", O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
#endif
    assert(root>=0);
    {
        auto owned=oreslang::runtime::DbDurableWalFile::open_trusted(
                root, name + 5, 5);
        auto report=owned->scan_recovery(7,4);
        assert(report.records==2 && report.valid_bytes==62);
        rejects([&]{ (void)owned->scan_recovery(8,4); });
        rejects([&]{ (void)owned->scan_recovery(7,5); });
    }
    assert(::close(root)==0);
    assert(::close(fd)==0); assert(::unlink(name)==0);
    check_maximum_wire_replay();
    check_rechecksummed_header_forgeries();
}
