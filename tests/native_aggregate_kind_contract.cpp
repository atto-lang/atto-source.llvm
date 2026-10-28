#include "oreslang/types/NativeAggregateShape.hpp"

#include <cassert>
#include <stdexcept>

using oreslang::types::NativeAggregateShape;
int main() {
    NativeAggregateShape a{NativeAggregateShape::Kind::ExactStruct,
        {{"foo","int"},{"bar","bool"}}};
    NativeAggregateShape b{NativeAggregateShape::Kind::ReadonlyRecord,
        {{"bar","bool"},{"foo","int"}}};
    NativeAggregateShape c{NativeAggregateShape::Kind::ExactStruct,
        {{"bar","bool"},{"foo","int"}}};
    a.canonicalize(); b.canonicalize(); c.canonicalize();
    assert(a.same_type_as(c));
    assert(!a.same_type_as(b));
    assert(!b.permits_implicit_source(a));
    assert(!a.permits_implicit_source(b));
    bool duplicate = false;
    try {
        NativeAggregateShape bad{NativeAggregateShape::Kind::ExactStruct,
            {{"foo","int"},{"foo","int"}}};
        bad.canonicalize();
    } catch (const std::invalid_argument&) { duplicate = true; }
    assert(duplicate);
    bool opaque = false;
    try {
        NativeAggregateShape bad{NativeAggregateShape::Kind::ReadonlyRecord,
            {{"ptr","Pointer<Thing>"}}};
        bad.canonicalize();
    } catch (const std::invalid_argument&) { opaque = true; }
    assert(opaque);
    return 0;
}
