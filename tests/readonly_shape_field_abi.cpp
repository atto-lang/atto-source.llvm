#include "../src/main/cpp/oreslang/types/ReadonlyShapeFieldAbi.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

using atto::shape::Error;
using atto::shape::Field;
using atto::shape::Layout;
using atto::shape::OwnerLease;
using atto::shape::Scalar;
using atto::shape::read_int;

constexpr Field dog_fields[] = {{"foo", Scalar::Int, 0, true, false, true}};
constexpr Field cat_fields[] = {
    {"age", Scalar::Int, 0, true, false, true},
    {"foo", Scalar::Int, 8, true, false, true}
};

static void put(std::span<std::byte> data, std::size_t pos, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        data[pos + i] = std::byte((v >> (8 * i)) & 0xff);
    }
}

int main() {
    std::array<std::byte, 16> dog{}, cat{};
    put(dog, 0, 41);
    put(cat, 0, 5);
    put(cat, 8, 42);
    OwnerLease dog_owner, cat_owner;
    Layout dog_layout{"Dog", dog_fields, false};
    Layout cat_layout{"Cat", cat_fields, false};
    const auto a = read_int(dog_layout, "foo", dog, dog_owner);
    const auto b = read_int(cat_layout, "foo", cat, cat_owner);
    assert(a.ok() && b.ok() && a.value + b.value == 83);
    assert(read_int(cat_layout, "foo", cat, cat_owner).value == 42);
    assert(read_int(cat_layout, "missing", cat, cat_owner).error == Error::MissingField);
    constexpr Field wrong[] = {{"foo", Scalar::Bool, 0, true, false, true}};
    assert(read_int(Layout{"Wrong",wrong,false},"foo",dog,dog_owner).error == Error::WrongType);
    constexpr Field priv[] = {{"foo", Scalar::Int, 0, false, false, true}};
    assert(read_int(Layout{"Private",priv,false},"foo",dog,dog_owner).error == Error::PrivateField);
    constexpr Field getter[] = {{"foo", Scalar::Int, 0, true, false, false}};
    assert(read_int(Layout{"Getter",getter,false},"foo",dog,dog_owner).error == Error::PrivateField);
    constexpr Field state[] = {{"foo", Scalar::Int, 0, true, true, true}};
    assert(read_int(Layout{"Actor",state,false},"foo",dog,dog_owner).error == Error::PrivateField);
    assert(read_int(Layout{"Actor",dog_fields,true},"foo",dog,dog_owner).error == Error::StateBorrow);
    constexpr Field bad_offset[] = {{"foo", Scalar::Int, 24, true, false, true}};
    assert(read_int(Layout{"Bad",bad_offset,false},"foo",dog,dog_owner).error == Error::Bounds);
    constexpr Field repeated[] = {
        {"foo", Scalar::Int, 0, true, false, true},
        {"foo", Scalar::Int, 8, true, false, true}
    };
    assert(read_int(Layout{"Duplicate",repeated,false},"foo",dog,dog_owner).error == Error::DuplicateField);
    constexpr Field alias[] = {
        {"age", Scalar::Int, 0, true, false, true},
        {"foo", Scalar::Int, 0, true, false, true}
    };
    assert(read_int(Layout{"Aliased",alias,false},"foo",dog,dog_owner).error == Error::InvalidLayout);
    assert(read_int(Layout{"",dog_fields,false},"foo",dog,dog_owner).error == Error::InvalidLayout);
    assert(read_int(dog_layout,"",dog,dog_owner).error == Error::InvalidLayout);
    dog_owner.moved = true;
    assert(read_int(dog_layout,"foo",dog,dog_owner).error == Error::Moved);
    std::cout << "LLVM_SHAPE_FIELD_WITNESS_PASS native=83 negatives=12\n";
}
