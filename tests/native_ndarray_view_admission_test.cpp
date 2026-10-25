#include "oreslang/runtime/NativeNdArrayViewAdmission.hpp"
#include <cstdint>
#include <random>
#include <stdexcept>
#include <set>
#include <vector>

using oreslang::runtime::NativeNdArrayViewAdmission;
static void check(bool c) { if (!c) throw std::logic_error("NdArray alias invariant"); }
template<class Fn> static void denied(Fn fn) {
    bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
int main() {
    auto r = NativeNdArrayViewAdmission::check({3,2},{8,24},0,48,8,true);
    check(r.begin == 0 && r.end == 48);
    r = NativeNdArrayViewAdmission::check({3,2},{-8,24},16,48,8,true);
    check(r.begin == 0 && r.end == 48);
    r = NativeNdArrayViewAdmission::check({3},{0},0,8,8,false);
    check(r.begin == 0 && r.end == 8);
    denied([] { (void) NativeNdArrayViewAdmission::check({3},{0},0,8,8,true); });
    denied([] { (void) NativeNdArrayViewAdmission::check({2,2},{8,8},0,24,8,true); });
    denied([] { (void) NativeNdArrayViewAdmission::check({2,2},{-8,8},0,24,8,true); });
    denied([] { (void) NativeNdArrayViewAdmission::check({2,2},{1,8},0,48,8,false); });
    denied([] { (void) NativeNdArrayViewAdmission::check({2},{INT64_MAX},0,32,8,false); });
    denied([] { (void) NativeNdArrayViewAdmission::check({2},{INT64_MIN},0,32,8,false); });
    r = NativeNdArrayViewAdmission::check({0,3},{0,8},8,16,8,true);
    check(r.begin == 8 && r.end == 8);
    r = NativeNdArrayViewAdmission::check({}, {}, 0, 8, 8, true);
    check(r.begin == 0 && r.end == 8);

    std::mt19937 rng(419);
    std::uniform_int_distribution<int> rankgen(0, 4), dimgen(0, 3),
        stridegen(-4, 4), offgen(0, 20);
    std::size_t accepted = 0, rejected = 0;
    for (int trial=0; trial<6000; ++trial) {
        std::vector<std::uint64_t> shape;
        std::vector<std::int64_t> strides;
        const int rank = rankgen(rng);
        for(int j=0;j<rank;++j) { shape.push_back(dimgen(rng)); strides.push_back(stridegen(rng)*8); }
        const auto offset = static_cast<std::uint64_t>(offgen(rng)*8);
        NativeNdArrayViewAdmission::Range bound{};
        try { bound=NativeNdArrayViewAdmission::check(shape,strides,offset,256,8,true); }
        catch(const std::exception&) { ++rejected; continue; }
        ++accepted;
        std::set<std::int64_t> occupied;
        auto visit = [&](const auto& self, std::size_t index, std::int64_t pos) -> void {
            if(index == shape.size()) {
                check(pos >= 0 && static_cast<std::uint64_t>(pos) >= bound.begin);
                check(static_cast<std::uint64_t>(pos) + 8 <= bound.end);
                check(occupied.insert(pos).second);
                return;
            }
            for(std::uint64_t i=0;i<shape[index];++i)
                self(self,index+1,pos+static_cast<std::int64_t>(i)*strides[index]);
        };
        visit(visit,0,static_cast<std::int64_t>(offset));
    }
    check(accepted > 100 && rejected > 100);
}
