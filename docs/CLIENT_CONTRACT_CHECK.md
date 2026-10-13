# Declaration checking and target admission

--check emits no IR. A String client contract is accepted only as a declaration-only
host source unit; mixed executable functions/classes/actors and experimental
owned-int/scalar-loop or Wasm target modes reject until their contract ABI exists.
Normal non-contract --check follows verified backend/target admission before
discarding IR, so parsing alone cannot certify unsupported native entrypoints
or Wasm ownership lowering. Runtime contract emission remains denied in both
the CLI and library. Signature arrows must be contiguous =>, matching Java tokens.
