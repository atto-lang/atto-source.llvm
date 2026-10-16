#include "oreslang/runtime/DopKernelIR.hpp"

#include <cstdint>
#include <stdexcept>

namespace {
void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

template <typename Fn>
void rejects_borrowed_close(Fn&& action, const char* message) {
    try {
        action();
    } catch (const std::logic_error&) {
        return;
    }
    throw std::runtime_error(message);
}
}

int main() {
    using namespace oreslang::dop;
    for (const auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        const std::uint32_t lanes = layout == Layout::AoSoA ? 4 : 1;
        const Plan shape(layout, 7, 3, 4, 16, lanes);

        // A retired root can remain referenced without pinning its native data.
        {
            HostBuffer owner(shape);
            auto root = owner.region_root();
            check(owner.retained_host_capacity_bytes() >= shape.byte_length(),
                  "new allocation has no retained host storage");
            owner.write(2, 1, 31.0f);
            owner.close();
            check(!root.is_live_owned(), "close did not retire physical identity");
            check(owner.retained_host_capacity_bytes() == 0,
                  "close retained physical host allocation capacity");
            owner.close();
            check(owner.retained_host_capacity_bytes() == 0,
                  "second close unexpectedly reacquired storage");
            rejects_borrowed_close([&] { (void) owner.read(2, 1); },
                                   "closed owner unexpectedly admitted a read");
        }

        // Dropping the final owner without an explicit close also retires the
        // physical identity. A copied RegionRoot must not retain host bytes.
        {
            RegionRoot escaped;
            {
                HostBuffer transient(shape);
                escaped = transient.region_root();
                check(escaped.is_live_owned(),
                      "transient owner did not create a live region");
            }
            check(!escaped.is_live_owned(),
                  "owner destructor failed to retire escaped region identity");
        }

        // A failed close must preserve storage and keep active readers valid.
        {
            HostBuffer owner(shape);
            owner.write(1, 2, 17.0f);
            const auto initial_capacity = owner.retained_host_capacity_bytes();
            {
                auto read = owner.borrow_read();
                rejects_borrowed_close([&] { owner.close(); },
                                       "close admitted an outstanding reader");
                check(owner.retained_host_capacity_bytes() == initial_capacity,
                      "failed read-loan close released backing storage");
                check(read.read(1, 2) == 17.0f,
                      "read loan lost storage after rejected close");
            }
            {
                auto write = owner.borrow_write();
                rejects_borrowed_close([&] { owner.close(); },
                                       "close admitted an outstanding writer");
                check(owner.retained_host_capacity_bytes() == initial_capacity,
                      "failed write-loan close released backing storage");
                write.write(1, 2, 19.0f);
            }
            check(owner.read(1, 2) == 19.0f,
                  "write loan lost storage after rejected close");
            owner.close();
            check(owner.retained_host_capacity_bytes() == 0,
                  "post-loan close retained backing allocation");
        }

        // Moving ownership preserves the one physical allocation and token.
        {
            HostBuffer original(shape);
            const auto capacity = original.retained_host_capacity_bytes();
            auto root = original.region_root();
            auto successor = original.take();
            original.close();
            check(original.retained_host_capacity_bytes() == 0,
                  "moved-from wrapper reports retained storage");
            check(root.same_root(successor.region_root()) && root.is_live_owned(),
                  "take replaced or retired a live storage identity");
            check(successor.retained_host_capacity_bytes() == capacity,
                  "moved-from close freed successor allocation");
            successor.write(0, 0, 41.0f);
            check(successor.read(0, 0) == 41.0f, "successor storage inaccessible");
            successor.close();
            check(!root.is_live_owned(), "successor close did not retire region");
            check(successor.retained_host_capacity_bytes() == 0,
                  "successor close failed to relinquish physical storage");
        }
    }
}
