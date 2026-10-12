#include "oreslang/runtime/DopLayout.hpp"
#include <unordered_set>
#include <cstdint>
#include <limits>
#include <stdexcept>

void check(bool condition) {
    if (!condition) throw std::runtime_error("DOP native layout assertion failed");
}

template<class Exception, class F>
void expects(F&& f) {
    bool thrown = false;
    try { f(); } catch (const Exception&) { thrown = true; }
    check(thrown);
}

int main() {
    using namespace oreslang::dop;
    Plan aos(Layout::AoS, 3, 2, 4, 16, 1);
    Plan soa(Layout::SoA, 3, 2, 4, 16, 1);
    Plan aosoa(Layout::AoSoA, 5, 2, 4, 16, 4);
    check(aos.byte_length() == 48 && aos.byte_offset(2, 1) == 36);
    check(soa.byte_length() == 32 && soa.byte_offset(2, 1) == 24);
    check(aosoa.byte_length() == 64 && aosoa.byte_offset(4, 1) == 48);
    check(Plan(Layout::SoA, 0, 2, 4, 16, 1).byte_length() == 0);
    for (const auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        for (std::uint32_t fields = 1; fields <= 8; ++fields) {
            for (std::uint64_t rows = 0; rows <= 64; ++rows) {
                const std::uint32_t lanes = layout == Layout::AoSoA ? 4 : 1;
                Plan plan(layout, rows, fields, 4, 16, lanes);
                std::unordered_set<std::uint64_t> offsets;
                for (std::uint64_t row = 0; row < rows; ++row) {
                    for (std::uint32_t field = 0; field < fields; ++field) {
                        auto offset = plan.byte_offset(row, field);
                        check(offset <= plan.byte_length() && plan.byte_length() - offset >= 4);
                        check(offsets.insert(offset).second);
                    }
                }
            }
        }
    }
    expects<std::out_of_range>([&] { (void)soa.byte_offset(3, 0); });
    expects<std::out_of_range>([&] { (void)soa.byte_offset(0, 2); });
    expects<std::overflow_error>([] {
        (void)Plan(Layout::AoS, std::numeric_limits<std::uint64_t>::max(), 2, 8, 16, 1);
    });
    expects<std::overflow_error>([] {
        (void)Plan(Layout::SoA, std::numeric_limits<std::uint64_t>::max(), 2, 8, 16, 1);
    });
    expects<std::overflow_error>([] {
        (void)Plan(Layout::AoSoA, std::numeric_limits<std::uint64_t>::max(), 2, 8, 16, 4);
    });
    expects<std::invalid_argument>([] { (void)Plan(Layout::AoS, 3, 2, 4, 3, 1); });
    expects<std::invalid_argument>([] { (void)Plan(Layout::AoSoA, 3, 2, 4, 16, 3); });
    expects<std::invalid_argument>([] { (void)Plan(Layout::SoA, 3, 2, 4, 16, 4); });
}
