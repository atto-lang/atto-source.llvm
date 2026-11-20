//! Source-free bounded dispatch ticket safety model (NOT executable Oreslang proof).
//! A stale completion/cancellation must never release the current actor turn.
use std::collections::{HashMap, VecDeque};
const FREE: i8 = -1;
const TICKETS: std::ops::Range<i8> = 0..3;
const WORKERS: std::ops::Range<i8> = 0..2;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
struct State {
    ticket: i8,
    worker: i8,
    running: i8,
    issued: u8,
    stopped: bool,
}
impl State {
    const INITIAL: Self = Self {
        ticket: FREE, worker: FREE, running: FREE, issued: 0, stopped: false,
    };

    fn violation(self) -> Option<&'static str> {
        if (self.worker == FREE) != (self.running == FREE) {
            return Some("physical worker and running-ticket disagree");
        }
        if self.worker != FREE && (self.ticket == FREE || self.running != self.ticket) {
            return Some("obsolete ticket released an active newer dispatch");
        }
        if self.ticket != FREE && (self.issued & (1u8 << self.ticket as u8)) == 0 {
            return Some("unissued ticket became active");
        }
        None
    }

    fn transitions(self, mutant: bool) -> Vec<(String, State)> {
        let mut out = Vec::new();
        if !self.stopped && self.ticket == FREE {
            for ticket in TICKETS {
                let bit = 1u8 << ticket as u8;
                if self.issued & bit == 0 {
                    out.push((format!("issue({ticket})"), State {
                        ticket, worker: FREE, running: FREE,
                        issued: self.issued | bit, stopped: self.stopped,
                    }));
                }
            }
        }
        if self.ticket != FREE && self.worker == FREE && !self.stopped {
            for worker in WORKERS {
                out.push((format!("enter({},{worker})", self.ticket), State {
                    worker, running: self.ticket, ..self
                }));
            }
        }
        if self.worker != FREE && self.running == self.ticket {
            out.push((format!("exit({})", self.ticket), State {
                ticket: FREE, worker: FREE, running: FREE, ..self
            }));
        }
        if self.ticket != FREE && self.worker == FREE {
            out.push((format!("cancel_queued({})", self.ticket), State {
                ticket: FREE, ..self
            }));
        }
        if !self.stopped {
            out.push(("stop".to_string(), State { stopped: true, ..self }));
        }
        for ticket in TICKETS {
            if self.issued & (1u8 << ticket as u8) != 0 && ticket != self.ticket {
                if mutant && self.ticket != FREE {
                    out.push((format!("UNSAFE_stale_release({ticket})"), State {
                        ticket: FREE, ..self
                    }));
                } else {
                    out.push((format!("ignore_stale({ticket})"), self));
                }
            }
        }
        out
    }
}

#[derive(Debug)]
struct Outcome {
    states: usize,
    error: Option<&'static str>,
    trace: Vec<String>,
}

fn explore(mutant: bool) -> Outcome {
    let mut pending = VecDeque::from([State::INITIAL]);
    let mut parents: HashMap<State, Option<(State, String)>> = HashMap::new();
    parents.insert(State::INITIAL, None);
    while let Some(state) = pending.pop_front() {
        if let Some(error) = state.violation() {
            let mut path = Vec::new();
            let mut cursor = state;
            while let Some(Some((parent, label))) = parents.get(&cursor) {
                path.push(label.clone());
                cursor = *parent;
            }
            path.reverse();
            return Outcome { states: parents.len(), error: Some(error), trace: path };
        }
        for (label, next) in state.transitions(mutant) {
            if let std::collections::hash_map::Entry::Vacant(entry) = parents.entry(next) {
                entry.insert(Some((state, label)));
                pending.push_back(next);
            }
        }
    }
    Outcome { states: parents.len(), error: None, trace: Vec::new() }
}

fn main() {
    if std::env::args_os().len() != 1 {
        eprintln!("usage: oreslang-dispatch-ticket-model");
        std::process::exit(2);
    }
    let safe = explore(false);
    if safe.error.is_some() || safe.states < 10 {
        eprintln!("dispatch ticket model DENIED: safe model violated or underexplored: {safe:?}");
        std::process::exit(1);
    }
    let bad = explore(true);
    if bad.error.is_none() || !bad.trace.last().is_some_and(|s| s.starts_with("UNSAFE_stale_release")) {
        eprintln!("dispatch ticket model DENIED: unsafe mutation was not detected: {bad:?}");
        std::process::exit(1);
    }
    println!("PASS: {} dispatch-ticket states, no stale lease release", safe.states);
    println!("MUTATION DETECTED after {} states: {}", bad.states, bad.trace.join(" -> "));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_model_has_no_violation() {
        let actual = explore(false);
        assert_eq!(actual.states, 88);
        assert_eq!(actual.error, None);
    }

    #[test]
    fn negative_mutant_has_counterexample_with_stale_release() {
        let mutated = explore(true);
        assert_eq!(mutated.states, 84);
        assert!(mutated.error.is_some());
        assert!(mutated.trace.last().unwrap().starts_with("UNSAFE_stale_release"));
    }

    #[test]
    fn direct_worker_identity_violation_rejected() {
        let bad = State { ticket: 1, worker: 0, running: 0, issued: 3, stopped: false };
        assert_eq!(bad.violation(), Some("obsolete ticket released an active newer dispatch"));
        let missing = State { ticket: 2, worker: FREE, running: FREE, issued: 0, stopped: false };
        assert_eq!(missing.violation(), Some("unissued ticket became active"));
    }

    #[test]
    fn stale_callbacks_are_no_ops_in_safe_model() {
        let state = State { ticket: 1, worker: 0, running: 1, issued: 3, stopped: false };
        let transitions = state.transitions(false);
        assert!(transitions.iter().any(|(label, s)| label == "ignore_stale(0)" && *s == state));
        assert!(!transitions.iter().any(|(label, _)| label.starts_with("UNSAFE")));
    }
}
