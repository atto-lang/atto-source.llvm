-------------------- MODULE ActorDispatchIdentity --------------------
EXTENDS Naturals, FiniteSets

(* Abstract physical scheduling tokens; only the exact currently issued
   token may start or release actor execution. Tickets are never reused. *)
CONSTANTS Tickets, Workers, NoTicket, NoWorker
ASSUME /\ IsFiniteSet(Tickets) /\ Tickets # {}
       /\ IsFiniteSet(Workers) /\ Workers # {}
       /\ NoTicket \notin Tickets
       /\ NoWorker \notin Workers

VARIABLES current, physical, runningTicket, issued, stopped
vars == <<current, physical, runningTicket, issued, stopped>>

Init ==
    /\ current = NoTicket
    /\ physical = NoWorker
    /\ runningTicket = NoTicket
    /\ issued = {}
    /\ stopped = FALSE

Issue(t) ==
    /\ ~stopped
    /\ current = NoTicket
    /\ t \in Tickets \ issued
    /\ current' = t
    /\ issued' = issued \cup {t}
    /\ UNCHANGED <<physical, runningTicket, stopped>>

Enter(t, w) ==
    /\ ~stopped
    /\ t = current
    /\ t \in issued
    /\ physical = NoWorker
    /\ w \in Workers
    /\ physical' = w
    /\ runningTicket' = t
    /\ UNCHANGED <<current, issued, stopped>>

Leave(t, w) ==
    /\ t = current
    /\ runningTicket = t
    /\ physical = w
    /\ w \in Workers
    /\ current' = NoTicket
    /\ physical' = NoWorker
    /\ runningTicket' = NoTicket
    /\ UNCHANGED <<issued, stopped>>

CancelQueued ==
    /\ current # NoTicket
    /\ physical = NoWorker
    /\ current' = NoTicket
    /\ UNCHANGED <<physical, runningTicket, issued, stopped>>

(* Stale queued dispatches and completions are permitted to replay
   arbitrarily; they have exactly zero authority over a newer ticket. *)
ReplayObsolete(t) ==
    /\ t \in issued
    /\ t # current
    /\ UNCHANGED vars

Stop ==
    /\ ~stopped
    /\ stopped' = TRUE
    /\ UNCHANGED <<current, physical, runningTicket, issued>>

Next ==
    \/ \E t \in Tickets : Issue(t)
    \/ \E t \in Tickets : \E w \in Workers : Enter(t, w)
    \/ \E t \in Tickets : \E w \in Workers : Leave(t, w)
    \/ CancelQueued
    \/ \E t \in Tickets : ReplayObsolete(t)
    \/ Stop

Spec == Init /\ [][Next]_vars
TypeOK ==
    /\ current \in Tickets \cup {NoTicket}
    /\ physical \in Workers \cup {NoWorker}
    /\ runningTicket \in Tickets \cup {NoTicket}
    /\ issued \subseteq Tickets
    /\ stopped \in BOOLEAN

NoStalePhysicalRelease ==
    /\ (physical = NoWorker) <=> (runningTicket = NoTicket)
    /\ (physical # NoWorker) => (current = runningTicket)
    /\ (current # NoTicket) => (current \in issued)

TicketNonreuse == issued \subseteq Tickets
=======================================================================
