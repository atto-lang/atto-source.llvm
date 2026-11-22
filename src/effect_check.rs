//! Closed-world, typed-IR effect inference for the current native AOT subset.
//! This is an opt-in static admission gate, NOT hard-RT/WCET certification.
//! Any unmodeled typed instruction fails closed rather than being called pure.
use std::collections::{BTreeSet, VecDeque};
use atto_compiler::ast::BinOp;
use atto_compiler::check::{TExpr, TProgram, TStmt, TE};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile { Pure, Deterministic, NoAlloc }

impl Profile {
    pub fn from_flag(flag: &str) -> Option<Self> {
        match flag {
            "pure" => Some(Self::Pure),
            "deterministic" => Some(Self::Deterministic),
            "no-alloc" => Some(Self::NoAlloc),
            _ => None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Summary {
    pub effects: BTreeSet<&'static str>,
    pub calls: BTreeSet<usize>,
}

#[derive(Default)]
struct Walker {
    effects: BTreeSet<&'static str>,
    calls: BTreeSet<usize>,
    nodes: usize,
}

impl Walker {
    fn tick(&mut self, depth: usize) -> Result<(), String> {
        if depth > 64 || self.nodes >= 100_000 {
            return Err("E_EFFECT_UNKNOWN: typed IR traversal budget exceeded".into());
        }
        self.nodes += 1;
        Ok(())
    }

    fn expr(&mut self, expr: &TE, depth: usize) -> Result<(), String> {
        self.tick(depth)?;
        match &expr.e {
            TExpr::I64(_) | TExpr::F64(_) | TExpr::Bool(_) |
            TExpr::Local(_) | TExpr::EnumVal(_) => {}
            // String constants are data-segment values, not a runtime allocation.
            TExpr::StrConst(_, _) => {}
            TExpr::Neg(arg) | TExpr::FNeg(arg) => {
                // The signed native integer negation lowers to an overflow trap.
                if matches!(&expr.e, TExpr::Neg(_)) {
                    self.effects.insert("panic");
                }
                self.expr(arg, depth + 1)?;
            }
            TExpr::Not(arg) | TExpr::IntToFloat(arg) => self.expr(arg, depth + 1)?,
            TExpr::Bin(op, lhs, rhs) => {
                if matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div) {
                    self.effects.insert("panic");
                }
                self.expr(lhs, depth + 1)?;
                self.expr(rhs, depth + 1)?;
            }
            TExpr::Ternary(test, yes, no) => {
                self.expr(test, depth + 1)?;
                self.expr(yes, depth + 1)?;
                self.expr(no, depth + 1)?;
            }
            TExpr::Call(target, args) => {
                self.calls.insert(*target as usize);
                for arg in args {
                    self.expr(arg, depth + 1)?;
                }
            }
            TExpr::Alloc(_) => { self.effects.insert("alloc"); }
            TExpr::Spawn(_) => {
                self.effects.extend(["alloc", "spawns"]);
            }
            TExpr::Send { target, value } => {
                self.effects.insert("actor.send");
                self.expr(target, depth + 1)?;
                self.expr(value, depth + 1)?;
            }
            TExpr::GlobalGet(_) => { self.effects.insert("state.read"); }
            TExpr::Print { value, .. } => {
                self.effects.insert("io");
                self.expr(value, depth + 1)?;
            }
            TExpr::PrintStr { .. } => { self.effects.insert("io"); }
            TExpr::PrintStrVal { value, .. } => {
                self.effects.insert("io");
                self.expr(value, depth + 1)?;
            }
            TExpr::PrintObj { obj, .. } => {
                self.effects.insert("io");
                self.expr(obj, depth + 1)?;
            }
            // The native compiler cannot currently prove these object,
            // iterator, FFI, string helper or exceptional cleanup effects.
            _ => return Err(format!(
                "E_EFFECT_UNKNOWN: unclassified typed expression {:?}", expr.e
            )),
        }
        Ok(())
    }

    fn statements(&mut self, statements: &[TStmt], depth: usize) -> Result<(), String> {
        self.tick(depth)?;
        for statement in statements {
            self.tick(depth)?;
            match statement {
                TStmt::Set(_, value) | TStmt::Expr(value) => self.expr(value, depth + 1)?,
                TStmt::Return(Some(value)) => self.expr(value, depth + 1)?,
                TStmt::Return(None) | TStmt::Break | TStmt::Continue |
                TStmt::Unreachable => {}
                TStmt::Throw(value) => {
                    self.effects.insert("throw");
                    self.expr(value, depth + 1)?;
                }
                TStmt::GlobalSet(_, value) => {
                    self.effects.insert("state.write");
                    self.expr(value, depth + 1)?;
                }
                TStmt::FieldSet { obj, value, .. } => {
                    // Proven-local/escaped field provenance is not in this
                    // current typed IR. Do not silently model a safe write.
                    self.expr(obj, depth + 1)?;
                    self.expr(value, depth + 1)?;
                    return Err("E_EFFECT_UNKNOWN: field write provenance unverified".into());
                }
                TStmt::If(arms, otherwise) => {
                    for (cond, body) in arms {
                        self.expr(cond, depth + 1)?;
                        self.statements(body, depth + 1)?;
                    }
                    if let Some(body) = otherwise {
                        self.statements(body, depth + 1)?;
                    }
                }
                TStmt::Loop { cond, body, update } => {
                    self.effects.insert("unbounded_work");
                    if let Some(cond) = cond {
                        self.expr(cond, depth + 1)?;
                    }
                    self.statements(body, depth + 1)?;
                    self.statements(update, depth + 1)?;
                }
                TStmt::Block(body) => self.statements(body, depth + 1)?,
                TStmt::ForIn { .. } => return Err(
                    "E_EFFECT_UNKNOWN: iterator lifecycle and drop unverified".into()),
            }
        }
        Ok(())
    }
}

/// Infer effect rows from compiler-checked typed IR, not from source keywords.
/// The current slice is deliberately closed-world; imported and latent
/// generator/runtime/actor-lifecycle contracts require future verified models.
pub fn infer(program: &TProgram) -> Result<Vec<Summary>, String> {
    if !program.imports.is_empty() || !program.generators.is_empty()
        || !program.actors.is_empty() || program.heap || program.strings
        || program.guest_eh || !program.data.is_empty() {
        return Err("E_EFFECT_UNKNOWN: imported or asynchronous runtime closure".into());
    }
    if program.funcs.is_empty() || program.funcs.len() > 4096 {
        return Err("E_EFFECT_UNKNOWN: invalid function budget".into());
    }
    let mut rows = Vec::with_capacity(program.funcs.len());
    let mut edge_count = 0usize;
    for function in &program.funcs {
        let mut walker = Walker::default();
        walker.statements(&function.body, 0)?;
        if walker.calls.iter().any(|i| *i >= program.funcs.len()) {
            return Err("E_EFFECT_UNKNOWN: unresolved typed call target".into());
        }
        edge_count = edge_count.saturating_add(walker.calls.len());
        if edge_count > 65_536 {
            return Err("E_EFFECT_UNKNOWN: call graph budget exceeded".into());
        }
        rows.push(Summary { effects: walker.effects, calls: walker.calls });
    }
    // Iterate over the finite union lattice. Handles mutual recursion without
    // certifying recursive termination or bounded stack consumption.
    let mut closure: Vec<BTreeSet<&'static str>> =
        rows.iter().map(|r| r.effects.clone()).collect();
    let mut parents = vec![Vec::<usize>::new(); rows.len()];
    for (caller, row) in rows.iter().enumerate() {
        for callee in &row.calls { parents[*callee].push(caller); }
    }
    let mut queue: VecDeque<usize> = (0..rows.len()).collect();
    let mut queued = vec![true; rows.len()];
    while let Some(caller) = queue.pop_front() {
        queued[caller] = false;
        let mut merged = rows[caller].effects.clone();
        for child in &rows[caller].calls {
            merged.extend(closure[*child].iter().copied());
        }
        if merged != closure[caller] {
            closure[caller] = merged;
            for parent in &parents[caller] {
                if !queued[*parent] {
                    queue.push_back(*parent);
                    queued[*parent] = true;
                }
            }
        }
    }
    for (row, effects) in rows.iter_mut().zip(closure) {
        row.effects = effects;
    }
    Ok(rows)
}

/// Explicit profile admission only; default native compilation is unchanged.
/// A positive result cannot be used as a hard-RT or sandbox certificate.
pub fn admit(program: &TProgram, profile: Profile) -> Result<(), String> {
    let rows = infer(program)?;
    let entry = program.main as usize;
    if entry >= rows.len() {
        return Err("E_EFFECT_UNKNOWN: missing typed main entry".into());
    }
    let row = &rows[entry].effects;
    if profile == Profile::NoAlloc && row.contains("alloc") {
        return Err("E_HARD_RT_ALLOCATION: transitive runtime allocation".into());
    }
    if profile == Profile::Pure && row.iter().any(|x| matches!(*x,
        "io" | "state.read" | "state.write" | "actor.send" | "spawns" | "nondet" | "ffi")) {
        return Err("E_PURITY_EFFECT: transitive external observation or mutation".into());
    }
    if profile == Profile::Deterministic && row.iter().any(|x| matches!(*x,
        "io" | "state.read" | "state.write" | "actor.send" | "spawns" | "nondet" | "ffi")) {
        return Err("E_NONDETERMINISTIC: ambient or externally mutable effect".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use atto_compiler::ast::Type;
    use atto_compiler::check::{TFunc, TProgram};

    fn e(expr: TExpr) -> TE { TE { e: expr, ty: Type::Int } }
    fn program(bodies: Vec<Vec<TStmt>>) -> TProgram {
        TProgram {
            funcs: bodies.into_iter().enumerate().map(|(i, body)| TFunc {
                name: format!("f{i}"), params: vec![], ret: Type::Int,
                locals: vec![], body
            }).collect(),
            generators: vec![], main: 0, imports: vec![],
            data: vec![], heap: false, globals: vec![],
            actors: vec![], strings: false, guest_eh: false
        }
    }

    #[test]
    fn closed_scalar_is_pure_and_no_alloc() {
        let p = program(vec![vec![TStmt::Return(Some(e(TExpr::I64(42))))]]);
        for mode in [Profile::Pure, Profile::Deterministic, Profile::NoAlloc] {
            assert!(admit(&p, mode).is_ok());
        }
    }

    #[test]
    fn indirect_alloc_propagates_through_call_graph() {
        let p = program(vec![
            vec![TStmt::Return(Some(e(TExpr::Call(1, vec![]))))],
            vec![TStmt::Expr(e(TExpr::Alloc(8))), TStmt::Return(Some(e(TExpr::I64(42))))],
        ]);
        assert!(infer(&p).unwrap()[0].effects.contains("alloc"));
        assert!(admit(&p, Profile::NoAlloc).unwrap_err().contains("E_HARD_RT_ALLOCATION"));
        assert!(admit(&p, Profile::Pure).is_ok()); // private allocation is compatible with purity
    }

    #[test]
    fn protected_state_and_output_are_not_pure_or_deterministic() {
        let state = program(vec![vec![TStmt::Return(Some(e(TExpr::GlobalGet(0))))]]);
        let out = program(vec![vec![TStmt::Expr(e(TExpr::Print {
            value: Box::new(e(TExpr::I64(1))), newline: true
        })), TStmt::Return(Some(e(TExpr::I64(42))))]]);
        assert!(admit(&state, Profile::Pure).unwrap_err().contains("E_PURITY_EFFECT"));
        assert!(admit(&out, Profile::Deterministic).unwrap_err().contains("E_NONDETERMINISTIC"));
    }

    #[test]
    fn incomplete_typed_calls_never_become_effect_free() {
        let p = program(vec![vec![TStmt::Return(Some(e(TExpr::Call(99, vec![]))))]]);
        assert!(admit(&p, Profile::Pure).unwrap_err().contains("E_EFFECT_UNKNOWN"));
    }

    #[test]
    fn recursion_fixed_point_is_not_a_termination_proof() {
        let p = program(vec![
            vec![TStmt::Return(Some(e(TExpr::Call(1, vec![]))))],
            vec![TStmt::Expr(e(TExpr::Alloc(1))),
                 TStmt::Return(Some(e(TExpr::Call(0, vec![]))))],
        ]);
        assert!(infer(&p).unwrap()[0].effects.contains("alloc"));
        assert!(admit(&p, Profile::NoAlloc).is_err());
    }

    #[test]
    fn checked_division_is_not_reported_as_effect_free() {
        // The native emitter currently rejects division; if a backend
        // supports it later, its exceptional path must already be explicit.
        let lhs = e(TExpr::I64(42));
        let rhs = e(TExpr::I64(0));
        let p = program(vec![vec![TStmt::Return(Some(e(TExpr::Bin(
            BinOp::Div, Box::new(lhs), Box::new(rhs)
        ))))]]);
        let rows = infer(&p).expect("the checked typed graph is closed");
        assert!(rows[0].effects.contains("panic"));
    }

    #[test]
    fn unverified_runtime_lifecycle_fails_closed() {
        let mut p = program(vec![vec![TStmt::Return(Some(e(TExpr::I64(1))))]]);
        p.heap = true; // hidden constructors/allocator initialization unmodeled
        assert!(admit(&p, Profile::NoAlloc).unwrap_err().contains("E_EFFECT_UNKNOWN"));
        p.heap = false;
        p.guest_eh = true; // hidden throw/unwind/drop closure unmodeled
        assert!(admit(&p, Profile::Pure).unwrap_err().contains("E_EFFECT_UNKNOWN"));
    }
}
