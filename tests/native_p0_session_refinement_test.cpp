#include "oreslang/types/NativeRefinementProofContract.hpp"
#include "oreslang/types/NativeSessionProofContract.hpp"

#include <cassert>
#include <stdexcept>
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
    return 0;
}
