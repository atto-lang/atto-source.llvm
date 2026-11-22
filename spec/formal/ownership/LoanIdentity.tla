---------------------------- MODULE LoanIdentity ----------------------------
EXTENDS Naturals, FiniteSets, TLC
(*
  Bounded lexical-loan origin/release authorization model.
  The unsafe constants enable expected counterexamples, never production.
  This is not a proof that compiler lowering implements the model.
*)
CONSTANTS UnsafeCollisions, UnsafeForeignRelease, UnsafeEntryCollision
ASSUME /\ UnsafeCollisions \in BOOLEAN
       /\ UnsafeForeignRelease \in BOOLEAN
       /\ UnsafeEntryCollision \in BOOLEAN
Sites == {"left", "right"}
Tokens == {1, 2, 3}
Stages == {"Validate", "Choose", "Borrowed", "Pending",
           "Accepted", "Reject", "ReplayRejected"}
VARIABLES stage, loanId, entryToken, origin, requested, settled
vars == <<stage, loanId, entryToken, origin, requested, settled>>
Unique == loanId["left"] # loanId["right"]
EntryDisjoint == \A site \in Sites: loanId[site] # entryToken
Accepted == stage \in {"Accepted", "ReplayRejected"}
Init ==
    /\ stage = "Validate"
    /\ loanId \in [Sites -> Tokens]
    /\ entryToken \in Tokens
    /\ origin = "None"
    /\ requested = 0
    /\ settled = 0
Validate ==
    /\ stage = "Validate"
    /\ stage' = IF (Unique \/ UnsafeCollisions) /\ (EntryDisjoint \/ UnsafeEntryCollision)
                   THEN "Choose" ELSE "Reject"
    /\ UNCHANGED <<loanId, entryToken, origin, requested, settled>>
Borrow ==
    /\ stage = "Choose"
    /\ \E site \in Sites:
        /\ stage' = "Borrowed"
        /\ origin' = site
    /\ UNCHANGED <<loanId, entryToken, requested, settled>>
RequestRelease ==
    /\ stage = "Borrowed"
    /\ \E token \in Tokens:
        /\ stage' = "Pending"
        /\ requested' = token
    /\ UNCHANGED <<loanId, entryToken, origin, settled>>
SettleRelease ==
    /\ stage = "Pending"
    /\ IF (requested = loanId[origin]) \/ UnsafeForeignRelease
       THEN /\ stage' = "Accepted"
            /\ settled' = 1
       ELSE /\ stage' = "Reject"
            /\ settled' = 0
    /\ UNCHANGED <<loanId, entryToken, origin, requested>>
RejectReplay ==
    /\ stage = "Accepted"
    /\ stage' = "ReplayRejected"
    /\ UNCHANGED <<loanId, entryToken, origin, requested, settled>>
Next == Validate \/ Borrow \/ RequestRelease \/ SettleRelease \/ RejectReplay
Spec == Init /\ [][Next]_vars
TypeOK ==
    /\ stage \in Stages
    /\ loanId \in [Sites -> Tokens]
    /\ entryToken \in Tokens
    /\ origin \in (Sites \cup {"None"})
    /\ requested \in (Tokens \cup {0})
    /\ settled \in 0..1
NoAliasedAccepted == Accepted => Unique
NoEntryAliasedAccepted == Accepted => EntryDisjoint
CorrectRelease == Accepted => requested = loanId[origin]
NoDoubleSettle == settled <= 1
=============================================================================
