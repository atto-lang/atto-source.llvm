-------------------- MODULE NativeRecycledHandoff --------------------
EXTENDS Naturals, TLC
(*
 * Host-owned recycled candidate handoff. CONTROL may close at either
 * handoff seam. Until logical + physical publication, a Hidden actor
 * must have a transaction owner. On close, DONE precedes ticket rollback.
 *)
VARIABLES stage, physical, logical, ticket, owner, closed, published, badCommit
vars == <<stage, physical, logical, ticket, owner, closed, published, badCommit>>

Init ==
  /\ stage = "Initial"
  /\ physical = "Done"
  /\ logical = "Finalized"
  /\ ticket = FALSE
  /\ owner = FALSE
  /\ closed = FALSE
  /\ published = FALSE
  /\ badCommit = FALSE

Recycle ==
  /\ stage = "Initial" /\ ~closed /\ physical = "Done"
  /\ stage' = "Hidden" /\ physical' = "Hidden" /\ owner' = TRUE
  /\ UNCHANGED <<logical, ticket, closed, published, badCommit>>

Reserve ==
  /\ stage = "Hidden" /\ owner /\ ~closed
  /\ stage' = "Reserved" /\ logical' = "Restarting" /\ ticket' = TRUE
  /\ UNCHANGED <<physical, owner, closed, published, badCommit>>

Close ==
  /\ ~closed /\ closed' = TRUE
  /\ UNCHANGED <<stage, physical, logical, ticket, owner, published, badCommit>>

Commit ==
  /\ stage = "Reserved" /\ ticket /\ owner /\ ~closed
  /\ stage' = "Published" /\ physical' = "Active"
  /\ logical' = "Active" /\ ticket' = FALSE /\ owner' = FALSE
  /\ published' = TRUE /\ UNCHANGED <<closed, badCommit>>

PhysicalCleanup ==
  /\ closed /\ owner /\ physical = "Hidden"
  /\ stage \in {"Hidden", "Reserved"}
  /\ stage' = "Terminating" /\ physical' = "Done" /\ owner' = FALSE
  /\ UNCHANGED <<logical, ticket, closed, published, badCommit>>

AbortLogical ==
  /\ stage = "Terminating" /\ physical = "Done"
  /\ stage' = "RolledBack" /\ logical' = "Finalized" /\ ticket' = FALSE
  /\ UNCHANGED <<physical, owner, closed, published, badCommit>>

Next == Recycle \/ Reserve \/ Close \/ Commit \/ PhysicalCleanup \/ AbortLogical
Spec == Init /\ [][Next]_vars /\ WF_vars(PhysicalCleanup) /\ WF_vars(AbortLogical)

TypeOK ==
  /\ stage \in {"Initial", "Hidden", "Reserved", "Terminating", "RolledBack", "Published"}
  /\ physical \in {"Done", "Hidden", "Active"}
  /\ logical \in {"Finalized", "Restarting", "Active"}
  /\ ticket \in BOOLEAN /\ owner \in BOOLEAN /\ closed \in BOOLEAN
  /\ published \in BOOLEAN /\ badCommit \in BOOLEAN

NoLiveUnowned == physical = "Hidden" => owner
NoPrematureRouting == published => (stage = "Published" /\ physical = "Active" /\ logical = "Active")
NoClosedPublication == ~badCommit
CleanRollback == stage = "RolledBack" =>
  (physical = "Done" /\ logical = "Finalized" /\ ~ticket /\ ~owner)
ClosedEventuallyDone == (closed /\ physical = "Hidden") ~> (physical = "Done")
ClosedEventuallyRolledBack == (closed /\ stage = "Terminating") ~> (stage = "RolledBack")

UnsafeDropOwnership ==
  /\ stage = "Hidden" /\ owner /\ owner' = FALSE
  /\ UNCHANGED <<stage, physical, logical, ticket, closed, published, badCommit>>

UnsafeCommitAfterClose ==
  /\ closed /\ stage = "Reserved" /\ ticket /\ owner
  /\ stage' = "Published" /\ physical' = "Active" /\ logical' = "Active"
  /\ ticket' = FALSE /\ owner' = FALSE /\ published' = TRUE /\ badCommit' = TRUE
  /\ UNCHANGED <<closed>>

OwnerMutantSpec == Init /\ [][Next \/ UnsafeDropOwnership]_vars
ClosedMutantSpec == Init /\ [][Next \/ UnsafeCommitAfterClose]_vars
=============================================================================
