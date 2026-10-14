; Oreslang borrowing: single-root unbounded shared-loan inductiveness.
; These SMT queries establish preservation under the declared guarded operations.
; They do NOT prove that a compiler implementation faithfully enforces each guard.
; Assumptions: one abstract owner root, sound alias canonicalization,
; no hidden loans, sequential abstract checker steps.
(set-logic QF_LIA)
(declare-const s Int)
(declare-const e Bool)
(declare-const moved Bool)
(declare-const suspended Bool)
(define-fun Safe ((sc Int) (ex Bool) (mv Bool) (sus Bool)) Bool
  (and (>= sc 0)
       (=> ex (= sc 0))
       (=> mv (and (= sc 0) (not ex)))
       (=> sus (and (= sc 0) (not ex)))))
(assert (Safe s e moved suspended))

; (1) acquire shared loan
(push 1)
(assert (and (not moved) (not suspended) (not e)))
(assert (not (Safe (+ s 1) e moved suspended)))
(check-sat)
(pop 1)

; (2) acquire exclusive loan
(push 1)
(assert (and (= s 0) (not e) (not moved) (not suspended)))
(assert (not (Safe s true moved suspended)))
(check-sat)
(pop 1)

; (3) release shared loan
(push 1)
(assert (> s 0))
(assert (not (Safe (- s 1) e moved suspended)))
(check-sat)
(pop 1)

; (4) release exclusive loan
(push 1)
(assert e)
(assert (not (Safe s false moved suspended)))
(check-sat)
(pop 1)

; (5) move an unborrowed live owner
(push 1)
(assert (and (= s 0) (not e) (not moved) (not suspended)))
(assert (not (Safe s e true suspended)))
(check-sat)
(pop 1)

; (6) reinitialize a moved owner
(push 1)
(assert (and moved (not suspended)))
(assert (not (Safe s e false suspended)))
(check-sat)
(pop 1)

; (7) suspend only with zero ordinary loans
(push 1)
(assert (and (= s 0) (not e) (not suspended)))
(assert (not (Safe s e moved true)))
(check-sat)
(pop 1)

; (8) resume a suspended owner
(push 1)
(assert suspended)
(assert (not (Safe s e moved false)))
(check-sat)
(pop 1)

; Mutants must have satisfying unsafe witnesses: no guard on exclusive borrow.
(push 1)
(assert (> s 0))
(assert (not e))
(assert (not (Safe s true moved suspended)))
(check-sat)
(pop 1)

; Mutant: move while there is a live shared loan.
(push 1)
(assert (> s 0))
(assert (not (Safe s e true suspended)))
(check-sat)
(pop 1)

; Mutant: suspend with ordinary shared loans still active.
(push 1)
(assert (> s 0))
(assert (not (Safe s e moved true)))
(check-sat)
(pop 1)
