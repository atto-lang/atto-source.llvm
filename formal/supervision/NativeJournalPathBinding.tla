------------------ MODULE NativeJournalPathBinding ------------------
EXTENDS Naturals

(* Abstract trusted-directory journal append protocol. "Original" means
 * the configured pathname still resolves to the open inode; "Replacement"
 * means a different inode now occupies the configured path. The journal
 * checks before and after fsync and quarantines on detected mismatch.
 * This does NOT model transient rename-and-restore between checks or
 * an adversarial directory owner. The filesystem must remain trusted.
 *)
VARIABLES phase, pathname, poisoned, committed, invalidCommit,
          resumedAfterQuarantine
vars == <<phase, pathname, poisoned, committed, invalidCommit,
          resumedAfterQuarantine>>

Init ==
  /\ phase = "Idle"
  /\ pathname = "Original"
  /\ poisoned = FALSE
  /\ committed = FALSE
  /\ invalidCommit = FALSE
  /\ resumedAfterQuarantine = FALSE

Prepare ==
  /\ phase = "Idle"
  /\ pathname = "Original"
  /\ ~poisoned
  /\ phase' = "Ready"
  /\ UNCHANGED <<pathname, poisoned, committed,
                 invalidCommit, resumedAfterQuarantine>>

RenameReplace ==
  /\ pathname = "Original"
  /\ pathname' = "Replacement"
  /\ UNCHANGED <<phase, poisoned, committed,
                 invalidCommit, resumedAfterQuarantine>>

RestorePath ==
  /\ pathname = "Replacement"
  /\ pathname' = "Original"
  /\ UNCHANGED <<phase, poisoned, committed,
                 invalidCommit, resumedAfterQuarantine>>

WriteAndFsync ==
  /\ phase = "Ready"
  /\ phase' = "Written"
  /\ UNCHANGED <<pathname, poisoned, committed,
                 invalidCommit, resumedAfterQuarantine>>

Commit ==
  /\ phase = "Written"
  /\ pathname = "Original"
  /\ ~poisoned
  /\ phase' = "Committed"
  /\ committed' = TRUE
  /\ UNCHANGED <<pathname, poisoned, invalidCommit,
                 resumedAfterQuarantine>>

Quarantine ==
  /\ phase \in {"Idle", "Ready", "Written"}
  /\ pathname = "Replacement"
  /\ ~poisoned
  /\ phase' = "Quarantined"
  /\ poisoned' = TRUE
  /\ UNCHANGED <<pathname, committed, invalidCommit,
                 resumedAfterQuarantine>>

Next == Prepare \/ RenameReplace \/ RestorePath \/ WriteAndFsync
        \/ Commit \/ Quarantine
Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"Idle", "Ready", "Written", "Committed", "Quarantined"}
  /\ pathname \in {"Original", "Replacement"}
  /\ poisoned \in BOOLEAN
  /\ committed \in BOOLEAN
  /\ invalidCommit \in BOOLEAN
  /\ resumedAfterQuarantine \in BOOLEAN

NeverAcceptWrongPath == ~invalidCommit
QuarantineNeverResumes == ~resumedAfterQuarantine
CommitIsExplicit == committed => phase = "Committed"
PoisonBlocksCommit == poisoned => ~committed

(* Wrong: silently accept a durable append while the configured
 * pathname points to a different inode than the journal FD. *)
UnsafeCommitRebound ==
  /\ phase = "Written"
  /\ pathname = "Replacement"
  /\ phase' = "Committed"
  /\ committed' = TRUE
  /\ invalidCommit' = TRUE
  /\ UNCHANGED <<pathname, poisoned, resumedAfterQuarantine>>

(* Wrong: once alias was detected, removing it reauthorizes the
 * same previously poisoned writer. *)
UnsafeResumeAfterRestore ==
  /\ phase = "Quarantined"
  /\ pathname = "Original"
  /\ phase' = "Committed"
  /\ committed' = TRUE
  /\ poisoned' = FALSE
  /\ resumedAfterQuarantine' = TRUE
  /\ UNCHANGED <<pathname, invalidCommit>>

RebindMutant == Init /\ [][Next \/ UnsafeCommitRebound]_vars
ResumeMutant == Init /\ [][Next \/ UnsafeResumeAfterRestore]_vars
=============================================================================
