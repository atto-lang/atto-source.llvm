//! AOT lowering for a closed, scalar Atto subset: checked typed frontend -> LLVM IR.
//! Unsupported guest constructs are explicit errors, never silently stubbed.
use std::fmt::Write as _;
use std::io::Write as IoWrite;
use std::process::{Command, Stdio};

use atto_compiler::ast::{BinOp, Type};
use atto_compiler::check::{self, TExpr, TFunc, TProgram, TStmt, TE};

fn scalar_type(t: Type) -> Result<&'static str, String> {
    match t {
        Type::Int => Ok("i64"),
        Type::Bool => Ok("i1"),
        Type::Void => Ok("void"),
        _ => Err(format!("E_UNSUPPORTED_NATIVE_TYPE: {}", t.name())),
    }
}

struct FunctionEmitter<'a> {
    program: &'a TProgram,
    func: &'a TFunc,
    out: String,
    counter: u32,
    terminated: bool,
    block_name: String,
    slots: Vec<Type>,
}

impl<'a> FunctionEmitter<'a> {
    fn new(program: &'a TProgram, func: &'a TFunc) -> Result<Self, String> {
        let mut slots = func.params.clone();
        slots.extend(func.locals.iter().copied());
        for t in &slots {
            if scalar_type(*t)? == "void" {
                return Err("E_UNSUPPORTED_NATIVE_TYPE: void local".into());
            }
        }
        scalar_type(func.ret)?;
        Ok(Self {
            program, func, out: String::new(), counter: 0,
            terminated: false, block_name: String::new(), slots,
        })
    }

    fn id(&mut self, prefix: &str) -> String {
        self.counter += 1;
        format!("{prefix}{}", self.counter)
    }

    fn instr(&mut self, instruction: impl AsRef<str>) {
        let _ = writeln!(self.out, "  {}", instruction.as_ref());
    }

    fn label(&mut self, name: &str) {
        let _ = writeln!(self.out, "{name}:");
        self.block_name = name.to_string();
        self.terminated = false;
    }

    fn branch(&mut self, name: &str) {
        self.instr(format!("br label %{name}"));
        self.terminated = true;
    }

    fn checked_math(&mut self, opcode: &str, left: &str, right: &str) -> String {
        let pair = self.id("v");
        let overflow = self.id("v");
        let trap = self.id("trap");
        let okay = self.id("okay");
        let result = self.id("v");
        self.instr(format!("%{pair} = call {{ i64, i1 }} @llvm.{opcode}.with.overflow.i64(i64 {left}, i64 {right})"));
        self.instr(format!("%{overflow} = extractvalue {{ i64, i1 }} %{pair}, 1"));
        self.instr(format!("br i1 %{overflow}, label %{trap}, label %{okay}"));
        self.terminated = true;
        self.label(&trap);
        self.instr("call void @llvm.trap()");
        self.instr("unreachable");
        self.terminated = true;
        self.label(&okay);
        self.instr(format!("%{result} = extractvalue {{ i64, i1 }} %{pair}, 0"));
        format!("%{result}")
    }

    fn expr(&mut self, expr: &TE) -> Result<String, String> {
        match &expr.e {
            TExpr::I64(n) => Ok(n.to_string()),
            TExpr::Bool(b) => Ok(if *b { "true" } else { "false" }.to_string()),
            TExpr::Local(slot) => {
                let idx = *slot as usize;
                let ty = scalar_type(*self.slots.get(idx).ok_or("E_INTERNAL: bad local slot")?)?;
                let tmp = self.id("v");
                self.instr(format!("%{tmp} = load {ty}, ptr %local{idx}"));
                Ok(format!("%{tmp}"))
            }
            TExpr::Neg(value) => {
                if value.ty != Type::Int {
                    return Err("E_UNSUPPORTED_NATIVE_NEG: expected int".into());
                }
                let x = self.expr(value)?;
                Ok(self.checked_math("ssub", "0", &x))
            }
            TExpr::Not(value) => {
                if value.ty != Type::Bool {
                    return Err("E_UNSUPPORTED_NATIVE_NOT: expected bool".into());
                }
                let x = self.expr(value)?;
                let tmp = self.id("v");
                self.instr(format!("%{tmp} = xor i1 {x}, true"));
                Ok(format!("%{tmp}"))
            }
            TExpr::Bin(op, lhs, rhs) => {
                let a = self.expr(lhs)?;
                let b = self.expr(rhs)?;
                let ty = scalar_type(lhs.ty)?;
                match op {
                    BinOp::Add | BinOp::Sub | BinOp::Mul => {
                        if ty != "i64" {
                            return Err("E_UNSUPPORTED_NATIVE_ARITH: expected int".into());
                        }
                        let op_name = match op {
                            BinOp::Add => "sadd",
                            BinOp::Sub => "ssub",
                            _ => "smul",
                        };
                        Ok(self.checked_math(op_name, &a, &b))
                    }
                    BinOp::Div => Err("E_UNSUPPORTED_NATIVE_DIV: integer division is not admitted".into()),
                    BinOp::And | BinOp::Or => {
                        if ty != "i1" {
                            return Err("E_UNSUPPORTED_NATIVE_LOGICAL: expected bool".into());
                        }
                        let tmp = self.id("v");
                        let opcode = if *op == BinOp::And { "and" } else { "or" };
                        self.instr(format!("%{tmp} = {opcode} i1 {a}, {b}"));
                        Ok(format!("%{tmp}"))
                    }
                    BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                        let pred = match op {
                            BinOp::Eq => "eq", BinOp::Ne => "ne",
                            BinOp::Lt => "slt", BinOp::Le => "sle",
                            BinOp::Gt => "sgt", _ => "sge",
                        };
                        let tmp = self.id("v");
                        self.instr(format!("%{tmp} = icmp {pred} {ty} {a}, {b}"));
                        Ok(format!("%{tmp}"))
                    }
                }
            }
            TExpr::Call(index, args) => {
                let fun = self.program.funcs.get(*index as usize)
                    .ok_or("E_INTERNAL: bad function index")?;
                if args.len() != fun.params.len() {
                    return Err("E_INTERNAL: checked call arity mismatch".into());
                }
                let mut pieces = Vec::new();
                for (arg, pty) in args.iter().zip(fun.params.iter()) {
                    let v = self.expr(arg)?;
                    let ty = scalar_type(*pty)?;
                    pieces.push(format!("{ty} {v}"));
                }
                let rty = scalar_type(fun.ret)?;
                if rty == "void" {
                    self.instr(format!("call void @atto_func_{}({})", index, pieces.join(", ")));
                    Ok(String::new())
                } else {
                    let tmp = self.id("v");
                    self.instr(format!("%{tmp} = call {rty} @atto_func_{}({})", index, pieces.join(", ")));
                    Ok(format!("%{tmp}"))
                }
            }
            TExpr::Ternary(cond, yes, no) => {
                let cv = self.expr(cond)?;
                let yes_label = self.id("tern_yes");
                let no_label = self.id("tern_no");
                let done_label = self.id("tern_end");
                self.instr(format!("br i1 {cv}, label %{yes_label}, label %{no_label}"));
                self.terminated = true;
                self.label(&yes_label);
                let y = self.expr(yes)?;
                let y_pred = self.block_name.clone();
                self.branch(&done_label);
                self.label(&no_label);
                let n = self.expr(no)?;
                let n_pred = self.block_name.clone();
                self.branch(&done_label);
                self.label(&done_label);
                let ty = scalar_type(yes.ty)?;
                if ty == "void" {
                    return Err("E_UNSUPPORTED_NATIVE_TERNARY: void".into());
                }
                let tmp = self.id("v");
                self.instr(format!("%{tmp} = phi {ty} [ {y}, %{y_pred} ], [ {n}, %{n_pred} ]"));
                Ok(format!("%{tmp}"))
            }
            _ => Err(format!("E_UNSUPPORTED_NATIVE_EXPR: {:?}", expr.e)),
        }
    }

    fn block(&mut self, statements: &[TStmt]) -> Result<(), String> {
        for statement in statements {
            if self.terminated {
                break;
            }
            self.statement(statement)?;
        }
        Ok(())
    }

    fn statement(&mut self, statement: &TStmt) -> Result<(), String> {
        match statement {
            TStmt::Set(slot, value) => {
                let idx = *slot as usize;
                let ty = scalar_type(*self.slots.get(idx).ok_or("E_INTERNAL: bad set slot")?)?;
                let x = self.expr(value)?;
                self.instr(format!("store {ty} {x}, ptr %local{idx}"));
            }
            TStmt::Expr(expr) => {
                let _ = self.expr(expr)?;
            }
            TStmt::Return(Some(expr)) => {
                let ty = scalar_type(expr.ty)?;
                if ty == "void" {
                    return Err("E_INTERNAL: return void value".into());
                }
                let x = self.expr(expr)?;
                self.instr(format!("ret {ty} {x}"));
                self.terminated = true;
            }
            TStmt::Return(None) => {
                if self.func.ret != Type::Void {
                    return Err("E_INTERNAL: return without value".into());
                }
                self.instr("ret void");
                self.terminated = true;
            }
            TStmt::Block(block) => self.block(block)?,
            TStmt::If(arms, fallback) => {
                let end_label = self.id("if_end");
                for (test, body) in arms {
                    let cv = self.expr(test)?;
                    let then_label = self.id("if_then");
                    let next_label = self.id("if_next");
                    self.instr(format!("br i1 {cv}, label %{then_label}, label %{next_label}"));
                    self.terminated = true;
                    self.label(&then_label);
                    self.block(body)?;
                    if !self.terminated {
                        self.branch(&end_label);
                    }
                    self.label(&next_label);
                }
                if let Some(otherwise) = fallback {
                    self.block(otherwise)?;
                }
                if !self.terminated {
                    self.branch(&end_label);
                }
                self.label(&end_label);
            }
            _ => return Err(format!("E_UNSUPPORTED_NATIVE_STMT: {statement:?}")),
        }
        Ok(())
    }

    fn emit(mut self, index: usize) -> Result<String, String> {
        let return_ty = scalar_type(self.func.ret)?;
        let mut args = Vec::new();
        for (i, t) in self.func.params.iter().enumerate() {
            args.push(format!("{} %arg{i}", scalar_type(*t)?));
        }
        let _ = writeln!(self.out, "define internal {return_ty} @atto_func_{index}({}) {{", args.join(", "));
        self.label("entry");
        let all_slots = self.slots.clone();
        for (idx, ty) in all_slots.into_iter().enumerate() {
            let llty = scalar_type(ty)?;
            self.instr(format!("%local{idx} = alloca {llty}"));
            if idx < self.func.params.len() {
                self.instr(format!("store {llty} %arg{idx}, ptr %local{idx}"));
            } else {
                let zero = if ty == Type::Bool { "false" } else { "0" };
                self.instr(format!("store {llty} {zero}, ptr %local{idx}"));
            }
        }
        self.block(&self.func.body)?;
        if !self.terminated {
            // The typed checker must prove definite return on reachable paths.
            // An unreachable join block traps rather than inventing a result.
            self.instr("call void @llvm.trap()");
            self.instr("unreachable");
            self.terminated = true;
        }
        let _ = writeln!(self.out, "}}");
        Ok(self.out)
    }
}

pub fn compile_ir(source: &str) -> Result<String, String> {
    if source.len() > atto_compiler::SOURCE_CAP {
        return Err("E_SOURCE_LIMIT: native Atto input exceeds 64 KiB".into());
    }
    let toks = atto_compiler::lexer::lex(source).map_err(|e|e.to_string())?;
    let program = atto_compiler::parser::Parser::new(toks).parse_program()
        .map_err(|e|e.to_string())?;
    let typed = check::check(&program).map_err(|e|e.to_string())?;
    if !typed.imports.is_empty() || !typed.generators.is_empty() ||
       !typed.actors.is_empty() || !typed.globals.is_empty() ||
       typed.heap || typed.strings || typed.guest_eh || !typed.data.is_empty() {
        return Err("E_UNSUPPORTED_NATIVE_RUNTIME: guest I/O/heap/actor/generator effects not lowered".into());
    }
    let main = typed.funcs.get(typed.main as usize)
        .ok_or("E_INTERNAL: main index not found")?;
    if !main.params.is_empty() || main.ret != Type::Int {
        return Err("E_UNSUPPORTED_NATIVE_MAIN: requires main(): int".into());
    }
    let mut out = String::from(
        "; Atto scalar native AOT: checked typed frontend, direct LLVM IR, no guest interpreter\n\
         declare void @llvm.trap()\n\
         declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)\n\
         declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)\n\
         declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)\n\n");
    for (idx, func) in typed.funcs.iter().enumerate() {
        out.push_str(&FunctionEmitter::new(&typed, func)?.emit(idx)?);
        out.push('\n');
    }
    let _ = writeln!(out, "define i32 @main() {{\nentry:\n  %answer = call i64 @atto_func_{}()\n  %code = trunc i64 %answer to i32\n  ret i32 %code\n}}\n", typed.main);
    Ok(out)
}

fn cli() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (src, out, emit_only) = match args.as_slice() {
        [src, flag, out] if flag == "-o" => (src, out, false),
        [src, flag, out, mode] if flag == "-o" && mode == "--emit-llvm" => (src, out, true),
        _ => return Err("usage: atto-llvm input.atto -o output [--emit-llvm]".into()),
    };
    if !src.ends_with(".atto") {
        return Err("E_SOURCE: input must have the .atto extension".into());
    }
    let content = std::fs::read_to_string(src).map_err(|e|format!("E_INPUT: {e}"))?;
    let ir = compile_ir(&content)?;
    if emit_only {
        std::fs::write(out, ir).map_err(|e|format!("E_OUTPUT: {e}"))?;
        return Ok(());
    }
    // LLVM's clang lowers the generated IR to a native, closed-world binary.
    // No Wasm runtime, guest interpreter, dynamic language VM, or collector.
    let mut child = Command::new("clang")
        .args(["-x", "ir", "-O2", "-o", out, "-"])
        .stdin(Stdio::piped()).spawn()
        .map_err(|e|format!("E_CLANG: {e}"))?;
    if let Some(mut input) = child.stdin.take() {
        input.write_all(ir.as_bytes()).map_err(|e|format!("E_CLANG_INPUT: {e}"))?;
    }
    let status = child.wait().map_err(|e|format!("E_CLANG_WAIT: {e}"))?;
    if !status.success() {
        let _ = std::fs::remove_file(out);
        return Err(format!("E_CLANG: compilation failed ({status})"));
    }
    Ok(())
}

fn main() {
    if let Err(e) = cli() {
        eprintln!("{e}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_main_is_direct_i64_call() {
        let ir = compile_ir("pub fnc main(): int { return 42; }").unwrap();
        assert!(ir.contains("define i32 @main()"));
        assert!(ir.contains("ret i64 42"));
        assert!(!ir.contains("wasm"));
    }
    #[test]
    fn rejects_unbound_identifiers_without_emitting_ir() {
        let e = compile_ir("pub fnc main(): int { return missing_symbol; }").unwrap_err();
        assert!(e.contains("E_UNBOUND"), "{e}");
    }
    #[test]
    fn retains_checked_overflow_intrinsics() {
        let ir = compile_ir("pub fnc main(): int { return 2 + 3; }").unwrap();
        assert!(ir.contains("llvm.sadd.with.overflow.i64"));
        assert!(ir.contains("llvm.trap"));
    }
    #[test]
    fn rejects_runtime_printing_without_native_abi() {
        assert!(compile_ir("pub fnc main(): int { stdio.println(1); return 42; }").is_err());
    }
}
