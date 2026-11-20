#include "oreslang/types/NativeRefinementProofContract.hpp"
#include "oreslang/types/NativeSessionProofContract.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::types;

template<class F>
void must_reject(F&& body) {
    bool failed = false;
    try { body(); }
    catch (const std::invalid_argument&) { failed = true; }
    assert(failed && "unsound static code admission must reject");
}

int main() {
    using A = NativeSessionAction;
    using P = NativeSessionPhase;
    const NativeSessionGraph valid{42, 7, {{{A::Mint, A::AdmitAuth,
        A::AuthTimeout, A::SettleAuth, A::AdmitCapture,
        A::SettleCapture, A::Close}, {}}}};
    assert(verify_native_session_cfg(valid).outgoing[0] == session_bit(P::Closed));

    must_reject([] {
        verify_native_session_cfg({42, 7, {{{A::Mint, A::AdmitCapture}, {}}}});
    });
    must_reject([] {
        verify_native_session_cfg({42, 7, {{{A::Mint, A::AdmitAuth,
            A::AuthTimeout, A::ReuseCreated}, {}}}});
    });
    must_reject([] {
        verify_native_session_cfg({42, 7, {{{A::Mint, A::CopyCapability}, {}}}});
    });
    must_reject([] {
        verify_native_session_cfg({42, 7, {{{A::Mint, A::RebindGeneration}, {}}}});
    });
    must_reject([] { verify_native_session_cfg({0, 1, {{{A::Mint}, {}}}}); });
    must_reject([] { verify_native_session_cfg({1, 1, {{{A::Mint}, {10}}}}); });

    NativeSessionGraph split{42, 7, {
        {{A::Mint}, {1, 2}},
        {{A::AdmitAuth, A::SettleAuth}, {3}},
        {{A::ReuseCreated}, {3}},
        {{}, {}}
    }};
    auto result = verify_native_session_cfg(split);
    assert(result.incoming[3] == (session_bit(P::Created) | session_bit(P::Authorized)));
    split.blocks[3].actions = {A::AdmitCapture};
    must_reject([&] { (void)verify_native_session_cfg(split); });
    must_reject([] {
        (void)verify_native_session_cfg({42, 7, {
            {{A::Mint}, {1}},
            {{A::AdmitAuth}, {1}}
        }});
    });

    const NativeFunctionRefinement contract{
        "ledger.charge", {{0, 100}, {1, 20}}, {0, 2000},
        {NativeRefinementEffect::Io}
    };
    require_native_refined_call(contract, {{1, 10}, {2, 3}}, {NativeRefinementEffect::Io});
    require_native_refined_returns(contract, {{0, 20}, {20, 1000}});
    must_reject([&] { require_native_refined_call(contract,
        {{-1, 10}, {2, 3}}, {NativeRefinementEffect::Io}); });
    must_reject([&] { require_native_refined_call(contract,
        {{1, 10}, {2, 3}}, {NativeRefinementEffect::Io, NativeRefinementEffect::Panic}); });
    must_reject([&] { require_native_refined_returns(contract, {}); });
    must_reject([&] { require_native_refined_returns(contract, {{0, 20}, {-1, 1000}}); });
    must_reject([] { NativeI64Interval invalid{10, 2}; (void)invalid; });
    const NativeFunctionRefinement unknown{
        "ledger.unknown", {}, {0, 1}, {NativeRefinementEffect::UnknownCall}};
    must_reject([&] { require_native_function_refinement_contract(unknown); });
    // Arithmetic proof parity with Java's BigInteger-derived intervals:
    // the C++ checked operations must never invoke signed-overflow UB.
    using I = NativeI64Interval;
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    auto plus = I{1, 3}.plus({2, 4});
    assert(plus.minimum == 3 && plus.maximum == 7);
    auto minus = I{1, 3}.minus({2, 4});
    assert(minus.minimum == -3 && minus.maximum == 1);
    auto times = I{-2, 3}.times({-4, 4});
    assert(times.minimum == -12 && times.maximum == 12);
    auto zero = I{0, 0}.times({kMin, kMax});
    assert(zero.minimum == 0 && zero.maximum == 0);
    auto edge = I{kMin, kMin}.plus({0, 0});
    assert(edge.minimum == kMin && edge.maximum == kMin);
    must_reject([&] { (void)I{kMax, kMax}.plus({1, 1}); });
    must_reject([&] { (void)I{kMin, kMin}.minus({1, 1}); });
    must_reject([&] { (void)I{kMin, kMin}.times({-1, -1}); });
    must_reject([&] { (void)I{kMax, kMax}.times({2, 2}); });
    must_reject([&] { (void)I{kMin, kMax}.plus({0, 1}); });

    // Model-check a bounded range with independently computed exact i64
    // expected values; keeps the native compiler test independent of Boost.
    for (std::int64_t a = -32; a <= 32; ++a) {
        for (std::int64_t b = -32; b <= 32; ++b) {
            assert(add_i64_checked(a, b) == a + b);
            assert(sub_i64_checked(a, b) == a - b);
            assert(mul_i64_checked(a, b) == a * b);
            const auto item = I{a, a}.times({b, b});
            assert(item.minimum == a * b && item.maximum == a * b);
        }
    }
    const NativeFunctionRefinement fakeEffect{
        "ledger.fake", {}, {0, 1},
        {static_cast<NativeRefinementEffect>(255)}
    };
    must_reject([&] { require_native_function_refinement_contract(fakeEffect); });
    must_reject([&] {
        require_native_refined_call(contract, {{1, 10}, {2, 3}},
                {static_cast<NativeRefinementEffect>(255)});
    });
    const NativeFunctionRefinement blank{
        "  ", {}, {0, 1}, {NativeRefinementEffect::Io}
    };
    must_reject([&] { require_native_function_refinement_contract(blank); });
    const NativeFunctionRefinement tooLong{
        std::string(257, 'x'), {}, {0, 1}, {}
    };
    must_reject([&] { require_native_function_refinement_contract(tooLong); });
    return 0;
}
