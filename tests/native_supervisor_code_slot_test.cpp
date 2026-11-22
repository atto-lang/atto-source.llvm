#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;

static void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}


/** Failed admission must own/reclaim code images outside the CONTROL lock. */
static void exercise_rejected_hot_reload_reclamation() {
    std::atomic<int> rejected_reclaims{0};
    std::atomic<int> accepted_reclaims{0};
    NativeSupervisorCodeSlot<std::string> slot(
            "actor-abi-v1", "actor-policy-v1", [&] { ++accepted_reclaims; }, 2);

    auto rejected_image = [&] {
        ++rejected_reclaims;
        // A callback that re-enters the slot exposes teardown-under-lock bugs.
        (void)slot.live_revisions();
    };
    bool denied = false;
    try {
        (void)slot.activate(0, "actor-abi-v1", "stale", rejected_image);
    } catch (const std::logic_error&) { denied = true; }
    check(denied && rejected_reclaims == 1,
          "stale candidate must reclaim exactly once outside the slot lock");

    denied = false;
    try {
        (void)slot.activate(1, "changed-actor-abi", "incompatible", rejected_image);
    } catch (const std::invalid_argument&) { denied = true; }
    check(denied && rejected_reclaims == 2,
          "ABI mismatch must reclaim candidate without replacing active code");
    check(slot.current().revision == 1 && slot.pin().code() == "actor-policy-v1",
          "denied upgrade corrupted active revision");

    auto pinned_v1 = slot.pin();
    check(slot.activate(1, "actor-abi-v1", "actor-policy-v2",
                        [&] { ++accepted_reclaims; }).revision == 2,
          "valid code-only upgrade must publish");
    auto pinned_v2 = slot.pin(); // Both v1 and v2 must be live to exhaust a cap of two.
    denied = false;
    try {
        (void)slot.activate(2, "actor-abi-v1", "over-cap", rejected_image);
    } catch (const std::length_error&) { denied = true; }
    check(denied && rejected_reclaims == 3,
          "pinned generation limit must reclaim denied candidate");
    check(pinned_v1.code() == "actor-policy-v1" && slot.pin().code() == "actor-policy-v2",
          "pinned invocation observed a different generation");
    pinned_v1.close();
    pinned_v2.close();

    // Two simultaneous candidates at the same expected epoch: one winner.
    std::atomic<int> wins{0}, conflicts{0};
    std::thread a([&] {
        try {
            slot.activate(2, "actor-abi-v1", "competing-a", rejected_image);
            ++wins;
        } catch (const std::logic_error&) { ++conflicts; }
    });
    std::thread b([&] {
        try {
            slot.activate(2, "actor-abi-v1", "competing-b", rejected_image);
            ++wins;
        } catch (const std::logic_error&) { ++conflicts; }
    });
    a.join();
    b.join();
    check(wins == 1 && conflicts == 1 && slot.current().revision == 3,
          "concurrent candidate publication was not single-winner CAS");
    check(rejected_reclaims == 4, "losing concurrent candidate leaked");
    slot.close();
    denied = false;
    try {
        (void)slot.activate(3, "actor-abi-v1", "after-close", rejected_image);
    } catch (const std::logic_error&) { denied = true; }
    check(denied && rejected_reclaims == 6,
          "terminal slot close must reject and reclaim a late candidate");
    check(accepted_reclaims == 2,
          "successfully installed old images must each be reclaimed once");
    // Winning competing candidate also uses rejected_image as its retirement
    // callback; no active generation may be retained after close.
    check(rejected_reclaims == 6,
          "latest revision cleanup did not run when slot closed");
}

// Runtime ownership proof: unload callbacks must run AFTER the Policy value
// has been destroyed, including on invalid ABI, stale CAS and slot closure.
struct TrackedImage final {
    std::shared_ptr<std::atomic<int>> live;
    explicit TrackedImage(std::shared_ptr<std::atomic<int>> counter)
        : live(std::move(counter)) { ++*live; }
    TrackedImage(const TrackedImage&) = delete;
    TrackedImage& operator=(const TrackedImage&) = delete;
    TrackedImage(TrackedImage&&) noexcept = default;
    TrackedImage& operator=(TrackedImage&&) noexcept = default;
    ~TrackedImage() noexcept { if (live) --*live; }
};
static void exercise_destructor_before_image_unload() {
    std::atomic<int> callbacks{0}, wrong_order{0};
    auto new_counter = [] { return std::make_shared<std::atomic<int>>(0); };
    auto cleanup = [&](std::shared_ptr<std::atomic<int>> alive) {
        return [&, alive] {
            ++callbacks;
            if (alive->load() != 0) ++wrong_order;
        };
    };
    const auto v1 = new_counter();
    {
        NativeSupervisorCodeSlot<TrackedImage> slot("actor-abi", TrackedImage(v1),
                                                     cleanup(v1));
        check(v1->load() == 1, "initial generation wrapper not retained");
        auto stale = new_counter();
        bool denied = false;
        try { slot.activate(0, "actor-abi", TrackedImage(stale), cleanup(stale)); }
        catch (const std::logic_error&) { denied = true; }
        check(denied && stale->load() == 0, "stale code wrapper not destroyed");
        auto invalid = new_counter();
        denied = false;
        try { slot.activate(1, "", TrackedImage(invalid), cleanup(invalid)); }
        catch (const std::invalid_argument&) { denied = true; }
        check(denied && invalid->load() == 0, "invalid ABI wrapper not destroyed");
        const auto v2 = new_counter();
        check(slot.activate(1, "actor-abi", TrackedImage(v2), cleanup(v2)).revision == 2,
              "valid tracked upgrade failed");
        check(v1->load() == 0 && v2->load() == 1,
              "previous accepted code wrapper not destroyed");
        slot.close();
        check(v2->load() == 0, "closed generation wrapper not destroyed");
    }
    check(callbacks == 4 && wrong_order == 0,
          "native image unloaded before Policy destruction or leaked");
    // Invalid initial admission must own and discharge its resource.
    auto invalid_initial = new_counter();
    bool denied = false;
    try {
        NativeSupervisorCodeSlot<TrackedImage> bad(
            "", TrackedImage(invalid_initial), cleanup(invalid_initial));
    } catch (const std::invalid_argument&) { denied = true; }
    check(denied && callbacks == 5 && wrong_order == 0
          && invalid_initial->load() == 0,
          "rejected initial admission leaked or unloaded a live image wrapper");
}

// Throwing std::function copies simulate staging allocation failure after the
// API has accepted ownership of a loaded resource. No image may leak.
struct ThrowWhenCopied final {
    std::shared_ptr<std::atomic<int>> callbacks;
    explicit ThrowWhenCopied(std::shared_ptr<std::atomic<int>> count)
        : callbacks(std::move(count)) {}
    ThrowWhenCopied(const ThrowWhenCopied&) {
        throw std::runtime_error("injected callback-copy failure");
    }
    ThrowWhenCopied(ThrowWhenCopied&&) noexcept = default;
    void operator()() const noexcept { ++*callbacks; }
};
static void exercise_callback_copy_failure_cleanup() {
    auto counter = std::make_shared<std::atomic<int>>(0);
    NativeSupervisorCodeSlot<std::string> slot("abi", "old",
        [] {});
    std::function<void()> risky = ThrowWhenCopied(counter);
    bool threw = false;
    try {
        (void)slot.activate(1, "abi", "unpublished", std::move(risky));
    } catch (const std::runtime_error&) { threw = true; }
    check(threw && counter->load() == 1 && slot.current().revision == 1,
          "exception while constructing image leaked candidate or published it");
    auto init_counter = std::make_shared<std::atomic<int>>(0);
    std::function<void()> initial_risky = ThrowWhenCopied(init_counter);
    threw = false;
    try {
        NativeSupervisorCodeSlot<std::string> rejected_initial(
            "abi", "never-active", std::move(initial_risky));
    } catch (const std::runtime_error&) { threw = true; }
    check(threw && init_counter->load() == 1,
          "exception while constructing initial image leaked loaded resource");
}


static void exercise_failed_image_unload_signal() {
    using Slot = NativeSupervisorCodeSlot<std::string>;
    const auto before = Slot::cleanup_failures_observed();
    std::atomic<int> attempted{0};
    Slot slot("abi", "live", [] {});
    bool denied = false;
    try {
        (void)slot.activate(0, "abi", "stale", [&] {
            ++attempted;
            throw std::runtime_error("injected unload failure");
        });
    } catch (const std::logic_error&) {
        denied = true;
    }
    check(denied && attempted == 1,
          "failing cleanup did not leave stale candidate rejected");
    check(slot.current().revision == 1,
          "failing cleanup changed authoritative code revision");
    check(Slot::cleanup_failures_observed() == before + 1,
          "failed noexcept image cleanup was swallowed without an audit signal");
    // Failure observation must not grant guest code, retry the candidate or
    // imply that its native code-image mapping was actually released.
    slot.close();
}

int main(){
    std::atomic<int> reclaimed{0};
    NativeSupervisorRegistry supervisors(42, 8, 8);
    auto root = supervisors.root();
    auto daddy = supervisors.create_child(root);
    auto group = supervisors.create_group(daddy, 2);
    const auto group_id = group->group_id();
    const auto supervisor_id = group->supervisor_id();
    auto installed = std::make_shared<NativeSupervisorCodeSlot<std::string>>(
        "abi-root", "Daddy-live-v1", [&]{reclaimed++;}, 3);
    supervisors.install_policy(daddy, installed);
    check(supervisors.policy_revision(daddy).revision==1,
          "native registry did not attach supervisor code");

    // Once installed, possession of the shared_ptr is not CONTROL authority.
    bool denied_direct=false;
    try { (void)installed->current(); }
    catch (const std::logic_error&) { denied_direct=true; }
    check(denied_direct, "bound slot leaked direct current authority");
    denied_direct=false;
    try { (void)installed->pin(); }
    catch (const std::logic_error&) { denied_direct=true; }
    check(denied_direct, "bound slot leaked direct pin authority");

    std::atomic<int> unauthorized_reclaims{0};
    denied_direct=false;
    try {
        (void)installed->activate(
            1, "abi-root", "forged-direct",
            [&]{ ++unauthorized_reclaims; });
    } catch (const std::logic_error&) { denied_direct=true; }
    check(denied_direct && unauthorized_reclaims==1,
          "bound slot admitted direct activation or leaked rejected image");

    denied_direct=false;
    try { installed->close(); }
    catch (const std::logic_error&) { denied_direct=true; }
    check(denied_direct && supervisors.policy_revision(daddy).revision==1,
          "bound slot leaked direct close authority");

    bool denied_slot_reuse=false;
    try { supervisors.install_policy(root, installed); }
    catch (const std::logic_error&) { denied_slot_reuse=true; }
    check(denied_slot_reuse, "one native code slot attached to two supervisors");

    // Same numeric supervisor IDs in a second registry are still foreign.
    NativeSupervisorRegistry foreign_supervisors(43, 8, 8);
    auto foreign_daddy =
        foreign_supervisors.create_child(foreign_supervisors.root());
    check(foreign_daddy.id()==daddy.id(),
          "test setup did not create same numeric supervisor ID");
    bool denied_foreign=false;
    try { (void)supervisors.policy_revision(foreign_daddy); }
    catch (const std::invalid_argument&) { denied_foreign=true; }
    check(denied_foreign,
          "same numeric supervisor ID crossed registry authority domain");

    check(supervisors.activate_policy(
              daddy, 1, "abi-root", "Daddy-live-v2",
              [&]{reclaimed++;}).revision==2,
          "owner-authorized native policy activation failed");
    check(supervisors.pin_policy(daddy).code()=="Daddy-live-v2",
          "owner-authorized native policy pin returned wrong generation");
    check(supervisors.policy_revision(daddy).revision==2,
          "native registry did not observe activated policy revision");
    check(group->group_id()==group_id && group->supervisor_id()==supervisor_id,
          "native registry policy activation changed group/supervisor identity");
    supervisors.close_policy(daddy);
    // This registry slot is independent from the direct slot exercised below.
    reclaimed.store(0);
    {
        NativeSupervisorCodeSlot<std::string> slot("abi1", "Daddy-v1",
                [&]{reclaimed++;}, 2);
        auto old = slot.pin();
        auto next = slot.activate(1,"abi1","Daddy-v2",[&]{reclaimed++;});
        check(next.revision==2,"new code revision not committed");
        check(old.code()=="Daddy-v1","old pinned continuation was modified");
        check(slot.pin().code()=="Daddy-v2","new turn got old policy revision");
        check(reclaimed.load()==0,"old policy reclaimed while a lease was live");
        bool rejected=false;
        auto second = slot.pin();
        try{(void)slot.activate(2,"abi1","Daddy-v3",[&]{reclaimed++;});}
        catch(const std::length_error&){rejected=true;}
        check(rejected,"pinned code revision cap not enforced");
        second.close();
        old.close();
        check(reclaimed.load()==2,"rejected candidate and old pinned code must each reclaim once");
        rejected=false;
        try{(void)slot.activate(99,"abi1","bad",[&]{reclaimed++;});}
        catch(const std::logic_error&){rejected=true;}
        check(rejected,"stale activation admitted");
        rejected=false;
        try{(void)slot.activate(2,"abi2","bad",[&]{reclaimed++;});}
        catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"unchecked ABI migration admitted");
        check(slot.activate(2,"abi1","Daddy-v3",[&]{reclaimed++;}).revision==3,
              "new policy revision could not activate after releasing old pin");
        check(reclaimed.load()==5,"rejected candidates and replaced code images not reclaimed");
        check(group->group_id()==group_id && group->supervisor_id()==supervisor_id,
              "code reload changed identity or group association");
        auto pinned = slot.pin();
        slot.close();
        check(reclaimed.load()==5,"close freed a pinned new supervisor policy");
        rejected=false;
        try{(void)slot.pin();}catch(const std::logic_error&){rejected=true;}
        check(rejected,"closed policy admitted another turn");
        pinned.close();
    }
    check(reclaimed.load()==6,"new policy or rejected candidate leaked after final pinned turn");
    group->close();
    exercise_rejected_hot_reload_reclamation();
    exercise_destructor_before_image_unload();
    exercise_callback_copy_failure_cleanup();
    exercise_failed_image_unload_signal();
}
