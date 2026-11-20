//! Finite carrier lease model. This checks a source-free abstraction, not native runtime execution.
use std::collections::{HashMap, VecDeque};

const FREE: i8 = -1;
const READY: i8 = 0;
const RUNNING: i8 = 1;
const SUSPENDED: i8 = 2;
const DONE: i8 = 3;
const JOB_ACTOR: [usize; 3] = [0, 0, 1];

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
struct State {
    lease: [i8; 2],
    carriers: [i8; 2],
    jobs: [i8; 3],
    stopped: [bool; 2],
}

impl State {
    const INITIAL: Self = Self {
        lease: [FREE, FREE],
        carriers: [FREE, FREE],
        jobs: [READY, READY, READY],
        stopped: [false, false],
    };

    fn check(self) -> Result<(), &'static str> {
        let mut executing: [Vec<usize>; 2] = [Vec::new(), Vec::new()];
        for (worker, job) in self.carriers.into_iter().enumerate() {
            if job != FREE {
                let actor = *JOB_ACTOR.get(job as usize).ok_or("unknown carrier job")?;
                executing[actor].push(worker);
            }
        }
        if executing.iter().any(|workers| workers.len() > 1) {
            return Err("two physical carriers executing same actor");
        }
        if executing.iter().enumerate().any(|(actor, workers)| {
            (self.lease[actor] == FREE) != workers.is_empty()
        }) {
            return Err("lease disagrees with physical executor");
        }
        if executing.iter().enumerate().any(|(actor, workers)| {
            !workers.is_empty() && self.lease[actor] != workers[0] as i8
        }) {
            return Err("execution owner disagrees with lease");
        }
        if self.jobs.iter().enumerate().any(|(job, state)| {
            (*state == RUNNING) != self.carriers.contains(&(job as i8))
        }) {
            return Err("task run state disagrees with carrier occupancy");
        }
        Ok(())
    }

    fn successors(self, unsafe_double_claim: bool) -> Vec<(String, State)> {
        let mut out = Vec::new();
        for (job, state) in self.jobs.into_iter().enumerate() {
            let actor = JOB_ACTOR[job];
            if state == READY && !self.stopped[actor] {
                for (worker, occupied) in self.carriers.into_iter().enumerate() {
                    if occupied == FREE && (self.lease[actor] == FREE || unsafe_double_claim) {
                        let mut next = self;
                        next.lease[actor] = worker as i8;
                        next.carriers[worker] = job as i8;
                        next.jobs[job] = RUNNING;
                        out.push((
                            format!("claim actor={actor} job={job} worker={worker}"), next
                        ));
                    }
                }
            }
            if state == RUNNING {
                for (worker, current) in self.carriers.into_iter().enumerate() {
                    if current == job as i8 {
                        for (outcome, new_state) in [
                            ("await", SUSPENDED),
                            ("complete", DONE),
                        ] {
                            let mut next = self;
                            next.lease[actor] = FREE;
                            next.carriers[worker] = FREE;
                            next.jobs[job] = new_state;
                            out.push((
                                format!("{outcome} actor={actor} job={job} worker={worker}"), next
                            ));
                        }
                    }
                }
            }
            if state == SUSPENDED && !self.stopped[actor] {
                let mut next = self;
                next.jobs[job] = READY;
                out.push((format!("wake actor={actor} job={job}"), next));
            }
            if state == READY || state == SUSPENDED {
                let mut next = self;
                next.jobs[job] = DONE;
                out.push((format!("cancel actor={actor} job={job}"), next));
            }
        }
        for actor in 0..2 {
            if !self.stopped[actor] {
                let mut next = self;
                next.stopped[actor] = true;
                out.push((format!("stop actor={actor}"), next));
            }
        }
        out
    }
}

#[derive(Debug)]
struct Outcome {
    states: usize,
    violation: Option<&'static str>,
    trace: Vec<String>,
    reentrant_witness: Option<Vec<String>>,
}

fn trace(mut state: State, parents: &HashMap<State, Option<(State, String)>>) -> Vec<String> {
    let mut actions = Vec::new();
    while let Some(Some((parent, action))) = parents.get(&state) {
        actions.push(action.clone());
        state = *parent;
    }
    actions.reverse();
    actions
}

fn explore(mutant: bool) -> Outcome {
    let mut queue = VecDeque::from([State::INITIAL]);
    let mut parents = HashMap::from([(State::INITIAL, None)]);
    let mut witness = None;
    while let Some(state) = queue.pop_front() {
        if let Err(reason) = state.check() {
            return Outcome {
                states: parents.len(),
                violation: Some(reason),
                trace: trace(state, &parents),
                reentrant_witness: witness,
            };
        }
        if witness.is_none() && state.jobs[0] == SUSPENDED && state.jobs[1] == RUNNING {
            witness = Some(trace(state, &parents));
        }
        for (name, next) in state.successors(mutant) {
            if let std::collections::hash_map::Entry::Vacant(entry) = parents.entry(next) {
                entry.insert(Some((state, name)));
                queue.push_back(next);
            }
        }
    }
    Outcome { states: parents.len(), violation: None, trace: vec![], reentrant_witness: witness }
}

fn main() {
    if std::env::args_os().len() != 1 {
        eprintln!("usage: actor-carrier-exclusivity"); std::process::exit(2);
    }
    let safe = explore(false);
    if safe.violation.is_some() || safe.reentrant_witness.is_none() || safe.states != 372 {
        eprintln!("carrier model DENIED: safe model unexpected result: {safe:?}");
        std::process::exit(1);
    }
    let mutant = explore(true);
    if mutant.states != 65
        || mutant.violation != Some("two physical carriers executing same actor")
        || mutant.trace.last().is_none_or(|step| !step.starts_with("claim actor=0 job=1"))
    {
        eprintln!("carrier model DENIED: unsafe mutant escaped: {mutant:?}");
        std::process::exit(1);
    }
    println!("PASS: {} reachable states, all satisfy exclusive physical execution", safe.states);
    println!("Reentrant witness: {}", safe.reentrant_witness.unwrap().join(" -> "));
    println!("MUTATION DETECTED after {} states: {}", mutant.states, mutant.violation.unwrap());
    println!("Counterexample: {}", mutant.trace.join(" -> "));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_model_preserves_exclusive_carrier_and_reentrant_progress() {
        let run = explore(false);
        assert_eq!(run.violation, None);
        assert_eq!(run.states, 372);
        assert!(run.reentrant_witness.unwrap().iter().any(|step| step.starts_with("await actor=0")));
    }

    #[test]
    fn two_carriers_mutant_detected_with_shortest_witness() {
        let run = explore(true);
        assert_eq!(run.states, 65);
        assert_eq!(run.violation, Some("two physical carriers executing same actor"));
        assert_eq!(run.trace.len(), 2);
    }

    #[test]
    fn corrupt_lease_or_executor_fails_closed() {
        let corrupt = State {
            lease: [0, FREE], carriers: [FREE, FREE],
            jobs: [READY, READY, READY], stopped: [false, false],
        };
        assert_eq!(corrupt.check(), Err("lease disagrees with physical executor"));
        let corrupt = State {
            lease: [FREE, FREE], carriers: [0, FREE],
            jobs: [RUNNING, READY, READY], stopped: [false, false],
        };
        assert_eq!(corrupt.check(), Err("lease disagrees with physical executor"));
    }
}
