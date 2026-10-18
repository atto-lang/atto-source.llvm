#include "fixtures/hot_aot_image_abi.hpp"

#include <dlfcn.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <utility>

/**
 * TEST-ONLY POSIX PIC loader proof. dlopen executes native code BEFORE
 * dlsym/descriptor admission. This is strictly for compiled, trusted CI
 * fixtures: it MUST NOT accept untrusted or unsigned user actor images.
 * It is not wired to the Oreslang compiler, supervisor or actor runtime.
 */

namespace {
bool digest(const char* value) {
    if (!value) return false;
    const auto size = std::strlen(value);
    if (size != 64) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

struct SoHandle {
    explicit SoHandle(const std::string& path) {
        value = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!value) throw std::runtime_error("trusted test fixture dlopen failed");
    }
    ~SoHandle() { if (value) dlclose(value); }
    SoHandle(const SoHandle&) = delete;
    SoHandle& operator=(const SoHandle&) = delete;
    void* value = nullptr;
};

class Image {
public:
    explicit Image(std::string path) : so_(std::make_shared<SoHandle>(path)) {
        void* symbol = dlsym(so_->value, "ores_test_hot_aot_image_v1");
        if (!symbol) throw std::runtime_error("image ABI symbol absent");
        OresTestHotAotImageGetter getter = nullptr;
        static_assert(sizeof(getter) == sizeof(symbol),
                      "POSIX dlsym function-pointer ABI unsupported");
        std::memcpy(&getter, &symbol, sizeof(getter));
        const auto* manifest = getter();
        if (!manifest || manifest->abi_version != 1
            || manifest->struct_bytes != sizeof(OresTestHotAotImageV1)
            || !digest(manifest->code_sha256)
            || !digest(manifest->state_sha256)
            || !digest(manifest->message_sha256)
            || !digest(manifest->capability_sha256)
            || !manifest->apply)
            throw std::runtime_error("invalid trusted fixture image descriptor");
        code_sha_ = manifest->code_sha256;
        state_sha_ = manifest->state_sha256;
        message_sha_ = manifest->message_sha256;
        capability_sha_ = manifest->capability_sha256;
        call_ = manifest->apply;
    }
    std::int64_t apply(std::int64_t input) const { return call_(input); }
    const std::string& code() const noexcept { return code_sha_; }
    bool compatible(const Image& old) const {
        return state_sha_ == old.state_sha_
            && message_sha_ == old.message_sha_
            && capability_sha_ == old.capability_sha_;
    }
private:
    // Shared library pin must outlive EVERY possible reference to call_.
    std::shared_ptr<SoHandle> so_;
    std::string code_sha_;
    std::string state_sha_;
    std::string message_sha_;
    std::string capability_sha_;
    std::int64_t (*call_)(std::int64_t) = nullptr;
};

class TrustedFixtureRouter {
public:
    using Lease = std::shared_ptr<const Image>;
    class Candidate {
        friend class TrustedFixtureRouter;
        Lease image_;
        std::uint64_t expected_epoch_;
        std::shared_ptr<const int> owner_;
        Candidate(Lease image, std::uint64_t epoch, std::shared_ptr<const int> owner)
            : image_(std::move(image)), expected_epoch_(epoch), owner_(std::move(owner)) {}
    public:
        Candidate(const Candidate&) = default;
    };

    void install_initial(Lease image) {
        if (!image) throw std::invalid_argument("null initial image");
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_ || current_) throw std::logic_error("already initialized or closed");
        current_ = std::move(image);
        ++epoch_;
    }

    Lease acquire() const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_ || !current_) throw std::logic_error("no active image");
        return current_;
    }

    Candidate stage(Lease image) const {
        if (!image) throw std::invalid_argument("null image");
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_ || !current_) throw std::logic_error("not active");
        if (!image->compatible(*current_)) throw std::logic_error("incompatible state/wire/capability ABI");
        if (image->code() == current_->code()) throw std::logic_error("identical image");
        return Candidate(std::move(image), epoch_, authority_);
    }

    bool publish(Candidate prepared) {
        Lease previous;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) throw std::logic_error("router closed");
            if (prepared.owner_ != authority_ || !prepared.image_
                    || prepared.expected_epoch_ != epoch_) return false;
            if (!prepared.image_->compatible(*current_)) return false;
            if (epoch_ == UINT64_MAX) throw std::overflow_error("routing epoch exhausted");
            // Atomic/no-throw pointer switch; do not run the old image's
            // destructor, DSO finalizers or dlclose under the router mutex.
            current_.swap(prepared.image_);
            previous.swap(prepared.image_);
            ++epoch_;
        }
        // Release the previous DSO only after dropping the routing mutex.
        return true;
    }

    void close() {
        Lease previous;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closed_ = true;
            previous.swap(current_);
        }
        // dlclose can execute arbitrary native finalizers: outside mutex.
    }
private:
    mutable std::mutex mutex_;
    Lease current_;
    // Opaque, nonforgeable per-router identity. A matching epoch/ABI from
    // another supervisor is not authorization to publish here.
    const std::shared_ptr<const int> authority_ = std::make_shared<const int>(0);
    std::uint64_t epoch_ = 0;
    bool closed_ = false;
};

template<class F> bool rejects(F&& f) {
    try { f(); }
    catch (const std::exception&) { return true; }
    return false;
}
}

int main(int argc, char** argv) {
    assert(argc == 4);
    const std::string old_path = argv[1], new_path = argv[2], bad_path = argv[3];
    auto old = std::make_shared<Image>(old_path);
    auto next = std::make_shared<Image>(new_path);
    auto bad = std::make_shared<Image>(bad_path);
    TrustedFixtureRouter route;
    route.install_initial(old);
    auto first = route.acquire();
    assert(first->apply(7) == 17);
    assert(rejects([&] { (void) route.stage(bad); }));
    assert(route.acquire()->apply(7) == 17);
    assert(rejects([&] { (void) route.stage(old); }));
    auto candidate = route.stage(next);
    auto stale = route.stage(next);
    TrustedFixtureRouter foreign;
    foreign.install_initial(old);
    assert(!foreign.publish(candidate));
    assert(foreign.acquire()->apply(7) == 17);
    assert(route.publish(candidate));
    assert(route.acquire()->apply(7) == 27);
    assert(first->apply(7) == 17);  // old image remains pinned
    assert(!route.publish(stale));   // stale generation cannot retake routing
    std::atomic<bool> go{false};
    std::atomic<bool> ok{true};
    std::thread reader([lease = first, &go, &ok] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (lease->apply(99) != 109) ok.store(false);
    });
    route.close();
    assert(rejects([&] { (void) route.acquire(); }));
    go.store(true, std::memory_order_release);
    reader.join();
    assert(ok.load());
    assert(first->apply(9) == 19);
    return 0;
}
