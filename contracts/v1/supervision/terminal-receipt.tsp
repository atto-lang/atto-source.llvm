// Normative, language-neutral host-only STOP-receipt data contracts.
// Uint64 is a decimal string to avoid IEEE-754 precision loss in JSON.
// Neither proof type is guest-authoritative: adapters MUST bind to a
// trusted scheduler or an independently verified external effect ledger.
namespace Oreslang.SupervisorReceipts;

@pattern("^(0|[1-9][0-9]{0,19})$")
scalar UInt64Decimal extends string;

@pattern("^[1-9][0-9]{0,19}$")
scalar NonZeroUInt64Decimal extends string;

@pattern("^[a-z][a-z0-9._:-]{0,127}$")
scalar EffectAuthorityName extends string;

@pattern("^[0-9a-f]{64}$")
scalar Sha256Hex extends string;

model ActorGeneration {
  scheduler_domain: NonZeroUInt64Decimal;
  actor_slot: UInt64Decimal;
  generation: NonZeroUInt64Decimal;
}

model TerminalIdentity {
  supervisor_id: NonZeroUInt64Decimal;
  runtime_id: NonZeroUInt64Decimal;
  original_parent: ActorGeneration;
  failed_child: ActorGeneration;
  failure_kind: "1" | "2" | "3";
  failure_code: UInt64Decimal;
  correlation: UInt64Decimal;
  logical_child_id: NonZeroUInt64Decimal;
  logical_incarnation: NonZeroUInt64Decimal;
}

model PhysicalDoneProof {
  actor: ActorGeneration;
  observation_source: "native_scheduler";
  // Trusted host must call exact-generation poll_done. This is an
  // observation, not proof of external-effect execution or durability.
}

model ExternalEffectProof {
  claim_nonce: NonZeroUInt64Decimal;
  evidence_sha256: Sha256Hex;
  authority: EffectAuthorityName;
  // Nonempty authority names an independently queried commit ledger.
  // A digest alone is NOT a valid assertion of external-effect success.
}

model TerminalReceipt {
  schema_version: "1.0.0";
  key: TerminalIdentity;
  phase: "recorded" | "physical_done" | "claimed"
    | "effect_proven" | "acknowledged";
  physical_done_proof?: PhysicalDoneProof;
  claim_nonce?: NonZeroUInt64Decimal;
  external_effect_proof?: ExternalEffectProof;
}

// State-machine constraints beyond TypeSpec's structural model are
// enforced by terminal-receipt.schema.json and the executable journal.
