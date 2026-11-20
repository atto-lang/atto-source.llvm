#include "atto/tenant_bytes_v1.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

using namespace atto::tenant_bytes_v1;
int main() {
  Arena alice({1, 2, 3}, 2048);
  Arena bob({4, 2, 3}, 2048);
  const auto baseline = alice.charged();
  {
    auto allocated = alice.allocate_zeroed(16);
    assert(allocated.code == Code::ok && allocated.value.has_value());
    auto& b = *allocated.value;
    assert(b.length() == 16 && alice.charged() > baseline);
    std::uint8_t out = 99;
    for (std::int64_t i = 0; i < 16; ++i) {
      assert(b.read_byte(alice, i, out) == Code::ok && out == 0);
    }
    assert(b.read_byte(bob, 0, out) == Code::invalid_owner && out == 0);
    assert(b.write_byte(bob, 0, 2) == Code::invalid_owner);
    assert(b.write_byte(alice, -1, 2) == Code::bounds);
    assert(b.write_byte(alice, 16, 2) == Code::bounds);
    assert(b.write_byte(alice, 0, 256) == Code::value);
    assert(b.write_byte(alice, 0, -1) == Code::value);
    assert(b.write_byte(alice, 0, 42) == Code::ok);
    assert(b.read_byte(alice, 0, out) == Code::ok && out == 42);
    auto moved = std::move(*allocated.value);
    assert(b.read_byte(alice, 0, out) == Code::invalid_owner);
    assert(moved.read_byte(alice, 0, out) == Code::ok && out == 42);
  }
  assert(alice.charged() == baseline);
  assert(alice.allocate_zeroed(-1).code == Code::invalid_owner);
  assert(alice.allocate_zeroed(std::numeric_limits<std::int64_t>::max()).code == Code::quota);
  {
    auto old = alice.allocate_zeroed(8);
    assert(old.code == Code::ok);
    alice.revoke();
    std::uint8_t result = 7;
    assert(old.value->read_byte(alice, 0, result) == Code::revoked && result == 7);
    assert(old.value->write_byte(alice, 0, 7) == Code::revoked);
    assert(alice.allocate_zeroed(0).code == Code::revoked);
  }
  assert(alice.charged() == 0);
  // A separate arena with the same numeric principal is not equivalent authority.
  Arena copied_numbers({1, 2, 3}, 2048);
  auto a = copied_numbers.allocate_zeroed(2);
  assert(a.code == Code::ok);
  assert(a.value->write_byte(alice, 0, 1) == Code::invalid_owner);
  // Independent tenant actors can make bounded allocations concurrently.
  Arena tenant({77, 1, 1}, 32 * 1024);
  std::vector<std::thread> threads;
  for (int k = 0; k < 4; ++k) threads.emplace_back([&] {
    for (int j = 0; j < 1000; ++j) {
      auto x = tenant.allocate_zeroed(32);
      assert(x.code == Code::ok);
      assert(x.value->write_byte(tenant, 0, 10) == Code::ok);
    }
  });
  for (auto& thread : threads) thread.join();
  assert(tenant.charged() == 0);
  Arena tiny({8, 8, 8}, sizeof(Bytes) + 1);
  assert(tiny.allocate_zeroed(2).code == Code::quota);
  assert(tiny.allocate_zeroed(1).code == Code::ok);
  assert(tiny.charged() == 0);
}
