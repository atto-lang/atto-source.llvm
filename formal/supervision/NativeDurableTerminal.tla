---------------------- MODULE NativeDurableTerminal ----------------------
EXTENDS Naturals

(* Abstract one-receipt crash journal. A durable CLAIM is a one-use intent,
 * never independent evidence that an external host effect succeeded. Missing
 * external proof is permanently ambiguous across a crash; recovery cannot
 * re-execute an effect just because its outcome was not acknowledged.
 * Physical DONE is independent from externally committed effects.
 * The executable native journal enforces the transition order; TLC does not
 * establish machine-checked implementation refinement or disk guarantees.
 *)
VARIABLES phase, physical, effect, claims, executions, crashed,
          closed, verified, illegalAck, illegalDone, illegalCloseEffect,
          reconciliationAck
vars == <<phase, physical, effect, claims, executions, crashed,
          closed, verified, illegalAck, illegalDone, illegalCloseEffect,
          reconciliationAck>>

Init ==
  /\ phase = "Absent"
  /\ physical = "Live"
  /\ effect = "Unknown"
  /\ claims = 0
  /\ executions = 0
  /\ crashed = FALSE
  /\ closed = FALSE
  /\ verified = FALSE
  /\ illegalAck = FALSE
  /\ illegalDone = FALSE
  /\ illegalCloseEffect = FALSE
  /\ reconciliationAck = FALSE

Record ==
  /\ phase = "Absent" /\ ~crashed /\ ~closed
  /\ phase' = "Recorded"
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

StopAndDone ==
  /\ phase = "Recorded" /\ ~crashed
  /\ physical' = "Done"
  /\ UNCHANGED <<phase, effect, claims, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

PersistDone ==
  /\ phase = "Recorded" /\ physical = "Done" /\ ~crashed
  /\ phase' = "PhysicalDone"
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

Claim ==
  /\ phase = "PhysicalDone" /\ ~crashed /\ ~closed
  /\ phase' = "Claimed"
  /\ claims' = claims + 1
  /\ UNCHANGED <<physical, effect, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

ExecuteExternalEffect ==
  /\ phase = "Claimed" /\ ~crashed /\ ~closed
  /\ effect = "Unknown" /\ executions = 0
  /\ effect' = "Committed"
  /\ executions' = 1
  /\ UNCHANGED <<phase, physical, claims, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

Crash ==
  /\ ~crashed
  /\ crashed' = TRUE
  /\ UNCHANGED <<phase, physical, effect, claims, executions, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

Reopen ==
  /\ crashed
  /\ crashed' = FALSE
  /\ UNCHANGED <<phase, physical, effect, claims, executions, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

IndependentExternalProof ==
  /\ phase = "Claimed" /\ effect = "Committed" /\ ~crashed
  /\ phase' = "EffectProven"
  /\ verified' = TRUE
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

Acknowledge ==
  /\ phase = "EffectProven" /\ verified /\ ~crashed
  /\ phase' = "Acknowledged"
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

(* A recovery ACK is distinct from the scheduler's ordinary CONTROL
 * enqueue ACK. It requires fsynced external-effect proof and journal ACK. *)
ReconcileAck ==
  /\ phase = "Acknowledged" /\ verified /\ ~crashed
  /\ ~reconciliationAck
  /\ reconciliationAck' = TRUE
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 phase, verified, illegalAck, illegalDone, illegalCloseEffect>>

CloseSupervisor ==
  /\ ~closed
  /\ closed' = TRUE
  /\ UNCHANGED <<phase, physical, effect, claims, executions, crashed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

Next == Record \/ StopAndDone \/ PersistDone \/ Claim
  \/ ExecuteExternalEffect \/ Crash \/ Reopen
  \/ IndependentExternalProof \/ Acknowledge \/ ReconcileAck \/ CloseSupervisor
Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"Absent", "Recorded", "PhysicalDone", "Claimed",
                "EffectProven", "Acknowledged"}
  /\ physical \in {"Live", "Done"}
  /\ effect \in {"Unknown", "Committed"}
  /\ claims \in 0..2 /\ executions \in 0..2
  /\ crashed \in BOOLEAN /\ closed \in BOOLEAN /\ verified \in BOOLEAN
  /\ illegalAck \in BOOLEAN /\ illegalDone \in BOOLEAN
  /\ illegalCloseEffect \in BOOLEAN
  /\ reconciliationAck \in BOOLEAN

AtMostOneClaim == claims <= 1
AtMostOneExternalEffect == executions <= 1
NoFabricatedAck == ~illegalAck
RecoveryAckRequiresDurableProof ==
  ~reconciliationAck \/ (phase = "Acknowledged" /\ verified /\ effect = "Committed")
AckRequiresIndependentProof ==
  phase = "Acknowledged" => (verified /\ effect = "Committed")
DoneRequiresPhysicalProof == ~illegalDone
NoEffectExecutedAfterClosure == ~illegalCloseEffect
NoAckBeforeProof == phase = "Acknowledged" => verified
NoExternalEffectBeforeClaim == executions > 0 => claims = 1

(* Unsafe: on restart, retry an effect whose first invocation succeeded
 * but ACK was lost. This would run the external host effect twice. *)
UnsafeCrashRetry ==
  /\ crashed /\ phase = "Claimed"
  /\ executions = 1
  /\ executions' = 2
  /\ UNCHANGED <<phase, physical, effect, claims, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

(* Unsafe: mark ACK with only physical DONE and no effect proof. *)
UnsafeFalseAck ==
  /\ phase = "Claimed" /\ effect = "Unknown"
  /\ phase' = "Acknowledged"
  /\ illegalAck' = TRUE
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 verified, illegalDone, illegalCloseEffect, reconciliationAck>>

(* Unsafe: a duplicated claim minted on resume from a crash. *)
UnsafeDoubleClaim ==
  /\ crashed /\ phase = "Claimed" /\ claims = 1
  /\ claims' = 2
  /\ UNCHANGED <<phase, physical, effect, executions, crashed, closed,
                 verified, illegalAck, illegalDone, illegalCloseEffect, reconciliationAck>>

(* Unsafe: physical DONE is assumed based on an earlier READY observation. *)
UnsafeFakePhysicalDone ==
  /\ phase = "Recorded" /\ physical = "Live"
  /\ phase' = "PhysicalDone"
  /\ illegalDone' = TRUE
  /\ UNCHANGED <<physical, effect, claims, executions, crashed, closed,
                 verified, illegalAck, illegalCloseEffect, reconciliationAck>>

(* Unsafe: start a new external effect after supervisor close. *)
UnsafeClosedEffect ==
  /\ closed /\ phase = "Claimed" /\ executions = 0
  /\ executions' = 1
  /\ illegalCloseEffect' = TRUE
  /\ UNCHANGED <<phase, physical, effect, claims, crashed, closed,
                 verified, illegalAck, illegalDone, reconciliationAck>>

RetryMutant == Init /\ [][Next \/ UnsafeCrashRetry]_vars
AckMutant == Init /\ [][Next \/ UnsafeFalseAck]_vars
DoubleClaimMutant == Init /\ [][Next \/ UnsafeDoubleClaim]_vars
DoneMutant == Init /\ [][Next \/ UnsafeFakePhysicalDone]_vars
ClosedMutant == Init /\ [][Next \/ UnsafeClosedEffect]_vars

(* Unsafe: a scheduler-only recovery helper releases CLAIMED even when
 * the fsynced journal has no independently committed external effect. *)
UnsafeLegacyRecoveryAck ==
  /\ phase = "Claimed" /\ ~crashed /\ ~reconciliationAck
  /\ reconciliationAck' = TRUE
  /\ UNCHANGED <<phase, physical, effect, claims, executions, crashed,
                 closed, verified, illegalAck, illegalDone, illegalCloseEffect>>

LegacyRecoveryAckMutant ==
  Init /\ [][Next \/ UnsafeLegacyRecoveryAck]_vars

=============================================================================
