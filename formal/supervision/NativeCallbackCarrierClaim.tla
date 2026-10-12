------------------- MODULE NativeCallbackCarrierClaim -------------------
EXTENDS Naturals

(*
 * Abstract identity-mode typed PANIC admission protocol.
 * Merely recording/queuing a terminal receipt is not authority to invoke
 * supervisor policy. The scheduler's exact callback carrier must be
 * executing and bound to the ORIGINAL parent incarnation.
 * This verifies an abstract protocol, not the source code's TLS semantics.
 *)
VARIABLES phase, parent, carrier, event, receipt, claims, accepted,
          offCarrierClaim, lateClaim, wrongGenerationClaim
vars == <<phase, parent, carrier, event, receipt, claims, accepted,
          offCarrierClaim, lateClaim, wrongGenerationClaim>>

Init ==
  /\ phase = "New"
  /\ parent = "LiveOriginal"
  /\ carrier = "Idle"
  /\ event = "None"
  /\ receipt = "None"
  /\ claims = 0
  /\ accepted = 0
  /\ offCarrierClaim = FALSE
  /\ lateClaim = FALSE
  /\ wrongGenerationClaim = FALSE

QueueTerminal ==
  /\ phase = "New"
  /\ parent = "LiveOriginal"
  /\ phase' = "Queued"
  /\ event' = "Original"
  /\ receipt' = "Recorded"
  /\ UNCHANGED <<parent, carrier, claims, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

BeginCarrier ==
  /\ phase = "Queued"
  /\ carrier = "Idle"
  /\ carrier' = "Original"
  /\ phase' = "Delivering"
  /\ UNCHANGED <<parent, event, receipt, claims, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

StopParent ==
  /\ parent = "LiveOriginal"
  /\ parent' = "Stopped"
  /\ UNCHANGED <<phase, carrier, event, receipt, claims, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

RecycleParent ==
  /\ parent = "Stopped"
  /\ parent' = "LiveNext"
  /\ UNCHANGED <<phase, carrier, event, receipt, claims, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

ClaimOnCarrier ==
  /\ phase = "Delivering"
  /\ carrier = "Original"
  /\ event = "Original"
  /\ parent = "LiveOriginal"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ claims' = claims + 1
  /\ UNCHANGED <<phase, parent, carrier, event, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

AcceptOnce ==
  /\ receipt = "Claimed"
  /\ accepted = 0
  /\ accepted' = 1
  /\ UNCHANGED <<phase, parent, carrier, event, receipt, claims,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

LeaveCarrier ==
  /\ carrier = "Original"
  /\ carrier' = "Idle"
  /\ phase' = "Done"
  /\ UNCHANGED <<parent, event, receipt, claims, accepted,
                 offCarrierClaim, lateClaim, wrongGenerationClaim>>

Next == QueueTerminal \/ BeginCarrier \/ StopParent
     \/ RecycleParent \/ ClaimOnCarrier \/ AcceptOnce \/ LeaveCarrier
Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"New", "Queued", "Delivering", "Done"}
  /\ parent \in {"LiveOriginal", "Stopped", "LiveNext"}
  /\ carrier \in {"Idle", "Original"}
  /\ event \in {"None", "Original"}
  /\ receipt \in {"None", "Recorded", "Claimed"}
  /\ claims \in 0..2
  /\ accepted \in 0..2
  /\ offCarrierClaim \in BOOLEAN
  /\ lateClaim \in BOOLEAN
  /\ wrongGenerationClaim \in BOOLEAN

NoOffCarrierClaim == ~offCarrierClaim
NoClaimAfterStop == ~lateClaim
NoRecycledParentBorrow == ~wrongGenerationClaim
OneUseClaim == claims <= 1
OneHostAdmission == accepted <= 1
AdmittedImpliesClaim == accepted > 0 => receipt = "Claimed"
NoClaimBeforeQueued == claims > 0 => event = "Original"

(* Incorrect: queued receipt is sufficient without executing the CONTROL
 * callback. The mutation must fail NoOffCarrierClaim. *)
UnsafeQueuedClaim ==
  /\ phase = "Queued"
  /\ carrier = "Idle"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ claims' = claims + 1
  /\ offCarrierClaim' = TRUE
  /\ UNCHANGED <<phase, parent, carrier, event, accepted,
                 lateClaim, wrongGenerationClaim>>

(* Incorrect: physical parent stopped after the callback began and before
 * the one-use claim. The claim must not succeed. *)
UnsafeLateClaim ==
  /\ carrier = "Original"
  /\ parent = "Stopped"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ claims' = claims + 1
  /\ lateClaim' = TRUE
  /\ UNCHANGED <<phase, parent, carrier, event, accepted,
                 offCarrierClaim, wrongGenerationClaim>>

(* Incorrect: original callback is still executing after parent slot
 * reincarnated. New parent's generation cannot authorize old CONTROL. *)
UnsafeRecycledParentClaim ==
  /\ carrier = "Original"
  /\ parent = "LiveNext"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ claims' = claims + 1
  /\ wrongGenerationClaim' = TRUE
  /\ UNCHANGED <<phase, parent, carrier, event, accepted,
                 offCarrierClaim, lateClaim>>

OffCarrierMutant == Init /\ [][Next \/ UnsafeQueuedClaim]_vars
LateClaimMutant == Init /\ [][Next \/ UnsafeLateClaim]_vars
RecycledMutant == Init /\ [][Next \/ UnsafeRecycledParentClaim]_vars
=============================================================================
