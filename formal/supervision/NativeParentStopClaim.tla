-------------------- MODULE NativeParentStopClaim --------------------
EXTENDS Naturals

(*
 * Bounded, abstract scheduler terminal receipt authority:
 * original-parent callback must atomically CLAIM while parent is LIVE.
 * A STOP-first receipt remains RECORDED until a separate verified
 * recovered-parent claimant takes ownership. Claimed effects are one-use.
 * This model does not establish process-crash durability, host enqueue
 * completion, or a machine-checked refinement to C.
 *)
VARIABLES parent, recovered, receipt, origin, claims, effects,
          originalClaimAfterStop
vars == <<parent, recovered, receipt, origin, claims, effects,
          originalClaimAfterStop>>

Init ==
  /\ parent = "Live"
  /\ recovered = FALSE
  /\ receipt = "Recorded"
  /\ origin = "None"
  /\ claims = 0
  /\ effects = 0
  /\ originalClaimAfterStop = FALSE

StopParent ==
  /\ parent = "Live"
  /\ parent' = "Stopped"
  /\ UNCHANGED <<recovered, receipt, origin, claims, effects,
                 originalClaimAfterStop>>

OriginalClaim ==
  /\ parent = "Live"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ origin' = "Original"
  /\ claims' = claims + 1
  /\ UNCHANGED <<parent, recovered, effects, originalClaimAfterStop>>

RecoverParent ==
  /\ parent = "Stopped"
  /\ ~recovered
  /\ recovered' = TRUE
  /\ UNCHANGED <<parent, receipt, origin, claims, effects,
                 originalClaimAfterStop>>

RecoveredClaim ==
  /\ parent = "Stopped"
  /\ recovered
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ origin' = "Recovered"
  /\ claims' = claims + 1
  /\ UNCHANGED <<parent, recovered, effects, originalClaimAfterStop>>

AcceptEffect ==
  /\ receipt = "Claimed"
  /\ effects = 0
  /\ effects' = 1
  /\ UNCHANGED <<parent, recovered, receipt, origin, claims,
                 originalClaimAfterStop>>

Acknowledge ==
  /\ receipt = "Claimed"
  /\ effects = 1
  /\ receipt' = "Acknowledged"
  /\ UNCHANGED <<parent, recovered, origin, claims, effects,
                 originalClaimAfterStop>>

Next == StopParent \/ OriginalClaim \/ RecoverParent \/ RecoveredClaim
        \/ AcceptEffect \/ Acknowledge
Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ parent \in {"Live", "Stopped"}
  /\ recovered \in BOOLEAN
  /\ receipt \in {"Recorded", "Claimed", "Acknowledged"}
  /\ origin \in {"None", "Original", "Recovered"}
  /\ claims \in 0..2
  /\ effects \in 0..2
  /\ originalClaimAfterStop \in BOOLEAN

NoOriginalClaimAfterStop == ~originalClaimAfterStop
OneUseClaim == claims <= 1
OneUseHostEffect == effects <= 1
AckHasEffect == receipt = "Acknowledged" => effects = 1
ClaimOwnerBound == (receipt = "Recorded") => (claims = 0 /\ origin = "None")
NoRecoveredClaimWithoutRecoveredParent ==
  origin = "Recovered" => recovered

(* STOP-first: deliberately allow an ordinary callback to claim the
 * original retired parent's receipt. Expected safety violation. *)
UnsafeLateOriginalClaim ==
  /\ parent = "Stopped"
  /\ receipt = "Recorded"
  /\ receipt' = "Claimed"
  /\ origin' = "Original"
  /\ claims' = claims + 1
  /\ originalClaimAfterStop' = TRUE
  /\ UNCHANGED <<parent, recovered, effects>>

(* Duplicate claimant: a terminal effect token is issued twice. *)
UnsafeDuplicateClaim ==
  /\ receipt = "Claimed"
  /\ claims = 1
  /\ claims' = 2
  /\ UNCHANGED <<parent, recovered, receipt, origin, effects,
                 originalClaimAfterStop>>

LateClaimMutantSpec == Init /\ [][Next \/ UnsafeLateOriginalClaim]_vars
DuplicateMutantSpec == Init /\ [][Next \/ UnsafeDuplicateClaim]_vars
=============================================================================
