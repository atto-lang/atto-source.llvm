-------------------- MODULE NativeDualFenceHandoff --------------------
EXTENDS Naturals, TLC

(* Abstract trusted-host handoff after a previously hidden actor reaches DONE.
 * Registry admission and physical parent CONTROL close independently.
 * A recycled live hidden generation stays owned until it is published or
 * is physically DONE, followed by logical reservation rollback.
 * This model does not represent process crash durability or external effects.
 *)
VARIABLES stage, physical, logical, ticket, owner, registryClosed,
          controlClosed, published, badCommit
vars == <<stage, physical, logical, ticket, owner, registryClosed,
          controlClosed, published, badCommit>>

Init ==
  /\ stage = "Initial"
  /\ physical = "Done"
  /\ logical = "Finalized"
  /\ ticket = FALSE
  /\ owner = FALSE
  /\ registryClosed = FALSE
  /\ controlClosed = FALSE
  /\ published = FALSE
  /\ badCommit = FALSE

Recycle ==
  /\ stage = "Initial"
  /\ physical = "Done"
  /\ ~registryClosed /\ ~controlClosed
  /\ stage' = "Hidden" /\ physical' = "Hidden" /\ owner' = TRUE
  /\ UNCHANGED <<logical, ticket, registryClosed, controlClosed, published, badCommit>>

Reserve ==
  /\ stage = "Hidden" /\ owner
  /\ ~registryClosed /\ ~controlClosed
  /\ stage' = "Reserved" /\ logical' = "Restarting" /\ ticket' = TRUE
  /\ UNCHANGED <<physical, owner, registryClosed, controlClosed, published, badCommit>>

CloseRegistry ==
  /\ ~registryClosed /\ registryClosed' = TRUE
  /\ UNCHANGED <<stage, physical, logical, ticket, owner, controlClosed,
                 published, badCommit>>

CloseControl ==
  /\ ~controlClosed /\ controlClosed' = TRUE
  /\ UNCHANGED <<stage, physical, logical, ticket, owner, registryClosed,
                 published, badCommit>>

Commit ==
  /\ stage = "Reserved" /\ ticket /\ owner
  /\ ~registryClosed /\ ~controlClosed
  /\ stage' = "Published" /\ physical' = "Active"
  /\ logical' = "Active" /\ ticket' = FALSE /\ owner' = FALSE
  /\ published' = TRUE
  /\ UNCHANGED <<registryClosed, controlClosed, badCommit>>

PhysicalCleanup ==
  /\ (registryClosed \/ controlClosed)
  /\ owner /\ physical = "Hidden"
  /\ stage \in {"Hidden", "Reserved"}
  /\ stage' = "Terminating" /\ physical' = "Done" /\ owner' = FALSE
  /\ UNCHANGED <<logical, ticket, registryClosed, controlClosed, published, badCommit>>

AbortLogical ==
  /\ stage = "Terminating" /\ physical = "Done"
  /\ stage' = "RolledBack" /\ logical' = "Finalized" /\ ticket' = FALSE
  /\ UNCHANGED <<physical, owner, registryClosed, controlClosed, published, badCommit>>

Next == Recycle \/ Reserve \/ CloseRegistry \/ CloseControl
     \/ Commit \/ PhysicalCleanup \/ AbortLogical
Spec == Init /\ [][Next]_vars /\ WF_vars(PhysicalCleanup) /\ WF_vars(AbortLogical)

TypeOK ==
  /\ stage \in {"Initial", "Hidden", "Reserved", "Terminating",
               "RolledBack", "Published"}
  /\ physical \in {"Done", "Hidden", "Active"}
  /\ logical \in {"Finalized", "Restarting", "Active"}
  /\ ticket \in BOOLEAN /\ owner \in BOOLEAN
  /\ registryClosed \in BOOLEAN /\ controlClosed \in BOOLEAN
  /\ published \in BOOLEAN /\ badCommit \in BOOLEAN

NoUnownedHidden == physical = "Hidden" => owner
NoPrematureRouting == published => (stage = "Published"
                                  /\ physical = "Active" /\ logical = "Active")
NoClosedCommit == ~badCommit
CleanRollback == stage = "RolledBack" =>
  (physical = "Done" /\ logical = "Finalized" /\ ~ticket /\ ~owner)
EitherFenceEventuallyDone ==
  ((registryClosed \/ controlClosed) /\ physical = "Hidden") ~> (physical = "Done")
CleanupEventuallyRollsBack ==
  (stage = "Terminating") ~> (stage = "RolledBack")

(* Deliberately invalid mutation: abandon live hidden physical generation. *)
UnsafeDropOwnership ==
  /\ stage = "Hidden" /\ owner
  /\ owner' = FALSE
  /\ UNCHANGED <<stage, physical, logical, ticket, registryClosed,
                 controlClosed, published, badCommit>>

(* Deliberately invalid mutation: CONTROL stays open but registry admission closed. *)
UnsafeCommitAfterRegistryClose ==
  /\ registryClosed /\ ~controlClosed
  /\ stage = "Reserved" /\ ticket /\ owner
  /\ stage' = "Published" /\ physical' = "Active" /\ logical' = "Active"
  /\ ticket' = FALSE /\ owner' = FALSE /\ published' = TRUE
  /\ badCommit' = TRUE
  /\ UNCHANGED <<registryClosed, controlClosed>>

(* Deliberately invalid mutation: registry stays open but CONTROL is closed. *)
UnsafeCommitAfterControlClose ==
  /\ controlClosed /\ ~registryClosed
  /\ stage = "Reserved" /\ ticket /\ owner
  /\ stage' = "Published" /\ physical' = "Active" /\ logical' = "Active"
  /\ ticket' = FALSE /\ owner' = FALSE /\ published' = TRUE
  /\ badCommit' = TRUE
  /\ UNCHANGED <<registryClosed, controlClosed>>

OwnerMutantSpec == Init /\ [][Next \/ UnsafeDropOwnership]_vars
RegistryMutantSpec == Init /\ [][Next \/ UnsafeCommitAfterRegistryClose]_vars
ControlMutantSpec == Init /\ [][Next \/ UnsafeCommitAfterControlClose]_vars
=============================================================================
