#include "oreslang/types/NativeAggregateSpreadPlan.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace {
thread_local long countdown = -1;
}
void *operator new(std::size_t n) {
    if (countdown >= 0 && countdown-- == 0) throw std::bad_alloc();
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

using oreslang::types::NativeAggregateSpreadPlan;
using oreslang::types::NativeAggregateShape;
using Kind = NativeAggregateShape::Kind;

static bool intact(const NativeAggregateSpreadPlan &plan) {
    if (plan.source_elements() != 1 || plan.source_ordered_writes().size() != 1)
        return false;
    const auto &w = plan.source_ordered_writes().front();
    if (w.field != "existing" || w.type != "int" || w.source_element != 0)
        return false;
    auto shape = plan.result_shape();
    return shape.fields.size() == 1 && shape.fields[0].name == "existing";
}
int main() {
    int checked=0;
    for (const int mode : {0, 1}) {
        for (int failpoint=0; failpoint < 96; ++failpoint) {
            NativeAggregateSpreadPlan plan(Kind::ReadonlyRecord);
            plan.add_named("existing", "int");
            NativeAggregateShape projected{Kind::ExactStruct, {
                {"large_new_field_with_name_0123456789", "int"},
                {"other_large_new_field_with_name_0123456789", "bool"},
                {"third_large_new_field_with_name_0123456789", "string"}
            }};
            bool failed=false;
            try {
                countdown=failpoint;
                if (mode == 0) {
                    plan.add_named("added_field_with_long_name_1234567890", "string");
                } else {
                    plan.add_checked_spread_projection(std::move(projected));
                }
            } catch(const std::bad_alloc&) { failed=true; }
            countdown=-1;
            if(failed) {
                if(!intact(plan)) {
                    std::fprintf(stderr,"PARTIAL STATE after bad_alloc mode %d allocation %d: elements=%zu writes=%zu\n",
                        mode,failpoint,plan.source_elements(),plan.source_ordered_writes().size());
                    return 1;
                }
                // A failure must not poison explicit-name tracking or the
                // next source-element number. The same operation must still
                // be accepted when the fault is removed.
                if (mode == 0) {
                    plan.add_named("added_field_with_long_name_1234567890", "string");
                    if (plan.source_elements() != 2 ||
                        plan.source_ordered_writes().size() != 2 ||
                        plan.source_ordered_writes().back().source_element != 1)
                        return 3;
                } else {
                    NativeAggregateShape retry{Kind::ExactStruct, {
                        {"large_new_field_with_name_0123456789", "int"},
                        {"other_large_new_field_with_name_0123456789", "bool"},
                        {"third_large_new_field_with_name_0123456789", "string"}
                    }};
                    plan.add_checked_spread_projection(std::move(retry));
                    if (plan.source_elements() != 2 ||
                        plan.source_ordered_writes().size() != 4 ||
                        plan.source_ordered_writes().back().source_element != 1)
                        return 4;
                }
                checked++;
            }
        }
    }
    if(checked < 5) return 2;
    std::printf("Strong exception guarantee across %d induced allocation failures\n",checked);
}
