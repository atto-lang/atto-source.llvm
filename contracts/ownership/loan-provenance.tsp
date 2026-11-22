// Compile-time conformance data, not a guest capability. Validation also
// requires the trusted source-to-CFG binder and actor-incarnation authority.
namespace Oreslang.Ownership;

@doc("Compiler-generated lexical loan identity event for cross-backend tests.")
model LoanProvenance {
  contractVersion: "1.0";
  @minValue(1)
  actorIncarnation: uint64;
  @minLength(1)
  borrowSite: string;
  @minValue(1)
  loanId: uint64;
  @minLength(1)
  place: string;
  kind: "shared" | "exclusive";
  action: "borrow" | "release";
}
