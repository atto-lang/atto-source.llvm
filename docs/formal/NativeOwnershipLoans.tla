---------------------- MODULE NativeOwnershipLoans ----------------------
EXTENDS Naturals, FiniteSets
(*
  Abstract bounded admission model, NOT a proof of LLVM lowering/runtime.
  Immutable loan identities prevent false release of other borrow sites.
*)
CONSTANTS Loans, NoLoan
ASSUME /\ IsFiniteSet(Loans)
       /\ Loans # {}
       /\ NoLoan \notin Loans
VARIABLES owner, readers, writer, suspended
vars == <<owner, readers, writer, suspended>>
Init ==
  /\ owner = "owned"
  /\ readers = {}
  /\ writer = NoLoan
  /\ suspended = FALSE
BorrowShared(l) ==
  /\ owner = "owned"
  /\ ~suspended
  /\ writer = NoLoan
  /\ l \in Loans \ readers
  /\ readers' = readers \cup {l}
  /\ UNCHANGED <<owner, writer, suspended>>
BorrowExclusive(l) ==
  /\ owner = "owned"
  /\ ~suspended
  /\ readers = {}
  /\ writer = NoLoan
  /\ l \in Loans
  /\ writer' = l
  /\ UNCHANGED <<owner, readers, suspended>>
ReleaseShared(l) ==
  /\ l \in readers
  /\ readers' = readers \ {l}
  /\ UNCHANGED <<owner, writer, suspended>>
ReleaseExclusive(l) ==
  /\ writer = l
  /\ l \in Loans
  /\ writer' = NoLoan
  /\ UNCHANGED <<owner, readers, suspended>>
Take ==
  /\ owner = "owned"
  /\ readers = {}
  /\ writer = NoLoan
  /\ ~suspended
  /\ owner' = "moved"
  /\ UNCHANGED <<readers, writer, suspended>>
Restore ==
  /\ owner = "moved"
  /\ owner' = "owned"
  /\ UNCHANGED <<readers, writer, suspended>>
Suspend ==
  /\ owner = "owned"
  /\ readers = {}
  /\ writer = NoLoan
  /\ ~suspended
  /\ suspended' = TRUE
  /\ UNCHANGED <<owner, readers, writer>>
Resume ==
  /\ suspended
  /\ suspended' = FALSE
  /\ UNCHANGED <<owner, readers, writer>>
Next ==
  \/ \E l \in Loans : BorrowShared(l)
  \/ \E l \in Loans : BorrowExclusive(l)
  \/ \E l \in Loans : ReleaseShared(l)
  \/ \E l \in Loans : ReleaseExclusive(l)
  \/ Take \/ Restore \/ Suspend \/ Resume
Spec == Init /\ [][Next]_vars
TypeOK ==
  /\ owner \in {"owned", "moved"}
  /\ readers \subseteq Loans
  /\ writer \in Loans \cup {NoLoan}
  /\ suspended \in BOOLEAN
NoOverlappingExclusive ==
  (writer # NoLoan) => (readers = {})
NoLoanAfterMove ==
  (owner = "moved") => (readers = {} /\ writer = NoLoan)
NoBorrowAcrossSuspend ==
  suspended => (readers = {} /\ writer = NoLoan)
Safety == TypeOK /\ NoOverlappingExclusive /\ NoLoanAfterMove /\ NoBorrowAcrossSuspend
=============================================================================
