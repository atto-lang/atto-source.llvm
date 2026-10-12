-------------------------- MODULE OresVMScheduler --------------------------
EXTENDS Naturals, FiniteSets, TLC

(*
 * Finite-state model of the OresVM master -> pool CONTROL -> 0..5 carrier
 * ownership boundary. The one-thread properties are represented as Boolean
 * masterAlive and poolActive flags; exactly one control authority exists
 * per pool, not one thread per actor.
 *
 * Source compiler / Truffle continuation admission is not modeled here:
 * this deliberately checks host scheduler safety rather than pretending to
 * prove complete guest-language semantics.
 *)
CONSTANTS Pools, Actors, ActorA, ActorB, PoolA, PoolB, MaxWorkers
Owner == [a \in Actors |-> IF a = ActorA THEN PoolA ELSE PoolB]
ASSUME /\ MaxWorkers \in Nat
       /\ MaxWorkers > 0
       /\ ActorA # ActorB
       /\ Actors = {ActorA, ActorB}
       /\ PoolA \in Pools
       /\ PoolB \in Pools

VARIABLES vm, masterAlive, poolActive, workers, status, lease,
          queued, wakeMaster, wakePool, cancelled
vars == <<vm, masterAlive, poolActive, workers, status, lease,
          queued, wakeMaster, wakePool, cancelled>>

Busy(p) == {a \in Actors :
               /\ Owner[a] = p
               /\ status[a] \in {"Reserved", "Running"}}
Awaiting(p) == {a \in Actors :
                  /\ Owner[a] = p
                  /\ status[a] \in {"Queued", "Ready"}}

Init ==
    /\ vm = "New"
    /\ masterAlive = FALSE
    /\ poolActive = [p \in Pools |-> FALSE]
    /\ workers = [p \in Pools |-> 0]
    /\ status = [a \in Actors |-> "New"]
    /\ lease = [a \in Actors |-> FALSE]
    /\ queued = [a \in Actors |-> 0]
    /\ wakeMaster = [p \in Pools |-> FALSE]
    /\ wakePool = [p \in Pools |-> FALSE]
    /\ cancelled = [a \in Actors |-> FALSE]

Start ==
    /\ vm = "New"
    /\ vm' = "Running"
    /\ masterAlive' = TRUE
    /\ UNCHANGED <<poolActive, workers, status, lease,
                    queued, wakeMaster, wakePool, cancelled>>

Spawn(a) ==
    /\ vm = "Running"
    /\ status[a] = "New"
    /\ status' = [status EXCEPT ![a] = "Idle"]
    /\ poolActive' = [poolActive EXCEPT ![Owner[a]] = TRUE]
    /\ UNCHANGED <<vm, masterAlive, workers, lease, queued,
                    wakeMaster, wakePool, cancelled>>

Submit(a) ==
    /\ vm = "Running"
    /\ status[a] \in {"Idle", "Queued", "Running", "Reserved", "Suspended", "Ready"}
    /\ queued[a] < 2
    /\ queued' = [queued EXCEPT ![a] = @ + 1]
    /\ status' = IF status[a] = "Idle"
                  THEN [status EXCEPT ![a] = "Queued"]
                  ELSE status
    /\ wakePool' = [wakePool EXCEPT ![Owner[a]] = TRUE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, lease,
                    wakeMaster, cancelled>>

SignalFromMaster(p) ==
    /\ vm = "Running"
    /\ poolActive[p]
    /\ wakeMaster' = [wakeMaster EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, status,
                    lease, queued, wakePool, cancelled>>

DeliverWake(p) ==
    /\ wakeMaster[p]
    /\ wakeMaster' = [wakeMaster EXCEPT ![p] = FALSE]
    /\ wakePool' = [wakePool EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, status,
                    lease, queued, cancelled>>

AddWorker(p) ==
    /\ poolActive[p]
    /\ workers[p] < MaxWorkers
    /\ Awaiting(p) # {}
    /\ workers' = [workers EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<vm, masterAlive, poolActive, status, lease,
                    queued, wakeMaster, wakePool, cancelled>>

IssueTicket(a) ==
    LET p == Owner[a] IN
    /\ vm \in {"Running", "Stopping"}
    /\ status[a] \in {"Queued", "Ready"}
    /\ workers[p] > Cardinality(Busy(p))
    /\ status' = [status EXCEPT ![a] = "Reserved"]
    /\ lease' = [lease EXCEPT ![a] = TRUE]
    /\ queued' = IF status[a] = "Queued"
                  THEN [queued EXCEPT ![a] = @ - 1]
                  ELSE queued
    /\ wakePool' = [wakePool EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers,
                    wakeMaster, cancelled>>

RunTicket(a) ==
    /\ status[a] = "Reserved"
    /\ status' = [status EXCEPT ![a] = "Running"]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, lease,
                    queued, wakeMaster, wakePool, cancelled>>

Suspend(a) ==
    /\ status[a] = "Running"
    /\ status' = [status EXCEPT ![a] = "Suspended"]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, lease,
                    queued, wakeMaster, wakePool, cancelled>>

Ready(a) ==
    /\ status[a] = "Suspended"
    /\ status' = [status EXCEPT ![a] = "Ready"]
    /\ wakePool' = [wakePool EXCEPT ![Owner[a]] = TRUE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, lease,
                    queued, wakeMaster, cancelled>>

Finish(a) ==
    /\ status[a] = "Running"
    /\ status' = [status EXCEPT ![a] =
                  IF cancelled[a] THEN "Cleaning"
                  ELSE IF queued[a] > 0 THEN "Queued" ELSE "Idle"]
    /\ lease' = [lease EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, queued,
                    wakeMaster, wakePool, cancelled>>

Cancel(a) ==
    /\ status[a] \in {"Idle", "Queued", "Suspended", "Ready",
                         "Reserved", "Running"}
    /\ ~cancelled[a]
    /\ cancelled' = [cancelled EXCEPT ![a] = TRUE]
    /\ status' = IF status[a] \in {"Idle", "Queued", "Suspended", "Ready"}
                  THEN [status EXCEPT ![a] = "Cleaning"]
                  ELSE status
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers, lease,
                    queued, wakeMaster, wakePool>>

Cleanup(a) ==
    /\ status[a] = "Cleaning"
    /\ status' = [status EXCEPT ![a] = "Done"]
    /\ lease' = [lease EXCEPT ![a] = FALSE]
    /\ queued' = [queued EXCEPT ![a] = 0]
    /\ UNCHANGED <<vm, masterAlive, poolActive, workers,
                    wakeMaster, wakePool, cancelled>>

Shutdown ==
    /\ vm = "Running"
    /\ vm' = "Stopping"
    /\ UNCHANGED <<masterAlive, poolActive, workers, status, lease,
                    queued, wakeMaster, wakePool, cancelled>>

StopVM ==
    /\ vm = "Stopping"
    /\ \A a \in Actors : status[a] \in {"Done", "New", "Idle"}
    /\ vm' = "Stopped"
    /\ masterAlive' = FALSE
    /\ poolActive' = [p \in Pools |-> FALSE]
    /\ workers' = [p \in Pools |-> 0]
    /\ wakeMaster' = [p \in Pools |-> FALSE]
    /\ wakePool' = [p \in Pools |-> FALSE]
    /\ UNCHANGED <<status, lease, queued, cancelled>>

Next ==
    \/ Start
    \/ \E a \in Actors :
       Spawn(a) \/ Submit(a) \/ IssueTicket(a) \/ RunTicket(a)
       \/ Suspend(a) \/ Ready(a) \/ Finish(a) \/ Cancel(a)
       \/ Cleanup(a)
    \/ \E p \in Pools : SignalFromMaster(p) \/ DeliverWake(p) \/ AddWorker(p)
    \/ Shutdown
    \/ StopVM
    \/ (vm = "Stopped" /\ UNCHANGED vars)

TypeOK ==
    /\ vm \in {"New", "Running", "Stopping", "Stopped"}
    /\ masterAlive \in BOOLEAN
    /\ poolActive \in [Pools -> BOOLEAN]
    /\ workers \in [Pools -> 0..MaxWorkers]
    /\ status \in [Actors -> {"New", "Idle", "Queued", "Reserved",
                               "Running", "Suspended", "Ready", "Cleaning", "Done"}]
    /\ lease \in [Actors -> BOOLEAN]
    /\ cancelled \in [Actors -> BOOLEAN]
    /\ queued \in [Actors -> 0..2]
    /\ wakeMaster \in [Pools -> BOOLEAN]
    /\ wakePool \in [Pools -> BOOLEAN]

CarrierBound == \A p \in Pools : Cardinality(Busy(p)) <= workers[p]
LeaseOwnership ==
    \A a \in Actors :
        status[a] \in {"Reserved", "Running", "Suspended", "Ready"} => lease[a]
DormantHasZeroCarriers ==
    \A p \in Pools : ~poolActive[p] => workers[p] = 0
StoppedHasNoWorkers == vm = "Stopped" => \A p \in Pools : workers[p] = 0
NoPrematureFinalization ==
    \A a \in Actors : status[a] = "Done" => ~lease[a]
MasterDuringRuntime == vm = "Running" => masterAlive

Spec == Init /\ [][Next]_vars
=============================================================================
