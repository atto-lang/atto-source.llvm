#include "oreslang/types/DopPhysicalLayoutV1.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace oreslang::dop::physical_v1;
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <typename Error, typename F> void rejected(F&& run) {
    bool thrown = false;
    try { run(); } catch (const Error&) { thrown = true; }
    require(thrown, "native typed layout negative case was admitted");
}
using Expected = std::array<std::array<std::uint64_t,3>,3>;
struct Vector {
    LayoutKind kind;
    std::uint32_t lanes;
    std::uint64_t bytes;
    std::size_t segments;
    Expected offsets;
};
constexpr std::array<Vector,4> vectors{{
    {LayoutKind::aos,1,120,1,{{{{0,8,16}},{{72,80,88}},{{96,104,112}}}}},
    {LayoutKind::soa,1,74,3,{{{{0,24,64}},{{12,48,70}},{{16,56,72}}}}},
    {LayoutKind::aosoa,4,112,1,{{{{0,16,48}},{{12,40,54}},{{56,72,104}}}}},
    {LayoutKind::soaos,1,80,2,{{{{0,40,4}},{{24,64,28}},{{32,72,36}}}}}
}};
std::vector<TypedField> fields() {
    return {{"position",4,4},{"velocity",8,8},{"flags",2,2}};
}
std::vector<std::vector<std::uint32_t>> groups(LayoutKind kind) {
    return kind == LayoutKind::soaos ?
            std::vector<std::vector<std::uint32_t>>{{0,2},{1}} :
            std::vector<std::vector<std::uint32_t>>{};
}
void verify(const Vector& vec) {
    TypedPhysicalPlan p(vec.kind,5,fields(),vec.lanes,groups(vec.kind));
    require(p.physical_bytes() == vec.bytes, "fixed physical byte size differs");
    require(p.segments().size() == vec.segments, "fixed segment count differs");
    const std::array<std::uint64_t,3> samples{{0,3,4}};
    for (std::size_t r = 0; r < samples.size(); ++r)
        for (std::uint32_t field = 0; field < 3; ++field) {
            const auto cell=p.lower_cell_address(samples[r],field);
            require(cell.absolute_offset == vec.offsets[r][field],
                    "typed offset differs from shared golden vectors");
        }
    // Actually address a bounded, mutable physical native byte allocation.
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(p.physical_bytes()), 0);
    std::unordered_set<std::uint64_t> occupied;
    const std::array<std::uint32_t,3> widths{{4,8,2}};
    for (std::uint64_t row=0;row<5;++row)
        for (std::uint32_t field=0;field<3;++field) {
            const auto a=p.lower_cell_address(row,field).absolute_offset;
            const std::uint64_t value=1000 + row*100 + field;
            require(a+widths[field]<=buffer.size(),"native physical cell exceeded buffer");
            for (std::uint32_t k=0;k<widths[field];++k) {
                require(occupied.insert(a+k).second, "two typed native cells overlap");
                buffer[static_cast<std::size_t>(a+k)] =
                    static_cast<std::uint8_t>((value>>(8*k)) & 0xffU);
            }
        }
    for (std::uint64_t row=0;row<5;++row)
        for (std::uint32_t field=0;field<3;++field) {
            const auto a=p.lower_cell_address(row,field).absolute_offset;
            std::uint64_t got=0;
            for (std::uint32_t k=0;k<widths[field];++k)
                got |= static_cast<std::uint64_t>(buffer[
                    static_cast<std::size_t>(a+k)])<<(8*k);
            require(got==1000+row*100+field,"native scalar byte corruption");
        }
    rejected<std::out_of_range>([&]{(void)p.lower_cell_address(5,0);});
    rejected<std::out_of_range>([&]{(void)p.lower_cell_address(0,3);});
}
}
void check_unicode_names() {
    std::string emoji;
    for (int i=0; i<256; ++i) emoji += "\xF0\x9F\x92\xBE";
    for (const auto& name : {std::string(" "), std::string("β"),
                             std::string("é"), std::string("e\xCC\x81"),
                             std::string(1024, 'a'), emoji}) {
        TypedPhysicalPlan p(LayoutKind::soa, 1, {{name,4,4}});
        require(p.lower_cell_address(0,0).absolute_offset == 0,
                "canonical Unicode scalar field name rejected");
    }
    TypedPhysicalPlan distinct(LayoutKind::aos, 1,
        {{"\xC3\xA9",2,2},{"e\xCC\x81",4,4}});
    require(distinct.lower_cell_address(0,0).absolute_offset !=
            distinct.lower_cell_address(0,1).absolute_offset,
            "canonical-equivalent Unicode names were silently normalized");
    std::string oversize;
    for (int i=0; i<257; ++i) oversize += "\xF0\x9F\x92\xBE";
    std::vector<std::string> invalid = {
        "", std::string(1, '\0'), std::string(1025,'a'),
        oversize, "\xC0\xAF", "\xED\xA0\x80",
        "\xF4\x90\x80\x80", "\xF0\x9F", "\x80",
        "\xE0\x80\xAF", "\xF8\x88\x80\x80\x80"
    };
    for (const auto& name : invalid) {
        rejected<std::invalid_argument>([&] {
            (void)TypedPhysicalPlan(LayoutKind::aos,1,{{name,4,4}});
        });
    }
    rejected<std::invalid_argument>([] {
        (void)TypedPhysicalPlan(LayoutKind::aos,1,
            {{"\xF0\x9F\x92\xBE",4,4},{"\xF0\x9F\x92\xBE",4,4}});
    });
}

void check_tail_alignment_geometry() {
    const std::vector<TypedField> specs = {
        {"u8",1,1},{"u16",2,4},{"u32",4,8},{"u64",8,16}};
    const std::array<LayoutKind,4> kinds = {
        LayoutKind::aos,LayoutKind::soa,LayoutKind::aosoa,LayoutKind::soaos};
    for (const auto rows : std::array<std::uint64_t,13>{
            0,1,2,3,4,5,7,8,9,17,31,32,33}) {
        for (const auto kind : kinds) {
            const auto lanes = kind == LayoutKind::aosoa ? 4U : 1U;
            const std::vector<std::vector<std::uint32_t>> groups =
                kind == LayoutKind::soaos
                    ? std::vector<std::vector<std::uint32_t>>{{0,2},{3,1}}
                    : std::vector<std::vector<std::uint32_t>>{};
            TypedPhysicalPlan p(kind,rows,specs,lanes,groups,100000);
            std::unordered_set<std::uint64_t> used;
            for (std::uint64_t row=0;row<rows;++row) {
                for (std::uint32_t field=0;field<specs.size();++field) {
                    const auto a=p.lower_cell_address(row,field);
                    const auto width=specs[field].bytes;
                    require(a.absolute_offset % specs[field].alignment == 0,
                            "typed physical scalar is misaligned");
                    require(a.relative_offset <= p.segments().at(a.segment).bytes &&
                            width <= p.segments().at(a.segment).bytes - a.relative_offset,
                            "typed physical cell crosses segment");
                    require(a.absolute_offset <= p.physical_bytes() &&
                            width <= p.physical_bytes() - a.absolute_offset,
                            "typed physical cell outside allocation");
                    for (std::uint32_t i=0;i<width;++i)
                        require(used.insert(a.absolute_offset+i).second,
                                "physical cells overlap");
                }
            }
            if (rows==0) require(p.physical_bytes()==0,"zero-row allocation");
            else require(p.physical_bytes()>=rows*15,"physical bytes lost");
        }
    }
}

int main() {
    check_unicode_names();
    check_tail_alignment_geometry();
    for(const auto& vec:vectors) verify(vec);
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::soaos,5,fields(),1,{{0,2}});});
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::soaos,5,fields(),1,{{0,2},{2,1}});});
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::aos,5,fields(),1,{{0,1,2}});});
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::aosoa,5,fields(),3);});
    rejected<std::length_error>([]{(void)TypedPhysicalPlan(
        LayoutKind::aos,5,fields(),1,{},119);});
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::aos,std::numeric_limits<std::uint64_t>::max(),fields());});
    rejected<std::invalid_argument>([]{(void)TypedPhysicalPlan(
        LayoutKind::aos,5,{{"a",4,4},{"a",8,8}});});
    std::cout << "DOP_NATIVE_PHYSICAL_LAYOUT_V1_HOST_PASS layouts=4 rows=5"
                 " typed=3 no_guest_compiler_claim=true\n";
}
