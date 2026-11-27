//! Closed scalar native C lowering from the real checked frontend IR.
//! No host interpretation of source; Clang compiles generated code. Native
//! C++ frontend, ABI layout and resource ownership are deliberately separate.
use crate::ast::{BinOp, Type};
use crate::check::{Builtin, Import, TExpr, TProgram, TStmt, TE};
use crate::{Diagnostic, Pos};
use std::fmt::Write;
type R<T> = Result<T, Diagnostic>;
pub(crate) fn unsupported() -> Diagnostic {
    Diagnostic::new(Pos { line: 1, col: 1 }, "E_NATIVE_STRUCT_UNSUPPORTED",
        "native attached struct target admits only closed checked scalar IR")
}
fn ty(t: Type) -> R<&'static str> {
    match t { Type::Int => Ok("int64_t"), Type::Bool => Ok("int32_t"), Type::Void => Ok("void"), _ => Err(unsupported()) }
}
fn bounded(s: String) -> R<String> {
    if s.len() > crate::wasm::MAX_OUTPUT { Err(unsupported()) } else { Ok(s) }
}
fn expr(e: &TE, next: &mut usize) -> R<String> {
    ty(e.ty)?;
    let result = match &e.e {
        TExpr::I64(n) if *n == i64::MIN => "INT64_MIN".into(),
        TExpr::I64(n) => format!("((int64_t){n}LL)"),
        TExpr::Bool(b) => format!("((int32_t){})", u8::from(*b)),
        TExpr::Local(i) => format!("l{i}"),
        TExpr::Neg(a) => format!("sub64(0,{})", expr(a, next)?),
        TExpr::Not(a) => format!("((int32_t)!({}))", expr(a, next)?),
        TExpr::Ternary(c, a, b) => format!("(({})?({}):({}))", expr(c,next)?,expr(a,next)?,expr(b,next)?),
        TExpr::Bin(op, a, b) => {
            let i = *next; *next += 1;
            let l=expr(a,next)?; let r=expr(b,next)?;
            let op = match op {
                BinOp::Add => format!("add64(a{i},b{i})"),
                BinOp::Sub => format!("sub64(a{i},b{i})"),
                BinOp::Mul => format!("mul64(a{i},b{i})"),
                BinOp::Eq => format!("(a{i}==b{i})"),
                BinOp::Ne => format!("(a{i}!=b{i})"),
                BinOp::Lt => format!("(a{i}<b{i})"),
                BinOp::Le => format!("(a{i}<=b{i})"),
                BinOp::Gt => format!("(a{i}>b{i})"),
                BinOp::Ge => format!("(a{i}>=b{i})"),
                BinOp::And => format!("(a{i}&b{i})"),
                BinOp::Or => format!("(a{i}|b{i})"),
                BinOp::Div => return Err(unsupported()),
            };
            format!("({{ {} a{i}={l}; {} b{i}={r}; {op}; }})",ty(a.ty)?,ty(b.ty)?)
        }
        TExpr::Builtin(op,a,b) => {
            let i=*next; *next+=1;
            let l=expr(a,next)?; let r=expr(b,next)?;
            let op=match op {Builtin::And=>"&",Builtin::Or=>"|",Builtin::Xor=>"^"};
            format!("({{ int32_t a{i}={l}; int32_t b{i}={r}; a{i}{op}b{i}; }})")
        }
        TExpr::Call(i,args) => {
            // Snapshot each argument in source order, independent of C's
            // unspecified argument evaluation order.
            let id=*next; *next+=1;
            let mut out=String::from("({ ");
            let mut names=Vec::new();
            for (j,a) in args.iter().enumerate(){
                let name=format!("arg{id}_{j}");
                writeln!(out,"{} {name}={};",ty(a.ty)?,expr(a,next)?).unwrap();
                names.push(name);
            }
            write!(out,"f{i}({}); }})",names.join(",")).unwrap();
            out
        }
        TExpr::Print{value,newline}=>{
            let f=match value.ty {Type::Int=>"atto_print_i64",Type::Bool=>"atto_print_bool",_=>return Err(unsupported())};
            let a=expr(value,next)?;
            format!("({{ {f}({a}); {} }})",if *newline {"atto_print_newline();"} else {""})
        }
        _=>return Err(unsupported()),
    };
    bounded(result)
}
fn stmts(body:&[TStmt],out:&mut String,next:&mut usize)->R<()> {
    for s in body {
        match s {
            TStmt::Set(i,v)=>writeln!(out,"l{i}={};",expr(v,next)?).unwrap(),
            TStmt::Expr(e)=>writeln!(out,"(void)({});",expr(e,next)?).unwrap(),
            TStmt::Return(Some(e))=>writeln!(out,"return {};",expr(e,next)?).unwrap(),
            TStmt::Return(None)=>out.push_str("return;\n"),
            TStmt::If(arms,other)=>{
                for (i,(c,b)) in arms.iter().enumerate(){
                    writeln!(out,"{}if({}){{",if i==0{""}else{"else "},expr(c,next)?).unwrap();
                    stmts(b,out,next)?;out.push_str("}\n");
                }
                if let Some(b)=other{out.push_str("else{\n");stmts(b,out,next)?;out.push_str("}\n");}
            }
            TStmt::Block(b)=>{out.push_str("{\n");stmts(b,out,next)?;out.push_str("}\n");}
            TStmt::Unreachable=>out.push_str("atto_panic(1);\n"),
            _=>return Err(unsupported()),
        }
        if out.len()>crate::wasm::MAX_OUTPUT{return Err(unsupported());}
    }
    Ok(())
}
fn signature(p:&TProgram,i:usize)->R<String>{
    let f=&p.funcs[i];
    let args=f.params.iter().enumerate().map(|(j,t)|Ok(format!("{} l{j}",ty(*t)?))).collect::<R<Vec<_>>>()?;
    Ok(format!("static {} f{i}({})",ty(f.ret)?,if args.is_empty(){"void".into()}else{args.join(",")}))
}
pub(crate) fn emit(p:&TProgram)->R<String>{
    if p.heap||p.strings||p.guest_eh||!p.data.is_empty()||!p.actors.is_empty()||!p.generators.is_empty()||!p.globals.is_empty()
        ||p.imports.iter().any(|i|!matches!(i,Import::PrintI64|Import::PrintBool|Import::PrintNewline)){return Err(unsupported());}
    let m=p.funcs.get(p.main as usize).ok_or_else(unsupported)?;
    if m.ret!=Type::Int||!m.params.is_empty(){return Err(unsupported());}
    let mut out=String::from(r#"#include <stdint.h>
extern void atto_print_i64(int64_t);
extern void atto_print_bool(int32_t);
extern void atto_print_newline(void);
extern _Noreturn void atto_panic(int32_t);
static inline int64_t add64(int64_t a,int64_t b){int64_t r;if(__builtin_add_overflow(a,b,&r))atto_panic(1);return r;}
static inline int64_t sub64(int64_t a,int64_t b){int64_t r;if(__builtin_sub_overflow(a,b,&r))atto_panic(1);return r;}
static inline int64_t mul64(int64_t a,int64_t b){int64_t r;if(__builtin_mul_overflow(a,b,&r))atto_panic(1);return r;}
"#);
    for i in 0..p.funcs.len(){writeln!(out,"{};",signature(p,i)?).unwrap();}
    for (i,f) in p.funcs.iter().enumerate(){
        writeln!(out,"{}{{",signature(p,i)?).unwrap();
        for (j,t) in f.locals.iter().enumerate(){writeln!(out,"{} l{}=0;",ty(*t)?,j+f.params.len()).unwrap();}
        stmts(&f.body,&mut out,&mut 0)?;
        if f.ret==Type::Void{out.push_str("return;\n");}else{out.push_str("atto_panic(1);\n");}
        out.push_str("}\n");
    }
    writeln!(out,"int64_t atto_guest_main(void){{return f{}();}}",p.main).unwrap();
    bounded(out)
}
