#include "oreslang/types/ThreadCaptureAdmission.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::types;
static void reject(const std::function<void()>& fn, const char* code) {
    try { fn(); } catch (const std::invalid_argument& ex) {
        assert(std::string(ex.what()) == code);
        return;
    }
    assert(!"expected rejection");
}
static AutoType scalar() { return AutoType{}; }
static AutoType wrapped(AutoTypeKind kind, AutoType child,
                        bool shared = false) {
    AutoType value;
    value.kind = kind;
    value.parameters = {std::move(child)};
    value.shareable_domain = shared;
    return value;
}
int main() {
    int boundary = 0, foreign_boundary = 0;
    const auto box = wrapped(AutoTypeKind::Box, scalar());
    const auto rc = wrapped(AutoTypeKind::Rc, scalar());
    const auto arc = wrapped(AutoTypeKind::Arc, scalar(), true);
    const auto denied_arc = wrapped(AutoTypeKind::Arc, scalar());
    AutoType dyn_send;
    dyn_send.kind = AutoTypeKind::Dyn;
    dyn_send.dyn_bounds = {AutoTrait::Send};
    AutoType dyn_unsafe;
    dyn_unsafe.kind = AutoTypeKind::Dyn;
    auto box_dyn = wrapped(AutoTypeKind::Box, dyn_send);
    auto box_dyn_bad = wrapped(AutoTypeKind::Box, dyn_unsafe);
    AutoType dyn_share;
    dyn_share.kind = AutoTypeKind::Dyn;
    dyn_share.shareable_domain = true;
    dyn_share.dyn_bounds = {AutoTrait::Send, AutoTrait::Sync, AutoTrait::Share};
    const auto arc_dyn = wrapped(AutoTypeKind::Arc, dyn_share, true);

    const auto approved = ThreadCaptureAdmission::admit(&boundary, 17, {
        {"scalar", ThreadCaptureOrigin::CopyScalar, scalar()},
        {"box", ThreadCaptureOrigin::MoveOwned, box},
        {"dyn", ThreadCaptureOrigin::MoveOwned, box_dyn},
        {"arc", ThreadCaptureOrigin::ShareArc, arc},
        {"arc_dyn", ThreadCaptureOrigin::ShareArc, arc_dyn},
    });
    approved.require(&boundary, 17);
    assert(approved.captures().size() == 5);
    assert(approved.captures().at(0).transfer == ThreadCaptureMode::Copy);
    assert(approved.captures().at(1).transfer == ThreadCaptureMode::Move);
    assert(approved.captures().at(3).transfer == ThreadCaptureMode::AtomicShare);

    reject([&] { approved.require(&foreign_boundary, 17); }, "thread_capture_wrong_boundary");
    reject([&] { approved.require(&boundary, 18); }, "thread_capture_stale_epoch");
    reject([&] { ThreadCaptureAdmission::admit(nullptr, 17, {}); }, "thread_invalid_boundary");
    reject([&] { ThreadCaptureAdmission::admit(&boundary, 0, {}); }, "thread_invalid_boundary");
    reject([&] { ThreadCaptureAdmission::admit(&boundary, 17, {
        {"x", ThreadCaptureOrigin::MoveOwned, box},
        {"x", ThreadCaptureOrigin::MoveOwned, box},
    }); }, "thread_invalid_capture_name");

    const auto invalid = [&](ThreadCaptureOrigin origin, AutoType type, const char* message) {
        reject([&] { ThreadCaptureAdmission::admit(&boundary, 17, {{"x", origin, type}}); }, message);
    };
    invalid(ThreadCaptureOrigin::BorrowShared, scalar(), "thread_capture_borrow");
    invalid(ThreadCaptureOrigin::BorrowMut, scalar(), "thread_capture_borrow");
    invalid(ThreadCaptureOrigin::ActorState, scalar(), "thread_capture_actor_state");
    invalid(ThreadCaptureOrigin::TaskLocal, scalar(), "thread_capture_execution_local");
    invalid(ThreadCaptureOrigin::ThreadLocal, scalar(), "thread_capture_execution_local");
    invalid(ThreadCaptureOrigin::ForeignHandle, scalar(), "thread_capture_foreign_handle");
    invalid(ThreadCaptureOrigin::Unknown, scalar(), "thread_capture_unknown_provenance");
    invalid(ThreadCaptureOrigin::MoveOwned, rc, "thread_capture_non_send");
    invalid(ThreadCaptureOrigin::MoveOwned, box_dyn_bad, "thread_capture_non_send");
    invalid(ThreadCaptureOrigin::CopyScalar, box, "thread_copy_requires_scalar");
    invalid(ThreadCaptureOrigin::ShareArc, box, "thread_share_requires_arc");
    invalid(ThreadCaptureOrigin::ShareArc, denied_arc, "thread_capture_non_send");
    invalid(ThreadCaptureOrigin::MoveOwned, arc, "thread_arc_requires_share_admission");
    auto unsafe_nominal = AutoType{};
    unsafe_nominal.kind = AutoTypeKind::Nominal;
    invalid(ThreadCaptureOrigin::MoveOwned, unsafe_nominal, "thread_capture_non_send");
    auto malformed = AutoType{};
    malformed.kind = AutoTypeKind::Box;
    invalid(ThreadCaptureOrigin::MoveOwned, malformed, "thread_capture_invalid_type_shape");

    // Shape/provenance adversarial checks. Previously malformed Scalar or Dyn
    // nodes could be accepted because their trait rules ignored children.
    auto scalar_with_hidden_rc = scalar();
    scalar_with_hidden_rc.parameters = {rc};
    invalid(ThreadCaptureOrigin::CopyScalar, scalar_with_hidden_rc,
            "thread_capture_invalid_type_shape");
    auto scalar_with_fake_dyn = scalar();
    scalar_with_fake_dyn.dyn_bounds = {AutoTrait::Send};
    invalid(ThreadCaptureOrigin::CopyScalar, scalar_with_fake_dyn,
            "thread_capture_invalid_type_shape");
    auto scalar_with_fake_layout = scalar();
    scalar_with_fake_layout.complete_nominal_layout = true;
    invalid(ThreadCaptureOrigin::CopyScalar, scalar_with_fake_layout,
            "thread_capture_invalid_type_shape");
    auto scalar_with_fake_domain = scalar();
    scalar_with_fake_domain.shareable_domain = true;
    invalid(ThreadCaptureOrigin::CopyScalar, scalar_with_fake_domain,
            "thread_capture_invalid_type_shape");

    auto dyn_with_hidden_rc = dyn_send;
    dyn_with_hidden_rc.parameters = {rc};
    invalid(ThreadCaptureOrigin::MoveOwned,
            wrapped(AutoTypeKind::Box, dyn_with_hidden_rc),
            "thread_capture_invalid_type_shape");
    auto dyn_with_fake_layout = dyn_send;
    dyn_with_fake_layout.complete_nominal_layout = true;
    invalid(ThreadCaptureOrigin::MoveOwned,
            wrapped(AutoTypeKind::Box, dyn_with_fake_layout),
            "thread_capture_invalid_type_shape");
    auto box_with_fake_domain = box;
    box_with_fake_domain.shareable_domain = true;
    invalid(ThreadCaptureOrigin::MoveOwned, box_with_fake_domain,
            "thread_capture_invalid_type_shape");
    auto box_with_fake_layout = box;
    box_with_fake_layout.complete_nominal_layout = true;
    invalid(ThreadCaptureOrigin::MoveOwned, box_with_fake_layout,
            "thread_capture_invalid_type_shape");

    auto arc_two_payloads = arc;
    arc_two_payloads.parameters.push_back(scalar());
    invalid(ThreadCaptureOrigin::ShareArc, arc_two_payloads,
            "thread_capture_invalid_type_shape");
    auto arc_fake_dyn = arc;
    arc_fake_dyn.dyn_bounds = {AutoTrait::Share};
    invalid(ThreadCaptureOrigin::ShareArc, arc_fake_dyn,
            "thread_capture_invalid_type_shape");
    auto rc_with_fake_domain = rc;
    rc_with_fake_domain.shareable_domain = true;
    invalid(ThreadCaptureOrigin::MoveOwned, rc_with_fake_domain,
            "thread_capture_invalid_type_shape");

    auto nominal_with_dyn = scalar();
    nominal_with_dyn.kind = AutoTypeKind::Nominal;
    nominal_with_dyn.complete_nominal_layout = true;
    nominal_with_dyn.dyn_bounds = {AutoTrait::Send};
    invalid(ThreadCaptureOrigin::MoveOwned, nominal_with_dyn,
            "thread_capture_invalid_type_shape");
    auto unknown_kind = scalar();
    unknown_kind.kind = static_cast<AutoTypeKind>(255);
    invalid(ThreadCaptureOrigin::MoveOwned, unknown_kind,
            "thread_capture_invalid_type_shape");
    auto unknown_trait = scalar();
    unknown_trait.negative.insert(static_cast<AutoTrait>(255));
    invalid(ThreadCaptureOrigin::CopyScalar, unknown_trait,
            "thread_capture_invalid_type_shape");

    AutoType too_deep = scalar();
    for (int i = 0; i < 68; ++i)
        too_deep = wrapped(AutoTypeKind::Box, std::move(too_deep));
    invalid(ThreadCaptureOrigin::MoveOwned, too_deep,
            "thread_capture_type_budget");
    auto too_wide = scalar();
    too_wide.kind = AutoTypeKind::Nominal;
    too_wide.complete_nominal_layout = true;
    too_wide.parameters.assign(4096, scalar());
    invalid(ThreadCaptureOrigin::MoveOwned, too_wide,
            "thread_capture_type_budget");


    std::cout << "native thread capture admission: OK (compiler-side model only)\n";
}
