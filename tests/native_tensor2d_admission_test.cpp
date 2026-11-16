#include "oreslang/runtime/Tensor2DAdmission.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

using oreslang::runtime::Tensor2DAdmission;
using oreslang::runtime::TensorView2D;

static void require(bool condition) {
    if (!condition) throw std::logic_error("tensor2d reference contract mismatch");
}

int main() {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    require(Tensor2DAdmission::valid({3,4,4,1,8,96,true}));
    require(Tensor2DAdmission::contiguous({3,4,4,1,8,96,true}));
    require(Tensor2DAdmission::valid({3,4,8,1,4,80,true}));
    require(!Tensor2DAdmission::contiguous({3,4,8,1,4,80,true}));
    require(Tensor2DAdmission::valid({3,4,0,0,8,8,false}));
    require(!Tensor2DAdmission::valid({3,4,0,0,8,8,true}));
    require(!Tensor2DAdmission::valid({2,2,1,1,8,24,true}));
    require(!Tensor2DAdmission::valid({3,4,4,1,8,95,true}));
    require(!Tensor2DAdmission::valid({max,3,2,1,8,96,true}));
    require(Tensor2DAdmission::contiguous({0,4,4,1,8,0,true}));
    require(!Tensor2DAdmission::valid({-1,4,4,1,8,96,true}));
    require(Tensor2DAdmission::broadcasts(3,1,1,4));
    require(Tensor2DAdmission::broadcasts(0,3,1,3));
    require(!Tensor2DAdmission::broadcasts(3,4,5,4));
    require(!Tensor2DAdmission::valid({2,1,max,0,1,max,false}));
    require(Tensor2DAdmission::valid({1,1,0,0,max,max,false}));

    // Every accepted small writable view must be in-bounds and injective.
    std::uint64_t trials = 0;
    for (std::int64_t rows=0; rows<=4; ++rows)
    for (std::int64_t cols=0; cols<=4; ++cols)
    for (std::int64_t rs=0; rs<=6; ++rs)
    for (std::int64_t cs=0; cs<=6; ++cs)
    for (int writable=0; writable<=1; ++writable)
    for (auto capacity : {std::int64_t{0},std::int64_t{4},std::int64_t{8},
                          std::int64_t{32},std::int64_t{96},std::int64_t{256}}) {
        const TensorView2D v{rows,cols,rs,cs,4,capacity,writable!=0};
        if (Tensor2DAdmission::valid(v) && rows>0 && cols>0) {
            std::set<std::int64_t> offsets;
            for(std::int64_t row=0;row<rows;++row)
            for(std::int64_t col=0;col<cols;++col) {
                const auto pos=row*rs+col*cs;
                require(pos*4+4<=capacity);
                if(v.writable) require(offsets.insert(pos).second);
            }
        }
        ++trials;
    }
    require(trials==14700);
    return 0;
}
