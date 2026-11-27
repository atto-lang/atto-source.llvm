//! Explicit bounded source-to-native continuation profile; independent of default IR.
//! Parser derived from Rust #742, checked lexical ownership and static stages.
//! Caller-owned frame, no heap allocation, no callback interpreter.
use atto_compiler::diag::{Diagnostic,Pos};
use atto_compiler::lexer::{lex,Tok,Token};
use std::fmt::Write as _;
use std::collections::{BTreeMap,BTreeSet};

const MAX:usize=8;
#[derive(Clone,Copy)]
enum Stage { Body(u32), Cleanup(u32), Recover(u32) }
impl Stage { fn slot(self)->u32 { match self {Self::Body(n)|Self::Cleanup(n)|Self::Recover(n)=>n} } }
struct Plan { stages:Vec<Stage>, literal:i64 }
struct Parser { t:Vec<Token>, source:String, at:usize, formals:BTreeMap<String,u32>, consumed:BTreeSet<u32> }
impl Parser {
    fn pos(&self)->Pos { self.t.get(self.at).map_or(Pos{line:1,col:1},|t|t.pos) }
    fn err(&self,code:&'static str,msg:&str)->Diagnostic { Diagnostic::new(self.pos(),code,msg) }
    fn peek(&self)->&Tok { &self.t[self.at].tok }
    fn eat(&mut self,t:Tok)->bool { if *self.peek()==t { self.at+=1;true } else {false} }
    fn need(&mut self,t:Tok)->Result<(),Diagnostic> {
        if self.eat(t.clone()) {Ok(())} else {Err(self.err("E_PENDING_UNWIND_PROFILE",&format!("expected {t:?}")))}
    }
    fn word(&mut self,w:&str)->Result<(),Diagnostic> { self.need(Tok::Ident(w.into())) }
    fn ident(&mut self)->Result<String,Diagnostic> {
        if let Tok::Ident(s)=self.peek() {
            if !s.is_empty() && s.bytes().enumerate().all(|(i,b)|b==b'_'||b.is_ascii_alphabetic()||(i>0&&b.is_ascii_digit()))
                && !["pub","fnc","run","suspends","Future","int","defer","recover","return","await","take","move","borrow","raise","throw","panic","let","val","const","true","false","if","self","new","select","mut"].contains(&s.as_str()) {
                let s=s.clone();self.at+=1;return Ok(s)
            }
        }
        Err(self.err("E_PENDING_UNWIND_PROFILE","expected nonreserved ASCII binder"))
    }
    fn consume(&mut self)->Result<u32,Diagnostic> {
        let n=self.ident()?;
        let slot=*self.formals.get(&n).ok_or_else(||self.err("E_OWNERSHIP","await requires an owned Future formal"))?;
        if !self.consumed.insert(slot){return Err(self.err("E_MOVED","Future formal used twice"))}
        Ok(slot)
    }
    fn number(&mut self)->Result<i64,Diagnostic>{
        let neg=self.eat(Tok::Minus);
        if let Tok::Int(Some(n))=*self.peek(){
            if n<=i64::MAX as u64+u64::from(neg){
                self.at+=1;
                return Ok(if neg { if n==1u64<<63{i64::MIN}else{-(n as i64)} } else {n as i64});
            }
        }
        Err(self.err("E_INTEGER_RANGE","signed i64 literal required"))
    }
    fn parse(mut self)->Result<Plan,Diagnostic>{
        self.word("pub")?;self.word("suspends")?;self.word("fnc")?;self.word("run")?;self.need(Tok::LParen)?;
        loop {
            if self.formals.len()>=MAX{return Err(self.err("E_PENDING_UNWIND_PROFILE","formal quota exceeded"))}
            let p=self.pos();
            if !self.eat(Tok::Ident("take".into())){return Err(self.err("E_OWNERSHIP","sealed Future requires take! formal"))}
            let bang=self.pos();
            if p.line!=bang.line||p.col.checked_add(4)!=Some(bang.col){return Err(self.err("E_OWNERSHIP","take! must be contiguous"))}
            self.need(Tok::Bang)?;
            let after=self.source.split('\n').nth(bang.line as usize-1).and_then(|s|s.chars().nth(bang.col as usize));
            if !after.is_some_and(|c|matches!(c,' '|'\t'|'\r'|'\n'|'\x0b'|'\x0c')) && !(after.is_none() && self.pos().line>bang.line) {return Err(self.err("E_OWNERSHIP","whitespace required after take!"))}
            self.word("Future")?;self.need(Tok::Lt)?;self.word("int")?;self.need(Tok::Gt)?;
            let n=self.ident()?;let slot=self.formals.len() as u32+1;
            if self.formals.insert(n,slot).is_some(){return Err(self.err("E_DUPLICATE","duplicate Future formal"))}
            if !self.eat(Tok::Comma){break}
        }
        self.need(Tok::RParen)?;self.need(Tok::Colon)?;self.word("int")?;self.need(Tok::LBrace)?;
        let mut defers=Vec::new();let mut recovery=None;
        loop {
            if self.eat(Tok::Ident("defer".into())) {
                self.word("await")?;defers.push(Stage::Cleanup(self.consume()?));self.need(Tok::Semi)?;
            }else if self.eat(Tok::Ident("recover".into())){
                if recovery.is_some(){return Err(self.err("E_PENDING_UNWIND_PROFILE","only one recovery callback"))}
                self.need(Tok::Pipe)?;let err=self.ident()?;self.need(Tok::Comma)?;let cursor=self.ident()?;self.need(Tok::Pipe)?;
                if err==cursor||self.formals.contains_key(&err)||self.formals.contains_key(&cursor){return Err(self.err("E_DUPLICATE","recovery binder collision"))}
                self.need(Tok::Arrow)?;self.need(Tok::LBrace)?;self.word("return")?;self.word("await")?;
                recovery=Some(Stage::Recover(self.consume()?));self.need(Tok::Semi)?;self.need(Tok::RBrace)?;self.need(Tok::Semi)?;
            }else{break}
        }
        self.word("return")?;
        let body=if self.eat(Tok::Ident("await".into())){Some(Stage::Body(self.consume()?))}else{None};
        let literal=if body.is_none(){self.number()?}else{0};
        self.need(Tok::Semi)?;self.need(Tok::RBrace)?;
        if *self.peek()!=Tok::Eof || self.consumed.len()!=self.formals.len() || (recovery.is_some()&&body.is_none()) {
            return Err(self.err("E_PENDING_UNWIND_PROFILE","closed profile requires exactly one lexical use of every owned Future and recovery requires body await"))
        }
        let mut stages=Vec::new();stages.extend(body);stages.extend(defers.into_iter().rev());stages.extend(recovery);
        Ok(Plan{stages,literal})
    }
}

const OWNER:usize=0;const EPOCH:usize=1;const PC:usize=2;const RANK:usize=3;const RECOVERED:usize=4;
const VALUE:usize=5;const ERROR:usize=6;const KIND:usize=7;const COMPLETION:usize=8;
const COUNT:usize=9;const SUP:usize=10;const RANKS:usize=18;const WORDS:usize=26;
struct Ir { s:String,n:usize }
impl Ir {
 fn new()->Self{Self{s:"; ModuleID = 'atto-owned-unwind-v1'\n".into(),n:0}}
 fn line(&mut self,s:impl AsRef<str>){writeln!(self.s,"  {}",s.as_ref()).unwrap();}
 fn label(&mut self,s:&str){writeln!(self.s,"{s}:").unwrap();}
 fn v(&mut self,s:impl AsRef<str>)->String{self.n+=1;let n=format!("%v{}",self.n);self.line(format!("{n} = {}",s.as_ref()));n}
 fn ptr(&mut self,i:usize)->String{self.v(format!("getelementptr i64, ptr %f, i64 {i}"))}
 fn get(&mut self,i:usize)->String{let p=self.ptr(i);self.v(format!("load i64, ptr {p}, align 8"))}
 fn set(&mut self,i:usize,v:impl std::fmt::Display){let p=self.ptr(i);self.line(format!("store i64 {v}, ptr {p}, align 8"))}
 fn eq(&mut self,v:&str,n:impl std::fmt::Display)->String{self.v(format!("icmp eq i64 {v}, {n}"))}
 fn br(&mut self,c:&str,y:&str,n:&str){self.line(format!("br i1 {c}, label %{y}, label %{n}"))}
 fn ret(&mut self,n:impl std::fmt::Display){self.line(format!("ret i64 {n}"))}
 fn begin(&mut self,name:&str,extra:&str){writeln!(self.s,"define i64 @unwind_{name}_v1(ptr %f, i64 %owner, i64 %epoch{extra}) {{").unwrap();self.label("entry");}
 fn close(&mut self){self.label("bad");self.ret(-1);self.s.push_str("}\n");}
 fn owner(&mut self,check:bool){
  let nonnull=self.v("icmp ne ptr %f, null");
  let a=self.v("icmp sgt i64 %owner, 0");let b=self.v("icmp sgt i64 %epoch, 0");
  let ab=self.v(format!("and i1 {a}, {b}"));let valid=self.v(format!("and i1 {nonnull}, {ab}"));
  self.br(&valid,"owner_check","bad");self.label("owner_check");
  if check{let o=self.get(OWNER);let e=self.get(EPOCH);let a=self.v(format!("icmp eq i64 {o}, %owner"));let b=self.v(format!("icmp eq i64 {e}, %epoch"));let ok=self.v(format!("and i1 {a}, {b}"));self.br(&ok,"live","bad");self.label("live");}
 }
 fn terminal(&mut self){self.owner(true);let pc=self.get(PC);let ok=self.eq(&pc,32);self.br(&ok,"terminal","bad");self.label("terminal");}
 fn switch(&mut self,p:&str,stages:&[Stage],offset:usize){self.line(format!("switch i64 {p}, label %bad ["));for i in 0..stages.len(){self.line(format!("i64 {}, label %s{i}",offset+i+1));}self.line("]");}
 fn finish(&mut self,p:&Plan,next:usize){
   if next>=p.stages.len(){self.set(PC,32);self.ret(2);}
   else if matches!(p.stages[next],Stage::Recover(_)){
    let rank=self.get(RANK);let ok=self.eq(&rank,1);self.br(&ok,&format!("recover{next}"),&format!("finish{next}"));
    self.label(&format!("finish{next}"));self.set(PC,32);self.ret(2);
    self.label(&format!("recover{next}"));self.set(PC,next+1);self.ret(1);
   }else{self.set(PC,next+1);self.ret(1);}
 }
}
fn emit(p:&Plan)->String{
 let mut g=Ir::new();
 writeln!(g.s,"define i64 @unwind_words_v1() {{ ret i64 {WORDS} }}\ndefine i64 @unwind_arity_v1() {{ ret i64 {} }}",p.stages.len()).unwrap();
 // merge rank and failure ID with bounded secondary log (at most seven).
 g.s.push_str("define internal void @merge(ptr %f, i64 %r, i64 %e) {\nentry:\n");
 let old=g.get(RANK);let olde=g.get(ERROR);let higher=g.v(format!("icmp sgt i64 %r, {old}"));
 let sr=g.v(format!("select i1 {higher}, i64 {old}, i64 %r"));
 let se=g.v(format!("select i1 {higher}, i64 {olde}, i64 %e"));
 let nr=g.v(format!("select i1 {higher}, i64 %r, i64 {old}"));
 let ne=g.v(format!("select i1 {higher}, i64 %e, i64 {olde}"));
 g.set(RANK,nr);g.set(ERROR,ne);
 let nz=g.v(format!("icmp ne i64 {sr}, 0"));g.br(&nz,"append","done");g.label("append");
 let count=g.get(COUNT);let ok=g.v(format!("icmp ult i64 {count}, 8"));g.br(&ok,"write","fatal");g.label("write");
 for (base,val) in [(SUP,se),(RANKS,sr)] {let i=g.v(format!("add i64 {count}, {base}"));let ptr=g.v(format!("getelementptr i64, ptr %f, i64 {i}"));g.line(format!("store i64 {val}, ptr {ptr}, align 8"));}
 let next=g.v(format!("add i64 {count}, 1"));g.set(COUNT,next);g.line("br label %done");g.label("fatal");g.line("call void @llvm.trap()");g.line("unreachable");g.label("done");g.line("ret void");g.s.push_str("}\ndeclare void @llvm.trap()\n");
 g.begin("start","");g.owner(false);let pc=g.get(PC);let ok=g.eq(&pc,0);g.br(&ok,"start","bad");g.label("start");
 // Fresh caller-owned zero-initialized frame. Clear all slots before use.
 for i in 0..WORDS{g.set(i,0);}g.set(OWNER,"%owner");g.set(EPOCH,"%epoch");g.set(PC,1);g.set(VALUE,p.literal);g.ret(1);g.close();
 g.begin("pending","");g.owner(true);let pc=g.get(PC);g.switch(&pc,&p.stages,0);
 for(i,s)in p.stages.iter().enumerate(){g.label(&format!("s{i}"));g.ret(s.slot());}g.close();
 g.begin("complete",", i64 %slot, i64 %kind, i64 %value");g.owner(true);
 let valid=g.v("icmp ule i64 %kind, 2");let success=g.v("icmp eq i64 %kind, 0");let positive=g.v("icmp sgt i64 %value, 0");let payload=g.v(format!("or i1 {success}, {positive}"));let ok=g.v(format!("and i1 {valid}, {payload}"));g.br(&ok,"dispatch","bad");g.label("dispatch");let pc=g.get(PC);g.switch(&pc,&p.stages,0);
 for(i,s)in p.stages.iter().enumerate(){g.label(&format!("s{i}"));let ok=g.eq("%slot",s.slot());g.br(&ok,&format!("accept{i}"),"bad");g.label(&format!("accept{i}"));g.set(KIND,"%kind");g.set(COMPLETION,"%value");g.set(PC,17+i);g.ret(0);}g.close();
 g.begin("resume","");g.owner(true);let pc=g.get(PC);g.switch(&pc,&p.stages,16);
 for(i,s)in p.stages.iter().enumerate(){
  g.label(&format!("s{i}"));let k=g.get(KIND);let v=g.get(COMPLETION);let ok=g.eq(&k,0);g.br(&ok,&format!("value{i}"),&format!("failure{i}"));
  g.label(&format!("value{i}"));if matches!(s,Stage::Body(_)|Stage::Recover(_)){g.set(VALUE,&v);}if matches!(s,Stage::Recover(_)){g.set(RECOVERED,1);}g.line(format!("br label %next{i}"));
  g.label(&format!("failure{i}"));let cancel=g.eq(&k,2);let rank=g.v(format!("select i1 {cancel}, i64 3, i64 {}",if matches!(s,Stage::Body(_)){1}else{2}));g.line(format!("call void @merge(ptr %f, i64 {rank}, i64 {v})"));g.line(format!("br label %next{i}"));
  g.label(&format!("next{i}"));g.set(KIND,0);g.set(COMPLETION,0);g.finish(p,i+1);
 }g.close();
 g.begin("outcome","");g.terminal();let r=g.get(RANK);let recovered=g.get(RECOVERED);let success=g.eq(&r,0);let rec=g.eq(&recovered,1);let failure=g.v(format!("add i64 {r}, 2"));let result=g.v(format!("select i1 {success}, i64 1, i64 {failure}"));let result=g.v(format!("select i1 {rec}, i64 2, i64 {result}"));g.ret(result);g.close();
 // Result uses an out pointer because every i64 is a valid returned value.
 g.begin("result",", ptr %out");g.terminal();let r=g.get(RANK);let recovered=g.get(RECOVERED);let a=g.eq(&r,0);let b=g.eq(&recovered,1);let ok=g.v(format!("or i1 {a}, {b}"));let nn=g.v("icmp ne ptr %out, null");let ok=g.v(format!("and i1 {ok}, {nn}"));g.br(&ok,"read","bad");g.label("read");let v=g.get(VALUE);g.line(format!("store i64 {v}, ptr %out, align 8"));g.ret(0);g.close();
 for(n,i)in [("error",ERROR),("suppressed_count",COUNT)]{g.begin(n,"");g.terminal();let v=g.get(i);g.ret(v);g.close();}
 for(n,base)in [("suppressed_error",SUP),("suppressed_rank",RANKS)]{
  g.begin(n,", i64 %index");g.terminal();let count=g.get(COUNT);let a=g.v(format!("icmp ult i64 %index, {count}"));let b=g.v("icmp ult i64 %index, 8");let ok=g.v(format!("and i1 {a}, {b}"));g.br(&ok,"read","bad");g.label("read");let i=g.v(format!("add i64 %index, {base}"));let ptr=g.v(format!("getelementptr i64, ptr %f, i64 {i}"));let v=g.v(format!("load i64, ptr {ptr}, align 8"));g.ret(v);g.close();
 }
 g.begin("retire","");g.terminal();for i in 0..WORDS{g.set(i,0);}g.set(PC,33);g.ret(0);g.close();g.s
}
pub fn compile(source:&str)->Result<String,String>{
 if source.len()>atto_compiler::SOURCE_CAP{return Err("E_SOURCE_LIMIT: owned unwind source cap".into())}
 let p=Parser{t:lex(source).map_err(|e|e.to_string())?,source:source.into(),at:0,formals:BTreeMap::new(),consumed:BTreeSet::new()}.parse().map_err(|e|e.to_string())?;
 Ok(emit(&p))
}
