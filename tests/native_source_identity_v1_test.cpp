#include "oreslang/runtime/SourceIdentityV1.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
  using namespace oreslang::telemetry;
  SourceId routine{};
  SourceId site{};
  assert(parse_source_id("rid_d5f8e146ba97430dac6bf17430477e2a", SourceKind::Routine, routine));
  assert(parse_source_id("sid_652e1632e7464fdb936fc40fe24f1c7b", SourceKind::Site, site));
  const auto saved = routine;
  assert(!parse_source_id("sid_d5f8e146ba97430dac6bf17430477e2a", SourceKind::Routine, routine));
  assert(routine == saved);  // invalid input never partially overwrites out
  assert(!parse_source_id("rid_D5f8e146ba97430dac6bf17430477e2a", SourceKind::Routine, routine));
  assert(!parse_source_id("rid_00000000000000000000000000000000", SourceKind::Routine, routine));
  assert(!parse_source_id("4bf92f3577b34da6a3ce929d0e0e4736", SourceKind::Routine, routine));
  assert(!parse_source_id("rid_d5f8e146ba97430dac6bf17430477e2a_extra", SourceKind::Routine, routine));
  const StaticSite ok[] = {{routine, site, 17}};
  assert(validate_static_sites(ok, 1));
  const StaticSite reversed[] = {{site, routine, 17}};
  assert(!validate_static_sites(reversed, 1));
  assert(validate_static_sites(nullptr, 0));
  assert(!validate_static_sites(nullptr, 1));
  const StaticSite dup[] = {{routine, site, 1}, {routine, site, 2}};
  assert(!validate_static_sites(dup, 2));
  SourceId other{};
  assert(parse_source_id("sid_738b13448f724fc5905841e9037b410a", SourceKind::Site, other));
  const StaticSite alias[] = {{routine, site, 1}, {routine, other, 1}};
  assert(!validate_static_sites(alias, 2));
  const StaticSite distinct[] = {{routine, site, 1}, {routine, other, 2}};
  assert(validate_static_sites(distinct, 2));
  std::cout << "LLVM_SOURCE_ID_HOST_METADATA_PASS\n";
}
