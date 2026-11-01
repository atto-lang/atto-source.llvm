#include "oreslang/runtime/FrozenTcpSlab.hpp"
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>
using oreslang::runtime::FrozenTcpSlab;

int main() {
    std::vector<std::uint8_t> source{0,1,127,128,255};
    FrozenTcpSlab slab(source);
    source.assign(source.size(),0);
    auto first=slab.try_pin();
    auto second=slab.try_pin();
    assert(first && second && slab.size()==5);
    assert(slab.read_byte(*first,3).value()==128);
    assert(slab.read_byte(*first,4).value()==255);
    assert(!slab.read_byte(*first,5));
    FrozenTcpSlab foreign(std::vector<std::uint8_t>{42});
    assert(!foreign.read_byte(*first,0));
    assert(!foreign.release_pin(*first));
    slab.retire();
    assert(!slab.try_pin());
    assert(!slab.freed());
    assert(slab.release_pin(*first));
    assert(!slab.release_pin(*first));
    assert(!slab.read_byte(*first,3));
    assert(slab.read_byte(*second,3).value()==128);
    assert(slab.release_pin(*second));
    assert(slab.freed() && slab.pin_count()==0);
    bool rejected=false;
    try {
        FrozenTcpSlab overflow(std::vector<std::uint8_t>(FrozenTcpSlab::max_bytes+1,0));
    } catch (const std::length_error&) { rejected=true; }
    assert(rejected);
    FrozenTcpSlab empty(std::vector<std::uint8_t>{});
    auto empty_pin=empty.try_pin();
    assert(empty_pin && !empty.read_byte(*empty_pin,0));
    empty.retire();
    assert(empty.release_pin(*empty_pin) && empty.freed());
    // RAII release and move preserve exactly one lifetime pin.
    FrozenTcpSlab moved(std::vector<std::uint8_t>{17});
    {
        auto original=moved.try_pin();
        assert(original);
        auto transferred=std::move(*original);
        assert(moved.read_byte(transferred,0).value()==17);
        moved.retire();
    }
    assert(moved.freed());
    return 0;
}
