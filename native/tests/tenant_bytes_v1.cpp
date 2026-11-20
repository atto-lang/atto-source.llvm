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
  const auto before_negative = alice.charged();
  assert(alice.allocate_zeroed(-1).code == Code::invalid_size);
  assert(alice.charged() == before_negative);
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
  // Logical zero length still consumes the one-byte physical allocation.
  Arena zero_limit({8, 8, 9}, sizeof(Bytes));
  assert(zero_limit.allocate_zeroed(0).code == Code::quota);
  Arena zero_ok({8, 8, 10}, sizeof(Bytes) + 1);
  {
    auto empty = zero_ok.allocate_zeroed(0);
    assert(empty.code == Code::ok && empty.value.has_value());
    assert(empty.value->length() == 0);
    assert(zero_ok.charged() == sizeof(Bytes) + 1);
    std::uint8_t unchanged = 88;
    assert(empty.value->read_byte(zero_ok, 0, unchanged) == Code::bounds);
    assert(unchanged == 88);
    assert(empty.value->write_byte(zero_ok, 0, 12) == Code::bounds);
  }
  assert(zero_ok.charged() == 0);
  // Move assignment retires the previous charge exactly once.
  Arena replacement({8, 8, 11}, 4096);
  {
    auto first = replacement.allocate_zeroed(4);
    auto second = replacement.allocate_zeroed(6);
    assert(first.code == Code::ok && second.code == Code::ok);
    assert(replacement.charged() == sizeof(Bytes) * 2 + 10);
    *first.value = std::move(*second.value);
    assert(replacement.charged() == sizeof(Bytes) + 6);
    assert(first.value->length() == 6);
    assert(second.value->length() == 0);
  }
  assert(replacement.charged() == 0);
}
