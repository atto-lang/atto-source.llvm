#include "oreslang/types/TaskCaptureAdmission.hpp"

#include <cassert>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using oreslang::types::AutoTrait;
using oreslang::types::AutoType;
using oreslang::types::AutoTypeKind;
using oreslang::types::TaskCaptureAdmission;
using oreslang::types::TaskCaptureRequest;
using oreslang::types::TaskCaptureSource;
using oreslang::types::TaskCaptureTransfer;
using oreslang::types::TaskExecutionDomain;

namespace {

AutoType scalar() {
    AutoType out;
    out.kind = AutoTypeKind::Scalar;
    return out;
}

AutoType wrapped(AutoTypeKind kind, AutoType child, bool shareable = false) {
    AutoType out;
    out.kind = kind;
    out.parameters.push_back(std::move(child));
    out.shareable_domain = shareable;
    return out;
}

void rejects(const std::function<void()>& action, const std::string& fragment) {
    try {
        action();
        assert(false && "expected Task capture admission rejection");
    } catch (const std::invalid_argument& failure) {
        assert(std::string(failure.what()).find(fragment) != std::string::npos);
    }
}

} // namespace

int main() {
    int boundary_a = 0;
    int boundary_b = 0;

    auto admitted = TaskCaptureAdmission::admit(
        &boundary_a,
        7,
        TaskExecutionDomain::Cpu,
        {
            {"copy", TaskCaptureSource::CopyValue, scalar()},
            {"owned_box", TaskCaptureSource::OwnedValue,
                wrapped(AutoTypeKind::Box, scalar())},
            {"owned_refcell", TaskCaptureSource::OwnedValue,
                wrapped(AutoTypeKind::RefCell, scalar())},
        });

    assert(admitted.captures().size() == 3);
    assert(admitted.captures()[0].transfer == TaskCaptureTransfer::Copy);
    assert(admitted.captures()[1].transfer == TaskCaptureTransfer::Move);
    assert(admitted.captures()[2].transfer == TaskCaptureTransfer::Move);
    assert(admitted.captures()[0].capabilities.send);
    assert(admitted.captures()[1].capabilities.send);
    admitted.require(&boundary_a, 7, TaskExecutionDomain::Cpu);

    rejects(
        [&] { admitted.require(&boundary_b, 7, TaskExecutionDomain::Cpu); },
        "different AST boundary");
    rejects(
        [&] { admitted.require(&boundary_a, 8, TaskExecutionDomain::Cpu); },
        "stale");
    rejects(
        [&] { admitted.require(&boundary_a, 7, TaskExecutionDomain::Gpu); },
        "domain mismatch");

    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"loan", TaskCaptureSource::BorrowShared, scalar()}});
        },
        "borrowed Task capture");
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"loan", TaskCaptureSource::BorrowMut, scalar()}});
        },
        "borrowed Task capture");
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"self", TaskCaptureSource::ActorSelf, scalar()}});
        },
        "actor self");
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"opaque", TaskCaptureSource::Unknown, scalar()}});
        },
        "unknown");

    AutoType opaque_nominal;
    opaque_nominal.kind = AutoTypeKind::Nominal;
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"opaque_nominal", TaskCaptureSource::OwnedValue,
                    opaque_nominal}});
        },
        "Send proof");

    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"rc", TaskCaptureSource::OwnedValue,
                    wrapped(AutoTypeKind::Rc, scalar())}});
        },
        "Send proof");

    auto refcell = wrapped(AutoTypeKind::RefCell, scalar());
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {{"arc_refcell", TaskCaptureSource::OwnedValue,
                    wrapped(AutoTypeKind::Arc, refcell, true)}});
        },
        "Send proof");

    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 9, TaskExecutionDomain::Cpu,
                {
                    {"dup", TaskCaptureSource::CopyValue, scalar()},
                    {"dup", TaskCaptureSource::CopyValue, scalar()},
                });
        },
        "duplicate Task capture");

    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                nullptr, 9, TaskExecutionDomain::Cpu, {});
        },
        "exact boundary identity");
    rejects(
        [&] {
            TaskCaptureAdmission::admit(
                &boundary_a, 0, TaskExecutionDomain::Cpu, {});
        },
        "positive ownership epoch");

    return 0;
}
