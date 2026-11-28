#include "oreslang/runtime/DopAotLayoutTrace.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

using namespace oreslang::dop;

static_assert(!std::is_copy_constructible_v<DopAotLayoutTrace>);
static_assert(!std::is_move_constructible_v<DopAotLayoutTrace>);

int main() {
    bool invalid = false;
    try { DopAotLayoutTrace empty(1, 0); } catch (const std::invalid_argument&) { invalid = true; }
    assert(invalid);
    invalid = false;
    try { DopAotLayoutTrace too_many(1, 65); } catch (const std::invalid_argument&) { invalid = true; }
    assert(invalid);

    DopAotLayoutTrace off(10, 3);
    assert(!off.observe(0, 0));
    assert(off.samples() == 0);
    assert(off.hint() == LayoutTraceHint::Insufficient);
    assert(off.configure(true));
    assert(!off.observe(10, 0));
    assert(!off.observe(0, 3));
    assert(off.samples() == 0);
    for (std::uint64_t row = 0; row < 10; ++row) assert(off.observe(row, 0));
    assert(off.samples() == 10);
    assert(off.field_hits(0) == 10);
    assert(off.field_hits(1) == 0);
    assert(off.column_scan_transitions() == 9);
    assert(off.hint() == LayoutTraceHint::SoA);
    assert(off.configure(false));
    assert(!off.observe(0, 1));
    assert(off.samples() == 10);  // disable is lossless; reset is explicit
    assert(off.configure(true));
    off.reset();
    assert(off.samples() == 0);
    for (std::uint64_t row = 0; row < 10; ++row) {
        for (std::uint32_t field = 0; field < 3; ++field) assert(off.observe(row, field));
    }
    assert(off.samples() == 30);
    assert(off.row_local_transitions() == 20);
    assert(off.hint() == LayoutTraceHint::AoS);
    invalid = false;
    try { (void)off.field_hits(3); } catch (const std::out_of_range&) { invalid = true; }
    assert(invalid);

    off.reset();
    assert(off.observe(0, 0));
    assert(off.observe(0, 1));
    assert(off.observe(1, 1));
    assert(off.observe(1, 2));
    assert(off.observe(2, 2));
    assert(off.hint() == LayoutTraceHint::Insufficient);

    // A single-row region cannot pretend to have a column scan.
    DopAotLayoutTrace single(1, 3, true);
    assert(single.observe(0, 0));
    assert(single.observe(0, 1));
    assert(single.observe(0, 2));
    assert(single.hint() == LayoutTraceHint::AoS);

    // Sealed AOT/hard-RT mode rejects mutations even when probes are enabled.
    DopAotLayoutTrace sealed(5, 2, true, true);
    assert(!sealed.configure(false));
    assert(sealed.enabled());
    assert(sealed.observe(0, 0));
    assert(sealed.samples() == 1);
    DopAotLayoutTrace sealed_off(5, 2, false, true);
    assert(!sealed_off.configure(true));
    assert(!sealed_off.observe(0, 0));
    assert(sealed_off.samples() == 0);

    // Large valid indices must not wrap, out-of-range probes do not alter history.
    DopAotLayoutTrace huge(UINT64_MAX, 2, true);
    assert(huge.observe(UINT64_MAX - 3, 1));
    assert(!huge.observe(UINT64_MAX, 1));
    assert(huge.observe(UINT64_MAX - 2, 1));
    assert(huge.column_scan_transitions() == 1);
    assert(huge.observe(UINT64_MAX - 1, 1));
    assert(huge.hint() == LayoutTraceHint::SoA);
    return 0;
}
