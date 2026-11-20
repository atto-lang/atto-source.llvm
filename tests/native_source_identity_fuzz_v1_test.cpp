#include "oreslang/runtime/SourceIdentityV1.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
  using namespace oreslang::telemetry;
  constexpr char digits[] = "0123456789abcdef";
  std::uint64_t state = 0x62e1d4b76027190cULL;
  auto draw = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  SourceId out{};
  for (int n = 0; n < 2000; ++n) {
    std::string valid = "rid_";
    for (int i = 0; i < 32; ++i) {
      valid.push_back(digits[draw() % 16]);
    }
    assert(parse_source_id(valid, SourceKind::Routine, out));
    assert(out.kind == SourceKind::Routine);
    for (int i = 4; i < 36; ++i) {
      auto mutated = valid;
      mutated[static_cast<std::size_t>(i)] = 'G';
      SourceId previous = out;
      assert(!parse_source_id(mutated, SourceKind::Routine, out));
      assert(out == previous);
    }
    auto site = valid;
    site.replace(0, 4, "sid_");
    SourceId parsed_site{};
    assert(parse_source_id(site, SourceKind::Site, parsed_site));
    assert(parsed_site.kind == SourceKind::Site);
    assert(!parse_source_id(site, SourceKind::Routine, out));
    assert(!parse_source_id(valid, SourceKind::Site, out));
    auto retained = out;
    assert(!parse_source_id(site, static_cast<SourceKind>(99), out));
    assert(out == retained);
  }
  std::cout << "LLVM_SOURCE_ID_FUZZ_HOST_PASS:2000\n";
}
