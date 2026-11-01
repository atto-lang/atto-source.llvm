#include "state_region.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace ores::state;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while(false)
int main() {
    Region r;
    std::uint64_t seed=0x6a09e667f3bcc909ULL;
    std::uint64_t accepted=0, rejected=0;
    for (int i=0; i<500000; ++i) {
        seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
        int action=static_cast<int>(seed % 8);
        bool hadWriter=r.writer;
        auto hadReaders=r.readers;
        Status status{};
        switch(action) {
            case 0: status=r.acquire(Mode::read); break;
            case 1: status=r.acquire(Mode::write); break;
            case 2: status=r.release(Mode::read); break;
            case 3: status=r.release(Mode::write); break;
            case 4: status=r.pin(); break;
            case 5: status=r.unpin(); break;
            case 6: status=r.acquire(static_cast<Mode>(255)); CHECK(status==Status::invalid_mode); break;
            case 7: status=r.release(static_cast<Mode>(255)); CHECK(status==Status::invalid_mode); break;
        }
        CHECK(!(r.writer && r.readers));
        CHECK(!r.retired);
        if (action==6 || action==7) { CHECK(r.writer==hadWriter); CHECK(r.readers==hadReaders); }
        if (status==Status::ok) accepted++; else rejected++;
    }
    while(r.readers) CHECK(r.release(Mode::read)==Status::ok);
    if (r.writer) CHECK(r.release(Mode::write)==Status::ok);
    while(r.pins) CHECK(r.unpin()==Status::ok);
    CHECK(r.retire()==Status::ok);
    CHECK(r.acquire(Mode::read)==Status::retired);
    CHECK(r.acquire(Mode::write)==Status::retired);
    CHECK(r.pin()==Status::retired);
    std::printf("state_region_stress: PASS 500000 deterministic operations; accepted=%llu rejected=%llu\n",(unsigned long long)accepted,(unsigned long long)rejected);
}
