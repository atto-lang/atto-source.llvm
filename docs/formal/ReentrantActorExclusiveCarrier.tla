---------------- MODULE ReentrantActorExclusiveCarrier ----------------
EXTENDS Naturals, FiniteSets

(* Finite TLC instance: two actors, two carriers and three jobs.
   job0/job1 share actor0. All transitions represent atomic scheduler
   admission/release actions; guest code cannot mutate these variables. *)
CONSTANTS Actor0, Actor1, Worker0, Worker1,
          Job0, Job1, Job2, NoWorker, NoJob

Actors == {Actor0, Actor1}
Workers == {Worker0, Worker1}
Jobs == {Job0, Job1, Job2}
JobActor == [Job0 |-> Actor0, Job1 |-> Actor0, Job2 |-> Actor1]

ASSUME /\ Cardinality(Actors) = 2
       /\ Cardinality(Workers) = 2
       /\ Cardinality(Jobs) = 3
       /\ NoWorker \notin Workers
       /\ NoJob \notin Jobs

VARIABLES lease, carrierJob, phase, stopped
vars == <<lease, carrierJob, phase, stopped>>

Init ==
    /\ lease = [a \in Actors |-> NoWorker]
    /\ carrierJob = [w \in Workers |-> NoJob]
    /\ phase = [j \in Jobs |-> "ready"]
    /\ stopped = [a \in Actors |-> FALSE]

(* A successful CAS/locked claim is indivisible. A task is runnable only
   after resume was ENQUEUED, not on its future-completion producer thread. *)
Claim(w, j) ==
    /\ w \in Workers
    /\ j \in Jobs
    /\ phase[j] = "ready"
    /\ ~stopped[JobActor[j]]
    /\ carrierJob[w] = NoJob
    /\ lease[JobActor[j]] = NoWorker
    /\ lease' = [lease EXCEPT ![JobActor[j]] = w]
    /\ carrierJob' = [carrierJob EXCEPT ![w] = j]
    /\ phase' = [phase EXCEPT ![j] = "running"]
    /\ UNCHANGED stopped

(* Await releases the PHYSICAL owner but leaves the logical operation live.
   Multiple suspended tasks for one actor are allowed. *)
Await(w, j) ==
    /\ w \in Workers
    /\ j \in Jobs
    /\ phase[j] = "running"
    /\ carrierJob[w] = j
    /\ lease[JobActor[j]] = w
    /\ lease' = [lease EXCEPT ![JobActor[j]] = NoWorker]
    /\ carrierJob' = [carrierJob EXCEPT ![w] = NoJob]
    /\ phase' = [phase EXCEPT ![j] = "suspended"]
    /\ UNCHANGED stopped

Complete(w, j) ==
    /\ w \in Workers
    /\ j \in Jobs
    /\ phase[j] = "running"
    /\ carrierJob[w] = j
    /\ lease[JobActor[j]] = w
    /\ lease' = [lease EXCEPT ![JobActor[j]] = NoWorker]
    /\ carrierJob' = [carrierJob EXCEPT ![w] = NoJob]
    /\ phase' = [phase EXCEPT ![j] = "done"]
    /\ UNCHANGED stopped

(* No direct continuation on the producer thread: wake only makes it ready. *)
Wake(j) ==
    /\ j \in Jobs
    /\ phase[j] = "suspended"
    /\ ~stopped[JobActor[j]]
    /\ phase' = [phase EXCEPT ![j] = "ready"]
    /\ UNCHANGED <<lease, carrierJob, stopped>>

Cancel(j) ==
    /\ j \in Jobs
    /\ phase[j] \in {"ready", "suspended"}
    /\ phase' = [phase EXCEPT ![j] = "done"]
    /\ UNCHANGED <<lease, carrierJob, stopped>>

(* In-flight guest code already on a worker must unwind cooperatively.
   No new claim can occur once stopped. *)
Stop(a) ==
    /\ a \in Actors
    /\ ~stopped[a]
    /\ stopped' = [stopped EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<lease, carrierJob, phase>>

Next ==
    \/ \E w \in Workers : \E j \in Jobs : Claim(w, j)
    \/ \E w \in Workers : \E j \in Jobs : Await(w, j)
    \/ \E w \in Workers : \E j \in Jobs : Complete(w, j)
    \/ \E j \in Jobs : Wake(j)
    \/ \E j \in Jobs : Cancel(j)
    \/ \E a \in Actors : Stop(a)

Spec == Init /\ [][Next]_vars

Executing(a) ==
    {w \in Workers :
       IF carrierJob[w] \in Jobs THEN JobActor[carrierJob[w]] = a ELSE FALSE}

TypeOK ==
    /\ lease \in [Actors -> (Workers \cup {NoWorker})]
    /\ carrierJob \in [Workers -> (Jobs \cup {NoJob})]
    /\ phase \in [Jobs -> {"ready", "running", "suspended", "done"}]
    /\ stopped \in [Actors -> BOOLEAN]

ExclusivePhysicalCarrier == \A a \in Actors : Cardinality(Executing(a)) <= 1

LeaseMatchesCarrier ==
    \A a \in Actors :
        IF lease[a] = NoWorker
        THEN Executing(a) = {}
        ELSE Executing(a) = {lease[a]}

RunningMatchesCarrier ==
    \A j \in Jobs :
        (phase[j] = "running") <=> (\E w \in Workers : carrierJob[w] = j)

(* Desired safety property: an actor may have many suspended logical jobs,
   but only one physical guest executor at a time. *)
Safety == TypeOK /\ ExclusivePhysicalCarrier /\ LeaseMatchesCarrier /\ RunningMatchesCarrier
=======================================================================
