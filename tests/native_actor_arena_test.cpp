#include "oreslang/runtime/ActorArena.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

using oreslang::runtime::ActorArena;
int main() {
    ActorArena a(1, 256), b(2, 256);
    assert(a.load_scalar(0) == 0 && b.load_scalar(0) == 0);
    a.store_scalar(0, INT64_MIN);
    b.store_scalar(0, INT64_MAX);
    assert(a.load_scalar(0) == INT64_MIN && b.load_scalar(0) == INT64_MAX);
    const auto pos = a.allocate(13, 8);
    assert(pos >= ActorArena::kScalarPrefix);
    a.write_byte(pos, 13, 12, 0xa5);
    assert(a.read_byte(pos, 13, 12) == 0xa5);
    try { a.read_byte(pos, 13, 13); assert(false); }
    catch (const std::out_of_range&) {}
    try { a.write_byte(0, 8, 0, 0xaa); assert(false); }
    catch (const std::out_of_range&) {}
    try { a.allocate(0); assert(false); }
    catch (const std::invalid_argument&) {}
    try { a.allocate(1, 3); assert(false); }
    catch (const std::invalid_argument&) {}
    const auto before = a.used_bytes();
    try { a.allocate(500); assert(false); }
    catch (const std::bad_alloc&) {}
    assert(a.used_bytes() == before);
    b.store_scalar(0, 99);
    assert(b.load_scalar(0) == 99);
    auto fresh = std::make_unique<ActorArena>(3, 256);
    assert(fresh->load_scalar(0) == 0);
    try { ActorArena forbidden(0, 256); assert(false); }
    catch (const std::invalid_argument&) {}
    try { ActorArena undersized(1, 64); assert(false); }
    catch (const std::invalid_argument&) {}
}
