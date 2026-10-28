//! Deterministic reference model of default serialized actor turns.
//! Host-only test oracle, NOT a proof that compiler guest turns are integrated.
//! Run: rustc --edition=2021 --test -D warnings tests/actor_execution_ledger_model.rs -o /tmp/ledger-model && /tmp/ledger-model
#![forbid(unsafe_code)]

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Kind { Mail, Timer, Supervisor }
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Phase { Pending, Ready, Running, Suspended, Dead }
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Id { actor: u64, generation: u64, sequence: u64 }
#[derive(Clone, Copy, Debug)]
struct Event { id: Id, kind: Kind, phase: Phase }
#[derive(Debug)]
struct Model { actor: u64, generation: u64, next: u64, events: Vec<Event>, active: Option<u64>, stopped: bool }
impl Model {
    fn new(actor: u64, generation: u64) -> Self {
        Self { actor, generation, next: 1, events: Vec::new(), active: None, stopped: false }
    }
    fn admit(&mut self, kind: Kind) -> Option<Id> {
        if self.stopped { return None }
        let id = Id { actor: self.actor, generation: self.generation, sequence: self.next };
        self.next += 1;
        self.events.push(Event { id, kind, phase: if kind == Kind::Timer { Phase::Pending } else { Phase::Ready } });
        Some(id)
    }
    fn slot(&self, id: Id) -> Option<usize> {
        if id.actor != self.actor || id.generation != self.generation { return None }
        self.events.iter().position(|e| e.id == id && e.phase != Phase::Dead)
    }
    fn wake(&mut self, id: Id) -> bool {
        if self.stopped { return false }
        let Some(i) = self.slot(id) else { return false };
        if self.events[i].phase != Phase::Pending && self.events[i].phase != Phase::Suspended { return false }
        self.events[i].phase = Phase::Ready;
        true
    }
    fn select(&self) -> Option<Id> {
        if self.active.is_some() { return None }
        let ctrl = self.events.iter().filter(|e| e.kind == Kind::Supervisor && e.phase == Phase::Ready)
            .min_by_key(|e| e.id.sequence);
        if let Some(e) = ctrl { return Some(e.id) }
        let normal = self.events.iter().filter(|e| e.kind != Kind::Supervisor && e.phase != Phase::Dead &&
            !(e.kind == Kind::Timer && e.phase == Phase::Pending))
            .min_by_key(|e| e.id.sequence);
        normal.filter(|e| e.phase == Phase::Ready).map(|e| e.id)
    }
    fn claim(&mut self, id: Id) -> bool {
        if self.select() != Some(id) { return false }
        let Some(i) = self.slot(id) else { return false };
        if self.stopped && self.events[i].kind != Kind::Supervisor { return false }
        self.events[i].phase = Phase::Running;
        self.active = Some(id.sequence);
        true
    }
    fn suspend(&mut self, id: Id) -> bool {
        if self.active != Some(id.sequence) { return false }
        let Some(i) = self.slot(id) else { return false };
        if self.events[i].phase != Phase::Running { return false }
        self.events[i].phase = Phase::Suspended;
        self.active = None;
        true
    }
    fn finish(&mut self, id: Id) -> bool {
        if self.active != Some(id.sequence) { return false }
        let Some(i) = self.slot(id) else { return false };
        if self.events[i].phase != Phase::Running { return false }
        self.events[i].phase = Phase::Dead;
        self.active = None;
        true
    }
    fn cancel(&mut self, id: Id) -> bool {
        let Some(i) = self.slot(id) else { return false };
        if self.events[i].kind == Kind::Supervisor || self.events[i].phase == Phase::Running { return false }
        self.events[i].phase = Phase::Dead;
        true
    }
    fn stop(&mut self) {
        self.stopped = true;
        for e in &mut self.events {
            if e.kind != Kind::Supervisor && e.phase != Phase::Running { e.phase = Phase::Dead }
        }
    }
    fn retired(&self) -> bool { self.stopped && self.events.iter().all(|e| e.phase == Phase::Dead) }
    fn check_invariants(&self) {
        assert!(self.events.iter().filter(|e| e.phase == Phase::Running).count() <= 1);
        assert_eq!(self.active.is_some(), self.events.iter().any(|e| e.phase == Phase::Running));
        for e in &self.events {
            assert_eq!(e.id.actor, self.actor);
            assert_eq!(e.id.generation, self.generation);
            if self.stopped { assert!(e.kind == Kind::Supervisor || e.phase == Phase::Dead || e.phase == Phase::Running); }
        }
        if let Some(oldest) = self.events.iter().filter(|e| e.phase == Phase::Suspended)
            .min_by_key(|e| e.id.sequence) {
            if let Some(pick) = self.select() {
                assert!(pick.sequence <= oldest.id.sequence || self.events.iter()
                    .any(|e| e.id == pick && e.kind == Kind::Supervisor));
            }
        }
    }
}

#[test]
fn default_resume_precedes_later_mail_and_timer() {
    let mut m = Model::new(42, 7);
    let a = m.admit(Kind::Mail).unwrap();
    assert!(m.claim(a));
    assert!(m.suspend(a));
    let b = m.admit(Kind::Mail).unwrap();
    let timer = m.admit(Kind::Timer).unwrap();
    assert!(m.wake(timer));
    assert!(m.select().is_none());
    let control = m.admit(Kind::Supervisor).unwrap();
    assert_eq!(m.select(), Some(control));
    assert!(m.claim(control) && m.finish(control));
    assert!(m.wake(a));
    assert_eq!(m.select(), Some(a));
    assert!(m.claim(a) && m.finish(a));
    assert_eq!(m.select(), Some(b));
    assert!(m.claim(b) && m.finish(b));
    assert_eq!(m.select(), Some(timer));
    assert!(m.claim(timer) && m.finish(timer));
    m.check_invariants();
}

fn exercise(sequence: &[u8]) {
    let mut m = Model::new(777, 19);
    let a = m.admit(Kind::Mail).unwrap();
    assert!(m.claim(a) && m.suspend(a));
    let timer = m.admit(Kind::Timer).unwrap();
    let b = m.admit(Kind::Mail).unwrap();
    let supervisor = m.admit(Kind::Supervisor).unwrap();
    let mut finished = Vec::new();
    for action in sequence {
        match *action {
            0 => { let _ = m.wake(a); }
            1 => { let _ = m.wake(timer); }
            2 => { let _ = m.cancel(timer); }
            3 => m.stop(),
            4 => {
                if let Some(id) = m.select() {
                    if m.claim(id) {
                        assert!(!finished.contains(&id));
                        finished.push(id);
                        assert!(m.finish(id));
                    }
                }
            }
            5 => { let _ = m.wake(a); } // duplicate notice
            _ => unreachable!(),
        }
        m.check_invariants();
        // An older unresolved message bars a later ordinary message.
        if m.slot(a).is_some() && m.events.iter().any(|e| e.id == a && e.phase == Phase::Suspended) {
            assert_ne!(m.select(), Some(b));
        }
        // No stale-generation notice can activate another incarnation.
        let mut old = a;
        old.generation += 1;
        assert!(!m.wake(old));
    }
    if m.stopped {
        assert!(!m.admit(Kind::Mail).is_some());
        assert!(m.slot(supervisor).is_none() || m.select() == Some(supervisor) || m.active.is_some());
    }
}
fn permutations(items: &mut [u8], offset: usize, count: &mut usize) {
    if offset == items.len() { exercise(items); *count += 1; return }
    for i in offset..items.len() {
        items.swap(offset, i);
        permutations(items, offset + 1, count);
        items.swap(offset, i);
    }
}
#[test]
fn all_720_wake_cancel_stop_dispatch_interleavings() {
    let mut steps = [0, 1, 2, 3, 4, 5];
    let mut count = 0;
    permutations(&mut steps, 0, &mut count);
    assert_eq!(count, 720);
}
#[test]
fn stopped_actor_waits_for_supervisor_control_physical_retirement() {
    let mut m = Model::new(33, 1);
    let data = m.admit(Kind::Mail).unwrap();
    assert!(m.claim(data));
    let fail = m.admit(Kind::Supervisor).unwrap();
    m.stop();
    assert!(!m.retired());
    assert!(!m.cancel(fail));
    assert!(m.finish(data));
    assert!(m.claim(fail));
    assert!(!m.retired());
    assert!(m.finish(fail));
    assert!(m.retired());
}
