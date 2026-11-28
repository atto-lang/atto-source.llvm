//! Closed native readonly object profile. The candidate frontend is not
//! sufficient authority: every typed function and initializer is checked
//! below before any artifact is returned. No heap, raw-pointer syntax, FFI,
//! actors, constructor effects, escaping objects, or recursive calls.
use std::fmt::Write;
use atto_compiler::ast::{Program, Type, BinOp};
use atto_compiler::check::{TProgram, TFunc, TExpr, TStmt, TE};

fn deny(why: &str) -> String { format!("E_NATIVE_READONLY: {why}") }

pub fn admit_source(p: &Program) -> Result<(), String> {
    if p.classes.is_empty() || p.classes.len() > 32 || p.funcs.len() > 64
        || !p.actors.is_empty() || p.actor_interface.is_some() || !p.enums.is_empty()
        || p.funcs.iter().any(|f| f.is_generator || f.ret != Type::Int
            || f.params.iter().any(|a| a.mutable))
        || p.classes.iter().any(|c| c.ctor.is_some() || !c.methods.is_empty()
            || c.fields.is_empty() || c.fields.len() > 32
            || c.fields.iter().any(|f| f.ty != Type::Int)) {
        return Err(deny("only closed integer-field classes and synchronous int functions"));
    }
    Ok(())
}

fn llty(t: Type) -> Result<&'static str, String> {
    match t { Type::Int => Ok("i64"), Type::Class(_) => Ok("ptr"), _ => Err(deny("unsupported signature")) }
}

struct Emit<'a> {
    p: &'a TProgram,
    widths: &'a [usize],
    f: &'a TFunc,
    index: usize,
    values: Vec<Option<String>>,
    out: String,
    next: usize,
    function_count: usize,
}
impl Emit<'_> {
    fn temp(&mut self, instruction: String) -> String {
        let name = format!("%v{}", self.next);
        self.next += 1;
        writeln!(self.out, "  {name} = {instruction}").unwrap();
        name
    }
    fn local(&self, v: &TE) -> Result<String, String> {
        let TExpr::Local(slot) = &v.e else { return Err(deny("named local required")); };
        let slot = *slot as usize;
        let t = if slot < self.f.params.len() { self.f.params.get(slot) }
            else { self.f.locals.get(slot-self.f.params.len()) };
        if t != Some(&v.ty) { return Err(deny("local type mismatch")); }
        self.values.get(slot).and_then(Clone::clone).ok_or_else(|| deny("uninitialized local"))
    }
    fn expr(&mut self, v: &TE, depth: usize) -> Result<String, String> {
        if depth > 32 || v.ty != Type::Int { return Err(deny("integer expression budget/type")); }
        match &v.e {
            TExpr::I64(n) => Ok(n.to_string()),
            TExpr::Local(_) => self.local(v),
            TExpr::FieldGet { obj, offset } => {
                let Type::Class(c) = obj.ty else { return Err(deny("field receiver")); };
                let width = *self.widths.get(c as usize).ok_or_else(|| deny("unknown layout"))?;
                if *offset % 8 != 0 || (*offset as usize)/8 >= width { return Err(deny("field offset")); }
                let pointer = self.local(obj)?;
                let field = self.temp(format!("getelementptr i8, ptr {pointer}, i64 {offset}"));
                Ok(self.temp(format!("load i64, ptr {field}, align 8")))
            }
            TExpr::Bin(op, a, b) => {
                let helper = match op {
                    BinOp::Add => "add", BinOp::Sub => "sub", BinOp::Mul => "mul",
                    _ => return Err(deny("only checked add/sub/mul")),
                };
                let left = self.expr(a, depth+1)?;
                let right = self.expr(b, depth+1)?;
                Ok(self.temp(format!("call i64 @atto_ro_{helper}(i64 {left}, i64 {right})")))
            }
            TExpr::Call(target, args) => {
                let target = *target as usize;
                // A strict declaration-order DAG is this profile's bounded
                // call-depth certificate. No recursive or forward edges.
                if target >= self.index || target >= self.function_count
                    || target == self.p.main as usize { return Err(deny("call graph not a closed DAG")); }
                let callee = &self.p.funcs[target];
                if callee.ret != Type::Int || args.len() != callee.params.len() {
                    return Err(deny("call signature"));
                }
                let mut pieces = Vec::new();
                for (arg, expected) in args.iter().zip(&callee.params) {
                    if arg.ty != *expected { return Err(deny("argument nominal type")); }
                    let value = if matches!(*expected, Type::Class(_)) {
                        self.local(arg)?
                    } else { self.expr(arg, depth+1)? };
                    pieces.push(format!("{} {value}", llty(*expected)?));
                }
                Ok(self.temp(format!("call i64 @atto_ro_f{target}({})", pieces.join(", "))))
            }
            _ => Err(deny("unsupported or effectful expression")),
        }
    }
}

// Verify compiler-generated constructor shape and read its checked literal
// initialization values. Never execute the Wasm bump-heap factory as a
// native pointer producer.
fn factory(f: &TFunc, class: usize, width: usize) -> Result<Vec<i64>, String> {
    let ty = Type::Class(class as u32);
    if !f.params.is_empty() || f.ret != ty || f.locals != [ty]
        || f.body.len() != width+2 { return Err(deny("factory signature")); }
    if !matches!(&f.body[0], TStmt::Set(0, TE { e: TExpr::Alloc(n), ty: t })
        if *t == ty && *n as usize == width*8) { return Err(deny("factory allocation")); }
    let mut values = Vec::new();
    for (index, stmt) in f.body[1..=width].iter().enumerate() {
        let TStmt::FieldSet { obj, offset, ty: field_ty, value } = stmt else {
            return Err(deny("factory initialization"));
        };
        if obj.ty != ty || !matches!(&obj.e, TExpr::Local(0))
            || *offset as usize != index*8 || *field_ty != Type::Int || value.ty != Type::Int {
            return Err(deny("factory layout"));
        }
        let TExpr::I64(n) = &value.e else { return Err(deny("literal integer initialization required")); };
        values.push(*n);
    }
    if !matches!(f.body.last(), Some(TStmt::Return(Some(TE { e: TExpr::Local(0), ty: t }))) if *t == ty) {
        return Err(deny("factory result"));
    }
    Ok(values)
}

pub fn emit(source: &Program, p: &TProgram) -> Result<String, String> {
    admit_source(source)?;
    if !p.actors.is_empty() || !p.globals.is_empty() || !p.generators.is_empty()
        || !p.imports.is_empty() || p.strings || p.guest_eh || !p.data.is_empty()
        || !p.heap || p.funcs.len() != source.funcs.len()+source.classes.len() {
        return Err(deny("unexpected runtime capability"));
    }
    let count = source.funcs.len();
    let widths: Vec<_> = source.classes.iter().map(|c| c.fields.len()).collect();
    let mut initializers = Vec::new();
    for (class, width) in widths.iter().enumerate() {
        initializers.push(factory(&p.funcs[count+class], class, *width)?);
    }
    let main = p.main as usize;
    if main >= count { return Err(deny("main index")); }
    let mut out = String::from("; Closed native readonly stack-object profile\ndeclare void @llvm.trap()\n");
    for (name, intrinsic) in [("add","sadd"),("sub","ssub"),("mul","smul")] {
        writeln!(out, "declare {{ i64, i1 }} @llvm.{intrinsic}.with.overflow.i64(i64, i64)\ndefine internal i64 @atto_ro_{name}(i64 %a, i64 %b) {{\nentry:\n  %r = call {{ i64, i1 }} @llvm.{intrinsic}.with.overflow.i64(i64 %a, i64 %b)\n  %bad = extractvalue {{ i64, i1 }} %r, 1\n  br i1 %bad, label %trap, label %ok\ntrap:\n  call void @llvm.trap()\n  unreachable\nok:\n  %v = extractvalue {{ i64, i1 }} %r, 0\n  ret i64 %v\n}}").unwrap();
    }
    for (index, f) in p.funcs[..count].iter().enumerate() {
        if f.ret != Type::Int || f.params.len() > 8 || (index != main && !f.locals.is_empty())
            || (index == main && (!f.params.is_empty() || f.locals.len() > 16
                || f.locals.iter().any(|t| !matches!(t, Type::Class(_))))) {
            return Err(deny("function frame"));
        }
        let params = f.params.iter().enumerate().map(|(i,t)|
            Ok(format!("{} %arg{i}", llty(*t)?))).collect::<Result<Vec<_>,String>>()?;
        let mut e = Emit { p, widths: &widths, f, index,
            values: (0..f.params.len()).map(|i| Some(format!("%arg{i}"))).collect(),
            out: format!("define internal i64 @atto_ro_f{index}({}) {{\nentry:\n", params.join(", ")),
            next: 0, function_count: count };
        e.values.resize(f.params.len()+f.locals.len(), None);
        let (last, prefix) = f.body.split_last().ok_or_else(|| deny("empty body"))?;
        for stmt in prefix {
            if index != main { return Err(deny("helper must have only scalar return")); }
            let TStmt::Set(slot, TE { e: TExpr::Call(target,args), ty: Type::Class(class) }) = stmt
                else { return Err(deny("only root-local allocation bindings")); };
            let slot = *slot as usize;
            let class = *class as usize;
            if f.locals.get(slot) != Some(&Type::Class(class as u32))
                || e.values.get(slot).and_then(|v| v.as_ref()).is_some()
                || *target as usize != count+class || !args.is_empty() {
                return Err(deny("root binding provenance"));
            }
            let values = initializers.get(class).ok_or_else(|| deny("class layout"))?;
            let ptr = e.temp(format!("alloca [{} x i64], align 8", values.len()));
            for (offset, value) in values.iter().enumerate() {
                let field = e.temp(format!("getelementptr i8, ptr {ptr}, i64 {}", offset*8));
                writeln!(e.out, "  store i64 {value}, ptr {field}, align 8").unwrap();
            }
            e.values[slot] = Some(ptr);
        }
        let TStmt::Return(Some(value)) = last else { return Err(deny("scalar return required")); };
        let result = e.expr(value,0)?;
        writeln!(e.out, "  ret i64 {result}\n}}").unwrap();
        out.push_str(&e.out);
    }
    writeln!(out, "define i32 @main() {{\nentry:\n  %answer = call i64 @atto_ro_f{main}()\n  %code = trunc i64 %answer to i32\n  ret i32 %code\n}}").unwrap();
    Ok(out)
}
