---------------------------- MODULE StateScopes ----------------------------
(* Bounded abstract protocol, not an automatic refinement of the scheduler.
   Jobs may interleave only after Yield/Leave; a live state loan forbids Yield.
   Retire means final quiescent retirement, not the raw C heap retire call.
   Unwind abstracts return/throw/cancellation; fairness assumes callbacks finish.
   Mutant != "None" enables deliberately unsafe transitions for negative controls.
   See docs/WITH_USING_FORMAL_VERIFICATION.md for bounds and implementation links. *)
EXTENDS Naturals, FiniteSets, TLC

CONSTANT Mutant
ASSUME Mutant \in {"None", "Entry", "ReadWrite", "Yield", "ForeignRelease",
                  "Retire", "DoubleRelease", "Nested", "DoubleClaim", "EarlyLeave", "Leak"}

Jobs == {"writer", "reader"}
Workers == {"worker0", "worker1"}
NoJob == "none"
Terminal == {"Done", "Cancelled"}
Active == {"Running", "Body", "Loan", "Unwind"}
Phases == {"Ready", "Running", "Body", "Loan", "Unwind", "Done", "Cancelled"}

VARIABLES life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
          loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
          cancelled, releases, value, writeSafe, rejected
vars == <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
          loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
          cancelled, releases, value, writeSafe, rejected>>

Init ==
    /\ life = "Live" /\ domain = 1 /\ epoch = 0 /\ lane = NoJob
    /\ workers = [w \in Workers |-> NoJob]
    /\ phase = [j \in Jobs |-> "Ready"]
    /\ ticketDomain = [j \in Jobs |-> 0]
    /\ ticketEpoch = [j \in Jobs |-> 0]
    /\ loanOwner = NoJob /\ loanSerial = 0
    /\ loanNonce = [j \in Jobs |-> 0] /\ loanDomain = 0 /\ loanDepth = 0
    /\ cancelled = [j \in Jobs |-> FALSE]
    /\ releases = [j \in Jobs |-> 0]
    /\ value = 0 /\ writeSafe = TRUE
    /\ rejected = [j \in Jobs |-> FALSE]

TokenValid(j) == /\ lane = j /\ life # "Retired"
                 /\ ticketDomain[j] = domain /\ ticketEpoch[j] = epoch

Claim(j, w) ==
    /\ phase[j] = "Ready" /\ ~cancelled[j] /\ life = "Live"
    /\ (lane = NoJob \/ Mutant = "DoubleClaim")
    /\ workers[w] = NoJob /\ loanOwner = NoJob /\ epoch < 2
    /\ epoch' = epoch + 1 /\ lane' = j
    /\ workers' = [workers EXCEPT ![w] = j]
    /\ phase' = [phase EXCEPT ![j] = "Running"]
    /\ ticketDomain' = [ticketDomain EXCEPT ![j] = domain]
    /\ ticketEpoch' = [ticketEpoch EXCEPT ![j] = epoch + 1]
    /\ releases' = [releases EXCEPT ![j] = 0]
    /\ loanNonce' = [loanNonce EXCEPT ![j] = 0]
    /\ UNCHANGED <<life, domain, loanOwner, loanSerial, loanDomain, loanDepth,
                   cancelled, value, writeSafe, rejected>>

Enter(j) ==
    /\ phase[j] \in {"Ready", "Running", "Done", "Cancelled"}
    /\ (TokenValid(j) \/ Mutant = "Entry")
    /\ phase' = [phase EXCEPT ![j] = "Body"]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, releases, value, writeSafe, rejected>>

Reject(j) ==
    /\ ~TokenValid(j) /\ ~rejected[j]
    /\ rejected' = [rejected EXCEPT ![j] = TRUE]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, releases, value, writeSafe>>

Acquire(j) ==
    /\ phase[j] = "Body" /\ TokenValid(j) /\ loanOwner = NoJob /\ loanSerial < 2
    /\ phase' = [phase EXCEPT ![j] = "Loan"]
    /\ loanOwner' = j /\ loanSerial' = loanSerial + 1 /\ loanDomain' = domain /\ loanDepth' = 1
    /\ loanNonce' = [loanNonce EXCEPT ![j] = loanSerial + 1]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, ticketDomain, ticketEpoch,
                   cancelled, releases, value, writeSafe, rejected>>

Write(j) ==
    /\ phase[j] = "Loan" /\ loanOwner = j /\ TokenValid(j)
    /\ (j = "writer" \/ Mutant = "ReadWrite")
    /\ value' = (value + 1) % 2
    /\ writeSafe' = (writeSafe /\ (j = "writer"))
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, releases, rejected>>

Cancel(j) ==
    /\ ~cancelled[j] /\ phase[j] \notin Terminal
    /\ cancelled' = [cancelled EXCEPT ![j] = TRUE]
    /\ phase' = [phase EXCEPT ![j] = IF @ = "Ready" THEN "Cancelled" ELSE @]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   releases, value, writeSafe, rejected>>

Unwind(j) ==
    /\ phase[j] \in {"Running", "Body", "Loan"} /\ TokenValid(j)
    /\ phase' = [phase EXCEPT ![j] = "Unwind"]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, releases, value, writeSafe, rejected>>

Release(j) ==
    /\ Mutant # "Leak"
    /\ phase[j] = "Unwind" /\ loanOwner = j /\ loanDepth = 1 /\ releases[j] < 2
    /\ loanOwner' = NoJob /\ loanDepth' = 0
    /\ releases' = [releases EXCEPT ![j] = @ + 1]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanSerial, loanNonce, loanDomain, cancelled, value, writeSafe, rejected>>

Leave(j) ==
    /\ phase[j] = "Unwind" /\ TokenValid(j)
    /\ (loanOwner = NoJob \/ Mutant = "EarlyLeave")
    /\ lane' = NoJob
    /\ workers' = [w \in Workers |-> IF workers[w] = j THEN NoJob ELSE workers[w]]
    /\ phase' = [phase EXCEPT ![j] = IF cancelled[j] THEN "Cancelled" ELSE "Done"]
    /\ UNCHANGED <<life, domain, epoch, ticketDomain, ticketEpoch, loanOwner,
                   loanSerial, loanNonce, loanDomain, loanDepth, cancelled,
                   releases, value, writeSafe, rejected>>

Yield(j) ==
    /\ (phase[j] = "Body" \/ (Mutant = "Yield" /\ phase[j] = "Loan"))
    /\ TokenValid(j) /\ ~cancelled[j] /\ life = "Live"
    /\ phase' = [phase EXCEPT ![j] = "Ready"]
    /\ lane' = NoJob
    /\ workers' = [w \in Workers |-> IF workers[w] = j THEN NoJob ELSE workers[w]]
    /\ UNCHANGED <<life, domain, epoch, ticketDomain, ticketEpoch, loanOwner,
                   loanSerial, loanNonce, loanDomain, loanDepth, cancelled,
                   releases, value, writeSafe, rejected>>

Stop == /\ life = "Live" /\ life' = "Stopping"
        /\ UNCHANGED <<domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                       loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                       cancelled, releases, value, writeSafe, rejected>>

Retire ==
    /\ life = "Stopping" /\ ((lane = NoJob /\ loanOwner = NoJob) \/ Mutant = "Retire")
    /\ life' = "Retired"
    /\ UNCHANGED <<domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, releases, value, writeSafe, rejected>>

Recreate ==
    /\ life = "Retired" /\ domain < 2
    /\ life' = "Live" /\ domain' = domain + 1 /\ epoch' = 0
    /\ loanSerial' = 0 /\ loanDomain' = 0 /\ value' = 0
    /\ UNCHANGED <<lane, workers, phase, ticketDomain, ticketEpoch, loanOwner,
                   loanNonce, loanDepth, cancelled, releases, writeSafe, rejected>>

Rearm(j) ==
    /\ life = "Live" /\ phase[j] \in Terminal /\ ticketDomain[j] < domain
    /\ phase' = [phase EXCEPT ![j] = "Ready"]
    /\ cancelled' = [cancelled EXCEPT ![j] = FALSE]
    /\ releases' = [releases EXCEPT ![j] = 0]
    /\ loanNonce' = [loanNonce EXCEPT ![j] = 0]
    /\ rejected' = [rejected EXCEPT ![j] = FALSE]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanDomain, loanDepth, value, writeSafe>>

ForeignRelease(j) ==
    /\ Mutant = "ForeignRelease" /\ loanOwner # NoJob /\ loanOwner # j /\ releases[j] < 2
    /\ loanOwner' = NoJob /\ loanDepth' = 0
    /\ releases' = [releases EXCEPT ![j] = @ + 1]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanSerial, loanNonce, loanDomain, cancelled, value, writeSafe, rejected>>

DoubleRelease(j) ==
    /\ Mutant = "DoubleRelease" /\ phase[j] \in Terminal /\ releases[j] = 1
    /\ releases' = [releases EXCEPT ![j] = 2]
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, loanDepth,
                   cancelled, value, writeSafe, rejected>>

Nested(j) ==
    /\ Mutant = "Nested" /\ phase[j] = "Loan" /\ loanOwner = j /\ loanDepth = 1
    /\ loanDepth' = 2
    /\ UNCHANGED <<life, domain, epoch, lane, workers, phase, ticketDomain, ticketEpoch,
                   loanOwner, loanSerial, loanNonce, loanDomain, cancelled,
                   releases, value, writeSafe, rejected>>

Next == Stop \/ Retire \/ Recreate \/ (\E j \in Jobs:
    (\E w \in Workers: Claim(j,w)) \/ Enter(j) \/ Reject(j) \/ Acquire(j) \/ Write(j)
    \/ Cancel(j) \/ Unwind(j) \/ Release(j) \/ Leave(j) \/ Yield(j) \/ Rearm(j)
    \/ ForeignRelease(j) \/ DoubleRelease(j) \/ Nested(j))

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ (\A j \in Jobs: WF_vars(Unwind(j)) /\ WF_vars(Release(j)) /\ WF_vars(Leave(j)))

TypeOK ==
    /\ life \in {"Live", "Stopping", "Retired"} /\ domain \in 1..2 /\ epoch \in 0..2
    /\ lane \in Jobs \cup {NoJob} /\ workers \in [Workers -> Jobs \cup {NoJob}]
    /\ phase \in [Jobs -> Phases] /\ ticketDomain \in [Jobs -> 0..2] /\ ticketEpoch \in [Jobs -> 0..2]
    /\ loanOwner \in Jobs \cup {NoJob} /\ loanSerial \in 0..2 /\ loanNonce \in [Jobs -> 0..2]
    /\ loanDomain \in 0..2 /\ loanDepth \in 0..2 /\ cancelled \in [Jobs -> BOOLEAN]
    /\ releases \in [Jobs -> 0..2] /\ value \in 0..1 /\ writeSafe \in BOOLEAN /\ rejected \in [Jobs -> BOOLEAN]
SinglePhysicalCarrier == Cardinality({w \in Workers: workers[w] # NoJob}) <= 1
CarrierAgreement == /\ (lane = NoJob) = (\A w \in Workers: workers[w] = NoJob)
                    /\ \A w \in Workers: workers[w] # NoJob => workers[w] = lane
GuestEntryValid == \A j \in Jobs: phase[j] \in {"Body", "Loan", "Unwind"} => TokenValid(j)
LoanSound ==
    /\ loanDepth <= 1 /\ (loanOwner = NoJob) = (loanDepth = 0)
    /\ (loanOwner # NoJob => /\ TokenValid(loanOwner) /\ loanDomain = domain
                            /\ loanNonce[loanOwner] = loanSerial
                            /\ phase[loanOwner] \in {"Loan", "Unwind"})
    /\ (\A j \in Jobs: phase[j] = "Loan" => loanOwner = j)
ReadDoesNotWrite == writeSafe
RetiredQuiescent == life = "Retired" => lane = NoJob /\ loanOwner = NoJob
ExactlyOnceCleanup == /\ (\A j \in Jobs: releases[j] <= 1)
                      /\ (\A j \in Jobs: (phase[j] \in Terminal /\ loanNonce[j] # 0) => releases[j] = 1)
CancelLeadsToCleanup == \A j \in Jobs: (cancelled[j] ~> (phase[j] \in Terminal))

(* Expected-to-fail invariants prove these scenarios are reachable. *)
NeverReadLoan == ~(loanOwner = "reader" /\ phase["reader"] = "Loan")
NeverWriteLoan == ~(loanOwner = "writer" /\ phase["writer"] = "Loan")
NeverCancelledLoan == ~(\E j \in Jobs: loanOwner = j /\ cancelled[j])
NeverRecreated == domain = 1
NeverInterleaved == ~(phase["writer"] = "Ready" /\ ticketEpoch["writer"] > 0 /\ lane = "reader")
NeverCleaned == ~(\E j \in Jobs: phase[j] \in Terminal /\ loanNonce[j] # 0 /\ releases[j] = 1)
NeverGuestWrite == value = 0
=============================================================================
