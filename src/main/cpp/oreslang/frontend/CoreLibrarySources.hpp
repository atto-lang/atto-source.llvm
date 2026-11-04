#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <cstdint>
#include <string_view>

namespace oreslang::frontend {

enum class CoreProviderRequirement : std::uint32_t {
    Allocator = 1u << 0,
    Executor = 1u << 1,
    Mailbox = 1u << 2,
    TrustedControl = 1u << 3,
    MonotonicClock = 1u << 4
};

enum class CompilerLibraryTier : std::uint8_t {
    Core,
    Alloc,
    Std // Hosted scalar assertions, not full TestSuite
};

constexpr std::uint32_t core_provider_bit(CoreProviderRequirement requirement) noexcept {
    return static_cast<std::uint32_t>(requirement);
}

struct CoreClosureRequirements {
    std::uint32_t provider_bits = 0;
    bool hosted_or_unknown_effect = false;
};

inline CoreClosureRequirements merge_core_closure_requirements(
        CoreClosureRequirements local,
        CoreClosureRequirements dependency) noexcept {
    return CoreClosureRequirements{
        local.provider_bits | dependency.provider_bits,
        local.hosted_or_unknown_effect || dependency.hosted_or_unknown_effect
    };
}

inline bool core_closure_admitted(
        CoreClosureRequirements requirements,
        std::uint32_t admitted_provider_bits) noexcept {
    return !requirements.hosted_or_unknown_effect &&
        (requirements.provider_bits & ~admitted_provider_bits) == 0u;
}

struct CoreLibrarySource {
    CompilerLibraryTier tier = CompilerLibraryTier::Core;
    std::string_view canonical_import;
    std::string_view module_name;
    std::string_view unit_id;
    std::string_view source;
    std::uint32_t admitted_provider_bits = 0;
};

inline bool is_reserved_core_import(std::string_view raw) noexcept {
    return raw == "ores.core" || raw.rfind("ores.core/", 0) == 0;
}

inline bool is_reserved_std_import(std::string_view raw) noexcept {
    return raw == "ores.std" || raw.rfind("ores.std/", 0) == 0 ||
        raw == "std" || raw.rfind("std/", 0) == 0;
}

inline bool is_reserved_alloc_import(std::string_view raw) noexcept {
    return raw == "alloc" || raw.rfind("alloc/", 0) == 0;
}

inline bool is_reserved_compiler_library_import(std::string_view raw) noexcept {
    // Admit only hosted scalar helpers and bounded M1/M2 test-ledger modules.
    // The full ores.std/testing callable TestSuite remains fail-closed.
    return is_reserved_core_import(raw) || is_reserved_alloc_import(raw) ||
        raw == "ores.std/testing/scalar" ||
        raw == "ores.std/testing/scalar.atto" ||
        raw == "ores.std/testing/scalar.ores" ||
        raw == "ores.std/testing/m1" ||
        raw == "ores.std/testing/m1.atto" ||
        raw == "ores.std/testing/m1.ores" ||
        raw == "ores.std/testing/m2" ||
        raw == "ores.std/testing/m2.atto" ||
        raw == "ores.std/testing/m2.ores";
}

inline bool compiler_library_dependency_allowed(
        CompilerLibraryTier importer,
        CompilerLibraryTier dependency) noexcept {
    if (importer == CompilerLibraryTier::Std) return true;
    if (importer == CompilerLibraryTier::Alloc)
        return dependency != CompilerLibraryTier::Std;
    return dependency == CompilerLibraryTier::Core;
}

inline const CoreLibrarySource* find_compiler_library_source(std::string_view raw) noexcept {
    // Keep aliases identity-stable: the optional .ores suffix selects the same
    // compiler-owned unit and can never create a duplicate module instance.
    static constexpr std::string_view kNumericSource =
R"ORES_CORE(// Compiler-owned ores.core source.
// This module has no OS, Java, filesystem, network, process, actor-scheduler,
// or ambient project dependency.

define module numeric as

  pub fnc min_int(int left, int right): int {
    if left < right; do
      return left;
    fi
    return right;
  }

  pub fnc max_int(int left, int right): int {
    if left > right; do
      return left;
    fi
    return right;
  }

  pub fnc clamp_int(int value, int lower, int upper): int {
    if lower > upper; do
      return lower;
    fi
    if value < lower; do
      return lower;
    fi
    if value > upper; do
      return upper;
    fi
    return value;
  }

end
)ORES_CORE";
    static constexpr CoreLibrarySource kNumeric{
        CompilerLibraryTier::Core,
        "ores.core/numeric",
        "numeric",
        "@ores.core/numeric.ores",
        kNumericSource,
        0u
    };

    static constexpr std::string_view kIntBufferSource =
R"ORES_ALLOC(// Compiler-owned alloc tier source.
// This module requires an allocator provider but no OS, filesystem, network,
// process, actor scheduler, or ambient project dependency.

define module int_buffer as

  pub fnc sum_pair(int left, int right): int {
    let mut Array<int> values = new Array<int>();
    values.add(left);
    values.add(right);
    return values.get(0) + values.get(1);
  }

end
)ORES_ALLOC";
    static constexpr CoreLibrarySource kIntBuffer{
        CompilerLibraryTier::Alloc,
        "alloc/int_buffer",
        "int_buffer",
        "@alloc/int_buffer.ores",
        kIntBufferSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kIntBufferBundleSource =
R"ORES_ALLOC(// Compiler-owned alloc tier transitive-closure probe.
// The local body is allocation-free, but linking this module brings in the
// allocator-bearing int_buffer dependency and therefore requires Allocator.

import module int_buffer from "alloc/int_buffer";

define module int_buffer_bundle as

  pub fnc marker(): int {
    return 42;
  }

end
)ORES_ALLOC";
    static constexpr CoreLibrarySource kIntBufferBundle{
        CompilerLibraryTier::Alloc,
        "alloc/int_buffer_bundle",
        "int_buffer_bundle",
        "@alloc/int_buffer_bundle.ores",
        kIntBufferBundleSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kScalarTestingSource =
R"ATTO_STD(// Compiler-owned pure scalar-test helpers for hosted LLVM.
// Return 0 for a satisfied assertion, 1 otherwise. No IO, alloc or reflection.
define module testing_scalar as
  pub fnc expect_equal_int(int actual, int expected): int { return (actual == expected) ? 0 : 1; }
  pub fnc expect_not_equal_int(int actual, int expected): int { return (actual != expected) ? 0 : 1; }
  pub fnc expect_true(bool actual): int { return (actual) ? 0 : 1; }
  pub fnc expect_false(bool actual): int { return (!actual) ? 0 : 1; }
  pub fnc expect_less_int(int actual, int limit): int { return (actual < limit) ? 0 : 1; }
  pub fnc expect_greater_int(int actual, int limit): int { return (actual > limit) ? 0 : 1; }
  pub fnc expect_equal_bool(bool actual, bool expected): int { return (actual == expected) ? 0 : 1; }
  pub fnc expect_range_int(int actual, int lower, int upper): int { return (actual >= lower && actual <= upper) ? 0 : 1; }
end
)ATTO_STD";
    static constexpr CoreLibrarySource kScalarTesting{
        CompilerLibraryTier::Std,
        "ores.std/testing/scalar",
        "testing_scalar",
        "@ores.std/testing/scalar.atto",
        kScalarTestingSource,
        0u
    };
    static constexpr std::string_view kTestingM1Source =
R"ATTO_M1(// AttoLang LLVM M1 bounded hosted scalar ledger; no closures or host test bodies.
// Canonical compiler-owned source identity: ores.std/testing/m1.atto.
// A TestCase is a VALUE DESCRIPTION: the caller must evaluate its body only if
// should_run() returns true, then set outcome 0=PASS or 1=FAIL.
// This is NOT the full ore.std/testing TestBody or nested TestSuite API.
define module testing_m1 as
  pub define class TestCase as
    pub let int id = 0;
    pub let int outcome = 0;
    pub let bool selected = true;
    pub let bool skipped = false;
  end

  pub define class TestSuite as
    pub let int total = 0;
    pub let int passed = 0;
    pub let int failed = 0;
    pub let int skipped = 0;
    pub let int filtered = 0;
    pub let int halted = 0;
    pub let bool fail_fast = false;
  end

  pub define class RunSummary as
    pub let int total = 0;
    pub let int passed = 0;
    pub let int failed = 0;
    pub let int skipped = 0;
    pub let int filtered = 0;
  end

  pub fnc should_run(borrow TestSuite suite, borrow TestCase candidate): bool {
    return suite.halted == 0 && candidate.selected && !candidate.skipped;
  }

  // 0 PASS, 1 FAIL, 2 SKIP, 3 FILTER, 4 NOT_RUN after fail-fast;
  // -1 invalid descriptor, -2 quota exceeded. Both errors are atomic.
  pub fnc record(borrow mut TestSuite suite, borrow TestCase candidate): int {
    if suite.halted != 0; then
      return 4;
    fi
    if candidate.id <= 0 || candidate.outcome < 0 || candidate.outcome > 1; then
      return -1;
    fi
    if !candidate.selected; then
      if suite.filtered >= 1024; then
        return -2;
      fi
      suite.filtered = suite.filtered + 1;
      return 3;
    fi
    if suite.total >= 1024; then
      return -2;
    fi
    suite.total = suite.total + 1;
    if candidate.skipped; then
      suite.skipped = suite.skipped + 1;
      return 2;
    fi
    if candidate.outcome == 0; then
      suite.passed = suite.passed + 1;
      return 0;
    fi
    suite.failed = suite.failed + 1;
    if suite.fail_fast; then
      suite.halted = 1;
    fi
    return 1;
  }

  // Returns nonzero on empty/skipped/filtered-only suites or any failure.
  // The caller must never treat a failed/empty summary as a green test run.
  pub fnc finish(borrow TestSuite suite, borrow mut RunSummary result): int {
    result.total = suite.total;
    result.passed = suite.passed;
    result.failed = suite.failed;
    result.skipped = suite.skipped;
    result.filtered = suite.filtered;
    if result.total != result.passed + result.failed + result.skipped; then
      return 2;
    fi
    if result.passed + result.failed == 0; then
      return 1;
    fi
    if result.failed > 0; then
      return 1;
    fi
    return 0;
  }
end
)ATTO_M1";
    static constexpr CoreLibrarySource kTestingM1{
        CompilerLibraryTier::Std,
        "ores.std/testing/m1",
        "testing_m1",
        "@ores.std/testing/m1.atto",
        kTestingM1Source,
        0u
    };
    static constexpr std::string_view kTestingM2Source =
R"ATTO_M2(// LLVM M2: deterministic, bounded, guest-owned registered scalar test bodies.
// This is NOT a general function value, closure, or full std/testing TestBody.
// Body opcodes are registry-local pure scalar operations, NOT user functions.
// No actor messages, reflection, host dispatch, indirect function calls, or I/O.
import module Ledger from "ores.std/testing/m1";
import module Scalar from "ores.std/testing/scalar";
import class TestSuite from "ores.std/testing/m1.atto";
import class RunSummary from "ores.std/testing/m1.ores";
import class TestCase from "ores.std/testing/m1";

define module testing_m2 as
  pub define class RegisteredCase as
    pub let int id = 0;
    pub let int body_kind = 0;
    pub let int actual = 0;
    pub let int expected = 0;
    pub let bool selected = true;
    pub let bool skipped = false;
    pub let int invocations = 0;
    pub let int visited = 0;
    pub let int outcome = 0;
  end

  // Stable first M2 scalar operation IDs: 1 equality, 2 inequality,
  // 3 strictly less, 4 strictly greater. Unknown opcodes fail closed.
  // A body can be reached only after the caller's selection/skip gates.
  pub fnc invoke_scalar_body(borrow RegisteredCase candidate): int {
    if candidate.body_kind == 1; then
      return Scalar.expect_equal_int(candidate.actual, candidate.expected);
    fi
    if candidate.body_kind == 2; then
      return Scalar.expect_not_equal_int(candidate.actual, candidate.expected);
    fi
    if candidate.body_kind == 3; then
      return Scalar.expect_less_int(candidate.actual, candidate.expected);
    fi
    if candidate.body_kind == 4; then
      return Scalar.expect_greater_int(candidate.actual, candidate.expected);
    fi
    return -3;
  }

  // Returns 0 PASS, 1 FAIL, 2 SKIP, 3 FILTER, 4 NOT_RUN;
  // -1 malformed ID, -2 capacity, -3 unknown body, -4 duplicate admission.
  // Skipped/filtered cases never call invoke_scalar_body.
  pub fnc run_one(borrow mut TestSuite suite, borrow mut RegisteredCase candidate): int {
    // Native LLVM requires an owning class local before structured branches.
    let mut TestCase snapshot = new TestCase();
    if suite.halted != 0; then
      return 4;
    fi
    if candidate.visited != 0; then
      return -4;
    fi
    if candidate.id <= 0 || candidate.invocations != 0; then
      return -1;
    fi

    snapshot.id = candidate.id;
    snapshot.selected = candidate.selected;
    snapshot.skipped = candidate.skipped;
    if !candidate.selected || candidate.skipped; then
      val int ignored = Ledger.record(rt mut borrow suite, rt borrow snapshot);
      if ignored == 2 || ignored == 3; then
        candidate.visited = 1;
      fi
      return ignored;
    fi

    // Do not execute the body when the ledger would reject quota.
    if suite.total >= 1024; then
      return -2;
    fi
    val int outcome = invoke_scalar_body(rt borrow candidate);
    if outcome != 0 && outcome != 1; then
      return -3;
    fi
    snapshot.outcome = outcome;
    val int status = Ledger.record(rt mut borrow suite, rt borrow snapshot);
    if status == 0 || status == 1; then
      candidate.outcome = outcome;
      candidate.invocations = 1;
      candidate.visited = 1;
    fi
    return status;
  }

  // Two is an explicit bounded v1 capacity. Reject duplicate IDs before
  // either callback; stop after fail-fast or an admission error.
  // The return is the final guest OS result; callers own receipt formatting.
  pub fnc run_pair(
    borrow mut TestSuite suite,
    borrow mut RegisteredCase first,
    borrow mut RegisteredCase second,
    borrow mut RunSummary result
  ): int {
    if first.id == second.id; then
      return -5;
    fi
    val int first_status = run_one(rt mut borrow suite, rt mut borrow first);
    if first_status < 0; then
      return first_status;
    fi
    val int second_status = run_one(rt mut borrow suite, rt mut borrow second);
    if second_status < 0; then
      return second_status;
    fi
    return Ledger.finish(rt borrow suite, rt mut borrow result);
  }
end
)ATTO_M2";
    static constexpr CoreLibrarySource kTestingM2{
        CompilerLibraryTier::Std,
        "ores.std/testing/m2",
        "testing_m2",
        "@ores.std/testing/m2.atto",
        kTestingM2Source,
        0u
    };
    if (raw == "ores.std/testing/m2" ||
        raw == "ores.std/testing/m2.atto" ||
        raw == "ores.std/testing/m2.ores")
        return &kTestingM2;

    if (raw == "ores.std/testing/m1" ||
        raw == "ores.std/testing/m1.atto" ||
        raw == "ores.std/testing/m1.ores")
        return &kTestingM1;

    if (raw == "ores.std/testing/scalar" ||
        raw == "ores.std/testing/scalar.atto" ||
        raw == "ores.std/testing/scalar.ores")
        return &kScalarTesting;
    if (raw == "ores.core/numeric" || raw == "ores.core/numeric.ores")
        return &kNumeric;
    if (raw == "alloc/int_buffer" || raw == "alloc/int_buffer.ores")
        return &kIntBuffer;
    if (raw == "alloc/int_buffer_bundle" ||
            raw == "alloc/int_buffer_bundle.ores")
        return &kIntBufferBundle;
    return nullptr;
}

inline const CoreLibrarySource* find_core_library_source(
        std::string_view raw) noexcept {
    const auto* source = find_compiler_library_source(raw);
    return source && source->tier == CompilerLibraryTier::Core ? source : nullptr;
}

inline const CoreLibrarySource* find_alloc_library_source(
        std::string_view raw) noexcept {
    const auto* source = find_compiler_library_source(raw);
    return source && source->tier == CompilerLibraryTier::Alloc ? source : nullptr;
}

inline const CoreLibrarySource* find_std_library_source(
        std::string_view raw) noexcept {
    const auto* source = find_compiler_library_source(raw);
    return source && source->tier == CompilerLibraryTier::Std ? source : nullptr;
}

inline CoreClosureRequirements infer_core_closure_requirements(
        const Program& program) noexcept {
    CoreClosureRequirements requirements;

    const auto add = [&](CoreProviderRequirement requirement) {
        requirements.provider_bits |= core_provider_bit(requirement);
    };

    const auto inspect_function = [&](const Function& fn) {
        if (!fn.declared_effects.valid()) {
            requirements.hosted_or_unknown_effect = true;
        } else {
            if (fn.declared_effects.has(FunctionEffect::Alloc))
                add(CoreProviderRequirement::Allocator);
            if (fn.declared_effects.has(FunctionEffect::Suspend))
                add(CoreProviderRequirement::Executor);
            if (fn.declared_effects.has(FunctionEffect::Io) ||
                    fn.declared_effects.has(FunctionEffect::Block) ||
                    fn.declared_effects.has(FunctionEffect::Ffi)) {
                requirements.hosted_or_unknown_effect = true;
            }
        }

        if (fn.async_source_i64 || !fn.source_tasks.empty() ||
                !fn.future_pipelines.empty() || !fn.ready_await_actor.empty() ||
                !fn.done_await_actor.empty()) {
            add(CoreProviderRequirement::Executor);
        }

        if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty()) {
            add(CoreProviderRequirement::Executor);
            add(CoreProviderRequirement::Mailbox);
            add(CoreProviderRequirement::TrustedControl);
        }

        for (const auto& local : fn.locals) {
            if (local.heap_owned || local.int_array)
                add(CoreProviderRequirement::Allocator);
            if (local.new_class) {
                for (const auto& cls : program.classes) {
                    if (cls.name != local.class_name) continue;
                    for (const auto& field : cls.fields) {
                        if (field.int_array) {
                            add(CoreProviderRequirement::Allocator);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    };

    for (const auto& fn : program.functions)
        inspect_function(fn);

    if (!program.actors.empty()) {
        add(CoreProviderRequirement::Executor);
        add(CoreProviderRequirement::Mailbox);
        add(CoreProviderRequirement::TrustedControl);
        for (const auto& actor : program.actors)
            inspect_function(actor.on_message);
    }

    return requirements;
}

} // namespace oreslang::frontend
