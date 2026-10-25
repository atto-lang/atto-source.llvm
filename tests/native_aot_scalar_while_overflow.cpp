#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

extern "C" int oreslang_rt_spin_init(std::int64_t*) noexcept;
extern "C" int oreslang_rt_spin_resume(std::int64_t*, std::int64_t) noexcept;
extern "C" int oreslang_rt_spin_cancel(std::int64_t*) noexcept;

static void check(bool value) {
    if (!value) {
        std::fputs("AOT_OVERFLOW_SOURCE_PARITY_FAIL\n", stderr);
        std::abort();
    }
}
int main() {
    std::int64_t frame[2] = {0, 0};
    check(oreslang_rt_spin_init(frame) == 1);
    check(frame[1] == std::numeric_limits<std::int64_t>::max() - 1);
    check(oreslang_rt_spin_resume(frame, 1) == 1);
    check(frame[1] == std::numeric_limits<std::int64_t>::max());
    check(oreslang_rt_spin_resume(frame, 1) == 1);
    check(frame[1] == std::numeric_limits<std::int64_t>::min());
    check(oreslang_rt_spin_resume(frame, 8) == 1);
    check(frame[1] == std::numeric_limits<std::int64_t>::min() + 8);
    check(oreslang_rt_spin_cancel(frame) == 1);
    check(frame[0] == 2 && frame[1] == 0);
    std::puts("AOT_I64_OVERFLOW_PARITY_PASS");
}
