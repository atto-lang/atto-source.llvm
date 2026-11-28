//! Recursive-descent parser for the browser AttoLang subset.
//!
//! The grammar follows `docs/grammar.ebnf` (Oreslang v0.6) in the Java
//! reference compiler and spec chapter 16. Constructs outside the admitted
//! subset are recognised and rejected with `E_UNSUPPORTED` rather than being
//! mis-parsed or silently approximated.

use crate::ast::*;
use crate::diag::{Diagnostic, Pos};
use crate::lexer::{Tok, Token};

pub const MAX_DEPTH: u32 = 128;
pub const MAX_FUNCS: usize = 1024;
pub const MAX_PARAMS: usize = 32;
/// Members (fields, methods, constructor, receive) in one class or actor,
/// and variants in one enum.
pub const MAX_MEMBERS: usize = 256;
/// Bound on the height of any syntax tree the parser builds. Left-associative
/// operator chains (`a + b + ...`) and `elif` arms are parsed iteratively but
/// produce trees that the checker, emitter and `Drop` walk recursively, so
/// each chain link counts against this budget as well as each nested level.
pub const MAX_TREE_HEIGHT: u32 = 256;

/// Words reserved by the reference lexer (spec chapter 16 keyword table).
const RESERVED: &[&str] = &[
    "define",
    "def",
    "module",
    "namespace",
    "contract",
    "conforms",
    "class",
    "struct",
    "interface",
    "trait",
    "type",
    "types",
    "typeof",
    "abstract",
    "extends",
    "implements",
    "impl",
    "import",
    "from",
    "as",
    "fnc",
    "routine",
    "async",
    "sync",
    "suspends",
    "generator",
    "yield",
    "nlex",
    "return",
    "self",
    "super",
    "static",
    "pub",
    "private",
    "const",
    "val",
    "let",
    "mut",
    "if",
    "then",
    "else",
    "elif",
    "elseif",
    "fi",
    "do",
    "done",
    "end",
    "while",
    "for",
    "of",
    "loop",
    "block",
    "break",
    "continue",
    "switch",
    "match",
    "matches",
    "over",
    "default",
    "try",
    "catch",
    "finally",
    "throw",
    "raise",
    "panic",
    "recover",
    "trap",
    "defer",
    "actor",
    "isoactor",
    "shared",
    "untrusted",
    "spawn",
    "pipe",
    "rpipe",
    "stop",
    "rt",
    "await",
    "nb",
    "cb",
    "select",
    "readch",
    "writech",
    "eq",
    "neq",
    "is",
    "new",
    "true",
    "false",
    "null",
    "void",
    "obj",
    "arr",
    "cpu",
    "gpu",
    "quantum",
];

/// Statement-leading keywords that name real AttoLang features this compiler
/// does not lower yet. They are rejected, never approximated.
const UNSUPPORTED_STMT: &[&str] = &[
    "switch",
    "try",
    "select",
    "defer",
    "throw",
    "raise",
    "panic",
    "recover",
    "trap",
    "spawn",
    "await",
    "actor",
    "isoactor",
    "define",
    "import",
    "async",
    "sync",
    "yield",
    "guard",
    "nb",
    "cb",
    "readch",
    "writech",
    "rt",
    "class",
    "struct",
    "module",
    "namespace",
    "interface",
    "trait",
    "type",
    "generator",
    "critical",
    "stop",
];

const UNSUPPORTED_TYPES: &[&str] = &[
    "f32",
    "decimal",
    "complex",
    "complex64",
    "complex128",
    "bigint",
    "i8",
    "i16",
    "i32",
    "i64",
    "u8",
    "u16",
    "u32",
    "u64",
    "uint",
    "Symbol",
    "Option",
    "Result",
    "Array",
    "List",
    "Vector",
    "Slice",
    "Tuple",
    "FixedArray",
    "FixedList",
    "Map",
    "Set",
    "char",
    "byte",
];

pub fn is_reserved(name: &str) -> bool {
    RESERVED.contains(&name)
}

pub struct Parser {
    toks: Vec<Token>,
    i: usize,
    depth: u32,
    /// Height of the tree path being built; see [`MAX_TREE_HEIGHT`].
    height: u32,
    /// Nonzero while parsing an if/while condition head, where `,` and `|` are
    /// boolean AND / OR (spec chapter 16).
    cond: u32,
    /// Spellings of user type references, see `Type::Named`.
    type_names: Vec<(String, Pos)>,
    /// Scope of an explicit generator function body.
    in_generator: bool,
    struct_target: bool,
    struct_value_bindings: Option<Vec<Pos>>,
    pattern_constants: std::collections::BTreeMap<String, (bool, Pattern)>,
    pattern_name_uses: std::collections::BTreeSet<usize>,
    pattern_type_spellings: std::collections::BTreeMap<String, Pos>,
    pattern_type_aliases: std::collections::BTreeMap<String, Vec<Type>>,
    pattern_alias_uses: std::collections::BTreeSet<usize>,
}

struct TypeFirstField {
    field: Field,
    /// `T name: e` (class field spelling) rather than `T name = e`.
    init_was_colon: bool,
}

/// Parse-only nominal struct target; never lowered as an identity-bearing class.
#[derive(Clone, Debug)]
pub struct StructWithTarget {
    pub name: String,
    pub pos: Pos,
    pub fields: Vec<(String, Type)>,
    pub constructors: Vec<(bool, Constructor)>,
    pub methods: Vec<Method>,
    pub mutable_methods: Vec<usize>,
}

/// Members collected from a class or actor body.
#[derive(Default)]
struct Members {
    fields: Vec<Field>,
    ctor: Option<Constructor>,
    methods: Vec<Method>,
    receive: Option<Receive>,
}

type R<T> = Result<T, Diagnostic>;

fn unsupported(pos: Pos, what: &str) -> Diagnostic {
    Diagnostic::new(pos, "E_UNSUPPORTED", format!("{what} is not supported by the browser compiler yet"))
}

fn lazy_guard_predicate(expr: Expr) -> Expr {
    let pos = expr.pos;
    match expr.kind {
        ExprKind::Binary(BinOp::And, left, right) => Expr {
            kind: ExprKind::Ternary(
                Box::new(lazy_guard_predicate(*left)),
                Box::new(lazy_guard_predicate(*right)),
                Box::new(Expr { kind: ExprKind::Bool(false), pos })),
            pos,
        },
        ExprKind::Binary(BinOp::Or, left, right) => Expr {
            kind: ExprKind::Ternary(
                Box::new(lazy_guard_predicate(*left)),
                Box::new(Expr { kind: ExprKind::Bool(true), pos }),
                Box::new(lazy_guard_predicate(*right))),
            pos,
        },
        kind => Expr { kind, pos },
    }
}

/// Conservative guard preflight: no path may escape through an explicit
/// throw, break, or continue before the required function return. The typed
/// checker still validates return types and exceptional effects separately.
fn guard_has_nonreturn_control_exit(stmts: &[Stmt]) -> bool {
    stmts.iter().any(|stmt| match stmt {
        Stmt::Throw(..) | Stmt::Break(..) | Stmt::Continue(..) => true,
        Stmt::Block(body) => guard_has_nonreturn_control_exit(body),
        Stmt::If { arms, otherwise, .. } =>
            arms.iter().any(|(_, body)| guard_has_nonreturn_control_exit(body))
                || otherwise.as_ref().is_some_and(|body| guard_has_nonreturn_control_exit(body)),
        Stmt::Match { arms, .. } =>
            arms.iter().any(|arm| guard_has_nonreturn_control_exit(&arm.body)),
        Stmt::While { body, .. } | Stmt::Loop { body, .. }
        | Stmt::For { body, .. } | Stmt::ForIn { body, .. } =>
            guard_has_nonreturn_control_exit(body),
        _ => false,
    })
}

fn guard_failure_returns(stmts: &[Stmt]) -> bool {
    match stmts.last() {
        Some(Stmt::Return(..)) => true,
        Some(Stmt::Block(body)) => guard_failure_returns(body),
        Some(Stmt::If { arms, otherwise: Some(fallback), .. }) => {
            !arms.is_empty()
                && arms.iter().all(|(_, body)| guard_failure_returns(body))
                && guard_failure_returns(fallback)
        }
        _ => false,
    }
}

impl Parser {
    pub fn new(toks: Vec<Token>) -> Self {
        Parser {
            toks,
            i: 0,
            depth: 0,
            height: 0,
            cond: 0,
            type_names: Vec::new(),
            in_generator: false,
            struct_target: false,
            struct_value_bindings: None,
            pattern_constants: Default::default(),
            pattern_name_uses: Default::default(),
            pattern_type_spellings: Default::default(),
            pattern_type_aliases: Default::default(),
            pattern_alias_uses: Default::default(),
        }
    }

    fn peek(&self) -> &Tok {
        &self.toks[self.i].tok
    }
    fn peek_at(&self, n: usize) -> &Tok {
        &self.toks[(self.i + n).min(self.toks.len() - 1)].tok
    }
    fn pos(&self) -> Pos {
        self.toks[self.i].pos
    }
    fn advance(&mut self) -> Token {
        let t = self.toks[self.i].clone();
        if self.i < self.toks.len() - 1 {
            self.i += 1;
        }
        t
    }
    fn is_word(&self, w: &str) -> bool {
        matches!(self.peek(), Tok::Ident(s) if s == w)
    }
    fn word_at(&self, n: usize) -> Option<&str> {
        match self.peek_at(n) {
            Tok::Ident(s) => Some(s.as_str()),
            _ => None,
        }
    }
    fn eat(&mut self, t: &Tok) -> bool {
        if self.peek() == t {
            self.advance();
            true
        } else {
            false
        }
    }
    fn eat_word(&mut self, w: &str) -> bool {
        if self.is_word(w) {
            self.advance();
            true
        } else {
            false
        }
    }
    fn expect(&mut self, t: Tok, what: &str) -> R<()> {
        if self.eat(&t) {
            Ok(())
        } else {
            Err(self.expected(what))
        }
    }
    fn expect_word(&mut self, w: &str) -> R<()> {
        if self.eat_word(w) {
            Ok(())
        } else {
            Err(self.expected(&format!("'{w}'")))
        }
    }
    fn expected(&self, what: &str) -> Diagnostic {
        let found = describe(self.peek());
        let code = match (what, self.peek()) {
            (_, Tok::Eof) => "E_EOF",
            ("';'", _) => "E_SEMICOLON",
            _ => "E_SYNTAX",
        };
        Diagnostic::new(self.pos(), code, format!("expected {what}, found {found}"))
    }
    fn enter(&mut self) -> R<()> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(Diagnostic::new(
                self.pos(),
                "E_NESTING",
                format!("nesting deeper than {MAX_DEPTH} levels"),
            ));
        }
        self.grow(1)
    }
    fn leave(&mut self) {
        self.depth -= 1;
        self.height -= 1;
    }
    /// Account for `n` more levels of tree height (one chain link or arm).
    fn grow(&mut self, n: u32) -> R<()> {
        self.height += n;
        if self.height > MAX_TREE_HEIGHT {
            return Err(Diagnostic::new(
                self.pos(),
                "E_NESTING",
                format!("expression or elif chain deeper than {MAX_TREE_HEIGHT} levels"),
            ));
        }
        Ok(())
    }

    fn ident(&mut self, what: &str) -> R<(String, Pos)> {
        let pos = self.pos();
        match self.peek().clone() {
            Tok::Ident(s) => {
                if is_reserved(&s) {
                    return Err(Diagnostic::new(
                        pos,
                        "E_RESERVED",
                        format!("'{s}' is a reserved word and cannot be used as {what}"),
                    ));
                }
                self.advance();
                Ok((s, pos))
            }
            _ => Err(self.expected(what)),
        }
    }

    // ---------------------------------------------------------------- types

    fn is_type_word(&self, n: usize) -> bool {
        matches!(
            self.word_at(n),
            Some("int" | "bool" | "boolean" | "void" | "float" | "f64" | "string" | "String")
        ) || self.word_at(n).is_some_and(|w| UNSUPPORTED_TYPES.contains(&w))
    }

    fn parse_type(&mut self) -> R<Type> {
        self.parse_type_with_return_marker(false)
    }

    fn parse_type_with_return_marker(&mut self, allow_return_marker: bool) -> R<Type> {
        let pos = self.pos();
        let name = match self.peek() {
            Tok::Ident(s) => s.clone(),
            _ => return Err(self.expected("a type")),
        };
        self.advance();
        let ty = match name.as_str() {
            "int" => Type::Int,
            "Option" => {
                self.expect(Tok::Lt, "'<' after Option")?;
                let inner = self.parse_type()?;
                self.expect(Tok::Gt, "'>' after Option payload")?;
                match inner {
                    Type::Int => Type::OptionInt,
                    Type::ResultIntStr => Type::OptionResultIntStr,
                    _ => return Err(Diagnostic::new(pos, "E_UNSUPPORTED_TYPE",
                        "browser ADT admits only Option<int> and Option<Result<int,string>>")),
                }
            }
            "Generator" => {
                self.expect(Tok::Lt, "'<' after Generator")?;
                let inner = self.parse_type()?;
                self.expect(Tok::Gt, "'>' after Generator item type")?;
                if inner != Type::Int {
                    return Err(Diagnostic::new(pos, "E_UNSUPPORTED_TYPE",
                        "the browser generator frontend currently recognizes only Generator<int>"));
                }
                Type::GeneratorInt
            }
            "Result" => {
                self.expect(Tok::Lt, "'<' after Result")?;
                let good = self.parse_type()?;
                self.expect(Tok::Comma, "',' between Result type arguments")?;
                let bad = self.parse_type()?;
                self.expect(Tok::Gt, "'>' after Result types")?;
                if good != Type::Int || bad != Type::Str {
                    return Err(Diagnostic::new(pos, "E_UNSUPPORTED_TYPE",
                        "browser ADT v1 admits only Result<int,string>"));
                }
                Type::ResultIntStr
            },
            "bool" | "boolean" => Type::Bool,
            "void" => Type::Void,
            "float" | "f64" => Type::Float,
            "string" | "String" => Type::Str,
            n if UNSUPPORTED_TYPES.contains(&n) => {
                return Err(Diagnostic::new(
                    pos,
                    "E_UNSUPPORTED_TYPE",
                    format!("type '{n}' is not supported by the browser compiler yet"),
                ))
            }
            "ActorRef" => {
                self.expect(Tok::Lt, "'<' after ActorRef")?;
                let (actor, apos) = self.ident("an actor name")?;
                self.expect(Tok::Gt, "'>'")?;
                Type::NamedRef(self.type_name(actor, apos))
            }
            "ActorMail" => {
                return Err(Diagnostic::new(
                    pos,
                    "E_TYPE",
                    "ActorMail<T> is only the parameter type of an actor receive handler",
                ))
            }
            n if is_reserved(n) => return Err(Diagnostic::new(pos, "E_UNKNOWN_TYPE", format!("unknown type '{n}'"))),
            // A class or enum name; the checker resolves it.
            n => Type::Named(self.type_name(n.to_string(), pos)),
        };
        // Type-position '?' is Option sugar, never formal omission or
        // expression propagation. Reuse the existing executable ADT ABI only.
        let ty = if self.eat(&Tok::Question) {
            match ty {
                Type::Int => Type::OptionInt,
                Type::ResultIntStr => Type::OptionResultIntStr,
                _ => return Err(Diagnostic::new(pos, "E_UNSUPPORTED_TYPE",
                    "type suffix '?' requires a supported Option payload in this backend")),
            }
        } else { ty };
        let return_marker = allow_return_marker && *self.peek() == Tok::Lt
            && *self.peek_at(1) == Tok::Minus;
        if !return_marker && matches!(self.peek(), Tok::Lt | Tok::LBracket | Tok::Question | Tok::Pipe) {
            return Err(unsupported(self.pos(), "generic, nested optional, union and sequence types"));
        }
        Ok(ty)
    }

    fn type_name(&mut self, name: String, pos: Pos) -> u32 {
        self.type_names.push((name, pos));
        (self.type_names.len() - 1) as u32
    }

    // ------------------------------------------------------------ top level

    /// Only the attached scalar value lowerer uses this target binding parser.
    /// Compiler-generated copy marker cannot be spelled as a source identifier.
    pub(crate) fn parse_struct_value_program(&mut self) -> R<(Program, Vec<Pos>)> {
        self.struct_target = true;
        self.struct_value_bindings = Some(Vec::new());
        let result = self.parse_program();
        self.struct_target = false;
        let bindings = self.struct_value_bindings.take().unwrap_or_default();
        Ok((result?, bindings))
    }

    pub fn parse_program(&mut self) -> R<Program> {
        let mut prog = Program::default();
        let mut decls = 0usize;
        loop {
            if self.is_word("const") && self.word_at(1) == Some("Pattern") {
                self.parse_pattern_constant()?;
            } else if self.is_word("type") && matches!(self.peek_at(2), Tok::Assign)
                && self.word_at(3) != Some("struct") {
                self.parse_pattern_type_alias()?;
            } else { break; }
        }
        while *self.peek() != Tok::Eof {
            if decls >= MAX_FUNCS {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_SOURCE_LIMIT",
                    format!("more than {MAX_FUNCS} top-level declarations"),
                ));
            }
            decls += 1;
            let public = self.is_word("pub");
            let at = usize::from(public);
            let start = self.pos();
            match (self.word_at(at), self.word_at(at + 1)) {
                (Some("define"), Some("class")) => {
                    self.i += at + 2;
                    prog.classes.push(self.parse_class(public)?);
                }
                (Some("define"), Some("actor")) => {
                    if public {
                        return Err(unsupported(start, "'pub' actors"));
                    }
                    self.i += 2;
                    let decl = self.parse_actor(true)?;
                    prog.actors.push(decl);
                }
                (Some("actor"), Some(w)) if w != "fnc" => {
                    if public {
                        return Err(unsupported(start, "'pub' actors"));
                    }
                    self.i += 1;
                    let decl = self.parse_actor(false)?;
                    prog.actors.push(decl);
                }
                (Some("define"), Some("enum")) => {
                    if public {
                        return Err(unsupported(start, "'pub' enums"));
                    }
                    self.i += 2;
                    prog.enums.push(self.parse_enum()?);
                }
                (Some("define"), Some(w)) => {
                    let what = match w {
                        "abstract" => "abstract classes".to_string(),
                        "pub" | "private" => {
                            "visibility after 'define' on classes, actors and enums (write 'pub define class')".to_string()
                        }
                        _ => format!("'define {w}' declarations"),
                    };
                    return Err(unsupported(self.pos(), &what));
                }
                _ => prog.funcs.push(self.parse_func()?),
            }
        }
        // Builtin spellings are not permission to erase nominal identity.
        for name in prog.classes.iter().map(|d| &d.name)
            .chain(prog.enums.iter().map(|d| &d.name))
            .chain(prog.actors.iter().map(|d| &d.name)) {
            if let Some(pos) = self.pattern_type_spellings.get(name) {
                return Err(Diagnostic::new(*pos, "E_PATTERN_TYPE",
                    "scalar pattern type spelling is shadowed by a nominal declaration"));
            }
            if name == "Pattern" {
                return Err(Diagnostic::new(self.pos(), "E_PATTERN_TYPE",
                    "Pattern is a compiler-owned compile-time category"));
            }
        }
        // The initial file-scope profile prohibits runtime escape and
        // shadowing rather than guessing at lexical binding identity.
        for (i, token) in self.toks.iter().enumerate() {
            if let Tok::Ident(name) = &token.tok {
                if self.pattern_type_aliases.contains_key(name) && !self.pattern_alias_uses.contains(&i) {
                    return Err(Diagnostic::new(token.pos, "E_PATTERN_TYPE",
                        "scalar pattern alias used outside a resolved pattern type position"));
                }
                if self.pattern_constants.contains_key(name) && !self.pattern_name_uses.contains(&i) {
                    return Err(Diagnostic::new(token.pos, "E_PATTERN_ESCAPE",
                        "compile-time Pattern name cannot escape or be shadowed"));
                }
            }
        }
        prog.type_names = std::mem::take(&mut self.type_names);
        Ok(prog)
    }

    /// Parse a bounded scalar struct declaration and its grouped/chained
    /// attachments. This does not certify initialization, layout or guest execution.
    pub(crate) fn parse_struct_value_targets(&mut self) -> R<Vec<StructWithTarget>> {
        let mut decls = Vec::new();
        let mut owners = std::collections::BTreeSet::new();
        loop {
            if decls.len() >= 16 {
                return Err(Diagnostic::new(self.pos(), "E_SOURCE_LIMIT", "too many attached nominal owners"));
            }
            let decl = self.parse_struct_with_target_impl(true)?;
            if !owners.insert(decl.name.clone()) {
                return Err(Diagnostic::new(decl.pos, "E_DUPLICATE", "duplicate attached nominal owner"));
            }
            decls.push(decl);
            let next = (self.word_at(0) == Some("define") && self.word_at(1) == Some("struct")
                && self.word_at(2).is_some() && *self.peek_at(3) == Tok::LBrace)
                || (self.word_at(0) == Some("type") && self.word_at(1).is_some()
                    && *self.peek_at(2) == Tok::Assign && self.word_at(3) == Some("struct")
                    && *self.peek_at(4) == Tok::LBrace);
            if !next { break; }
        }
        Ok(decls)
    }

    pub fn parse_struct_with_target(&mut self) -> R<StructWithTarget> {
        self.parse_struct_with_target_impl(false)
    }

    fn parse_struct_with_target_impl(&mut self, allow_nominal: bool) -> R<StructWithTarget> {
        self.struct_target = true;
        let direct = self.eat_word("define");
        if direct { self.expect_word("struct")?; } else { self.expect_word("type")?; }
        let (name, pos) = self.ident("a struct name")?;
        if UNSUPPORTED_TYPES.contains(&name.as_str()) ||
            matches!(name.as_str(), "int" | "bool" | "boolean" | "float" | "f64" | "string" | "String" | "void") {
            return Err(Diagnostic::new(pos, "E_RESERVED_TYPE", "struct name is an intrinsic type"));
        }
        if !direct {
            self.expect(Tok::Assign, "'=' after struct name")?;
            self.expect_word("struct")?;
        }
        self.expect(Tok::LBrace, "'{' after struct")?;
        let mut fields: Vec<(String, Type)> = Vec::new();
        while !self.eat(&Tok::RBrace) {
            if fields.len() >= MAX_MEMBERS {
                return Err(Diagnostic::new(self.pos(), "E_SOURCE_LIMIT", "too many struct fields"));
            }
            let ty = self.parse_type()?;
            if !(matches!(ty, Type::Int | Type::Bool) || allow_nominal && matches!(ty, Type::Named(_))) {
                return Err(unsupported(self.pos(), "unsupported attached struct field type"));
            }
            let (field, at) = self.ident("a struct field name")?;
            if fields.iter().any(|(prior, _)| prior == &field) {
                return Err(Diagnostic::new(at, "E_DUPLICATE", "duplicate struct field"));
            }
            self.expect(Tok::Semi, "';' after struct field")?;
            fields.push((field, ty));
        }
        self.expect_word("with")?;
        let mut out = StructWithTarget {
            name, pos, fields, constructors: Vec::new(), methods: Vec::new(), mutable_methods: Vec::new(),
        };
        loop {
            if self.eat(&Tok::LBrace) {
                if *self.peek() == Tok::RBrace {
                    return Err(self.expected("a constructor or method inside with"));
                }
                while !self.eat(&Tok::RBrace) {
                    self.parse_struct_attachment_member(&mut out, allow_nominal)?;
                }
            } else {
                self.parse_struct_attachment_member(&mut out, allow_nominal)?;
            }
            if !self.eat_word("with") { break; }
        }
        self.eat(&Tok::Semi);
        self.struct_target = false;
        Ok(out)
    }

    /// A deliberately bounded straight-line scalar constructor proof. Do not
    /// certify arbitrary calls, control flow or effects until the value ABI
    /// and full ownership checker can establish those contracts.
    pub(crate) fn prove_struct_constructor(
        fields: &[(String, Type)], params: &[Param], body: &[Stmt], pos: Pos,
    ) -> R<()> {
        use std::collections::{BTreeMap, BTreeSet};
        fn scalar(
            e: &Expr, fields: &BTreeMap<&str, Type>, params: &BTreeMap<&str, Type>,
            initialized: &BTreeSet<&str>,
        ) -> R<Type> {
            let invalid = || Diagnostic::new(e.pos, "E_STRUCT_INIT",
                "constructor expression is not a proven initialized scalar");
            match &e.kind {
                ExprKind::Int(_) => Ok(Type::Int),
                ExprKind::Bool(_) => Ok(Type::Bool),
                ExprKind::Var(n) => params.get(n.as_str()).copied().ok_or_else(invalid),
                ExprKind::Member(base, n) if matches!(&base.kind, ExprKind::SelfRef) => {
                    if !initialized.contains(n.as_str()) {
                        return Err(Diagnostic::new(e.pos, "E_STRUCT_INIT",
                            "read of an uninitialized struct field"));
                    }
                    fields.get(n.as_str()).copied().ok_or_else(invalid)
                }
                ExprKind::Unary(op, inner) => {
                    let t = scalar(inner, fields, params, initialized)?;
                    match (op, t) {
                        (UnOp::Neg | UnOp::Plus, Type::Int) => Ok(Type::Int),
                        (UnOp::Not, Type::Bool) => Ok(Type::Bool),
                        _ => Err(invalid()),
                    }
                }
                ExprKind::Binary(op, l, r) => {
                    let l = scalar(l, fields, params, initialized)?;
                    let r = scalar(r, fields, params, initialized)?;
                    match (op, l, r) {
                        (BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div, Type::Int, Type::Int) => Ok(Type::Int),
                        (BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge, Type::Int, Type::Int) => Ok(Type::Bool),
                        (BinOp::Eq | BinOp::Ne, Type::Int, Type::Int) |
                        (BinOp::Eq | BinOp::Ne | BinOp::And | BinOp::Or, Type::Bool, Type::Bool) => Ok(Type::Bool),
                        _ => Err(invalid()),
                    }
                }
                ExprKind::Ternary(cond, yes, no) => {
                    let c = scalar(cond, fields, params, initialized)?;
                    let y = scalar(yes, fields, params, initialized)?;
                    let n = scalar(no, fields, params, initialized)?;
                    if c == Type::Bool && y == n { Ok(y) } else { Err(invalid()) }
                }
                _ => Err(invalid()),
            }
        }
        // Each reachable fallthrough path must initialize the same fields.
        // A missing else contributes the unchanged incoming state. Reject
        // differing branch states rather than speculate on later conditions.
        fn block<'a>(
            body: &'a [Stmt], fields: &BTreeMap<&str, Type>, params: &BTreeMap<&str, Type>,
            initialized: &mut BTreeSet<&'a str>, pos: Pos,
        ) -> R<()> {
            for stmt in body {
                match stmt {
                    Stmt::FieldAssign { target, field, value, pos: at }
                        if matches!(&target.kind, ExprKind::SelfRef) => {
                        let expected = fields.get(field.as_str()).copied().ok_or_else(||
                            Diagnostic::new(*at, "E_STRUCT_INIT", "unknown struct field"))?;
                        if initialized.contains(field.as_str()) {
                            return Err(Diagnostic::new(*at, "E_STRUCT_INIT", "struct field initialized twice"));
                        }
                        if scalar(value, fields, params, initialized)? != expected {
                            return Err(Diagnostic::new(*at, "E_STRUCT_INIT", "struct field initializer type mismatch"));
                        }
                        initialized.insert(field.as_str());
                    }
                    Stmt::If { arms, otherwise, pos: at } => {
                        let mut joined = None;
                        for (condition, branch) in arms {
                            if scalar(condition, fields, params, initialized)? != Type::Bool {
                                return Err(Diagnostic::new(*at, "E_STRUCT_INIT", "constructor condition must be bool"));
                            }
                            let mut state = initialized.clone();
                            block(branch, fields, params, &mut state, *at)?;
                            if joined.as_ref().is_some_and(|prior| prior != &state) {
                                return Err(Diagnostic::new(*at, "E_STRUCT_INIT", "constructor branches initialize different fields"));
                            }
                            joined = Some(state);
                        }
                        let mut state = initialized.clone();
                        if let Some(other) = otherwise {
                            block(other, fields, params, &mut state, *at)?;
                        }
                        if joined.as_ref().is_some_and(|prior| prior != &state) {
                            return Err(Diagnostic::new(*at, "E_STRUCT_INIT", "constructor branches initialize different fields"));
                        }
                        *initialized = state;
                    }
                    _ => return Err(unsupported(pos, "constructor requires scalar assignments and bounded if branches")),
                }
            }
            Ok(())
        }
        let field_types: BTreeMap<&str, Type> = fields.iter().map(|(n,t)| (n.as_str(), *t)).collect();
        let param_types: BTreeMap<&str, Type> = params.iter().map(|p| (p.name.as_str(), p.ty)).collect();
        let mut initialized = BTreeSet::new();
        block(body, &field_types, &param_types, &mut initialized, pos)?;
        if initialized.len() != fields.len() {
            return Err(Diagnostic::new(pos, "E_STRUCT_INIT", "constructor leaves required fields uninitialized"));
        }
        Ok(())
    }

    fn parse_struct_attachment_member(&mut self, out: &mut StructWithTarget, allow_nominal: bool) -> R<()> {
        let pos = self.pos();
        if out.methods.len() + out.constructors.len() >= MAX_MEMBERS {
            return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "too many attached struct members"));
        }
        let public = self.eat_word("pub");
        if !public { self.eat_word("private"); }
        let mutable_receiver = self.eat_word("mut");
        let scalar_params = |ps: &[Param]| ps.iter().all(|p| matches!(p.ty, Type::Int | Type::Bool));
        let same_params = |a: &[Param], b: &[Param]| a.iter().map(|p| p.ty).eq(b.iter().map(|p| p.ty));
        if self.eat_word("constructor") {
            if mutable_receiver {
                return Err(Diagnostic::new(pos, "E_SYNTAX", "constructor cannot have a mutable receiver modifier"));
            }
            self.expect(Tok::LParen, "'(' after constructor")?;
            let params = self.parse_params(false)?;
            if !(scalar_params(&params) || allow_nominal && params.iter().all(|p| matches!(p.ty, Type::Int | Type::Bool | Type::Named(_)))) {
                return Err(unsupported(pos, "unsupported attached constructor parameters"));
            }
            if out.constructors.iter().any(|(_, ctor)| same_params(&ctor.params, &params)) {
                return Err(Diagnostic::new(pos, "E_DUPLICATE", "duplicate struct constructor signature"));
            }
            let body = self.parse_block()?;
            if !allow_nominal || (scalar_params(&params) && out.fields.iter().all(|(_, t)| matches!(t, Type::Int | Type::Bool))) {
                Self::prove_struct_constructor(&out.fields, &params, &body, pos)?;
            }
            out.constructors.push((public, Constructor { pos, params, body }));
        } else if self.eat_word("mtd") {
            let prefixed = if self.return_first_marker_here() {
                // The bounded profile accepts scalar return types only.
                // Consume the scalar token before the contextual <- marker,
                // without letting the generic-type suffix parser see '<'.
                let ty = match self.word_at(0) {
                    Some("int") => Type::Int,
                    Some("bool" | "boolean") => Type::Bool,
                    Some("void") => Type::Void,
                    _ => return Err(unsupported(self.pos(), "non-scalar attached method return type")),
                };
                self.advance();
                self.consume_return_first_marker()?;
                Some(ty)
            } else { None };
            let (name, pos) = self.ident("a method name")?;
            self.expect(Tok::LParen, "'(' after method name")?;
            let params = self.parse_params(false)?;
            let ret = if let Some(ty) = prefixed {
                if matches!(self.peek(), Tok::Colon | Tok::Arrow) {
                    return Err(Diagnostic::new(pos, "E_SYNTAX", "duplicate return type"));
                }
                ty
            } else {
                if !(self.eat(&Tok::Colon) || self.eat(&Tok::Arrow)) {
                    return Err(self.expected("':' or '->' before attached method return type"));
                }
                self.parse_type()?
            };
            if !scalar_params(&params) || !matches!(ret, Type::Int | Type::Bool | Type::Void) {
                return Err(unsupported(pos, "non-scalar attached method signature"));
            }
            if out.methods.iter().any(|m| m.name == name && same_params(&m.params, &params)) {
                return Err(Diagnostic::new(pos, "E_DUPLICATE", "duplicate attached method signature"));
            }
            let body = self.parse_block()?;
            if mutable_receiver { out.mutable_methods.push(out.methods.len()); }
            out.methods.push(Method { name, pos, public, is_static: false, params, ret, body });
        } else {
            return Err(self.expected("an attached constructor or mtd"));
        }
        Ok(())
    }

    /// After `define class`: `Name as { class_member } end`.
    fn parse_class(&mut self, public: bool) -> R<ClassDecl> {
        let (name, pos) = self.ident("a class name")?;
        self.decl_head("class")?;
        self.expect_word("as")?;
        let m = self.parse_members(&name, false, "end")?;
        Ok(ClassDecl {
            name,
            pos,
            public,
            fields: m.fields,
            ctor: m.ctor,
            methods: m.methods,
        })
    }

    /// After `define actor` (`defined`) or a bare `actor`.
    fn parse_actor(&mut self, defined: bool) -> R<ActorDecl> {
        let (name, pos) = self.ident("an actor name")?;
        self.decl_head("actor")?;
        // The existing executable ActorMail<T> handler determines the actual
        // mailbox type. A v1 receives T header may only make that fact
        // explicit, never widen it or create an unverified second ingress.
        let declared_receives = if self.eat_word("receives") {
            if self.is_word("never") {
                return Err(Diagnostic::new(
                    self.pos(), "E_UNSUPPORTED",
                    "receives never requires a separately authenticated CONTROL-only runtime",
                ));
            }
            Some(self.parse_type()?)
        } else {
            None
        };
        // The sends type is an upper bound on outbound DATA payloads; the
        // typed checker audits all reachable statically resolved send sites.
        // Never is a distinct application-DATA effect, NOT Type::Void.
        // An omitted clause still means inferred effect, not Any.
        let sends = if self.eat_word("sends") {
            if self.eat_word("never") {
                Some(ActorSendEffect::Never)
            } else {
                let ty = self.parse_type()?;
                if ty == Type::Void {
                    return Err(Diagnostic::new(self.pos(), "E_ACTOR_SEND_CONTRACT",
                        "sends void is invalid; use sends never for an empty application effect"));
                }
                Some(ActorSendEffect::Payload(ty))
            }
        } else {
            None
        };
        if self.is_word("receives") || self.is_word("sends") {
            return Err(Diagnostic::new(self.pos(), "E_ACTOR_CONTRACT",
                "duplicate or out-of-order actor contract clause"));
        }
        // Pipeline emits is a different effect and has no checked lowering.
        if self.is_word("emits") {
            return Err(Diagnostic::new(self.pos(), "E_UNSUPPORTED",
                "actor emits pipeline effect has no checked lowering"));
        }
        let close = if defined {
            self.expect_word("as")?;
            "end"
        } else if self.eat(&Tok::LBrace) {
            "}"
        } else {
            "end"
        };
        let m = self.parse_members(&name, true, close)?;
        if let Some(expected) = declared_receives {
            match &m.receive {
                Some(handler) if handler.payload == expected => {}
                Some(_) => return Err(Diagnostic::new(
                    pos, "E_ACTOR_RECEIVES", "actor receives header disagrees with concrete ActorMail<T>",
                )),
                None => return Err(Diagnostic::new(
                    pos, "E_ACTOR_RECEIVES", "typed actor requires a matching receive(ActorMail<T>) handler",
                )),
            }
        }
        Ok(ActorDecl {
            name,
            pos,
            receives: declared_receives,
            sends,
            fields: m.fields,
            receive: m.receive,
            methods: m.methods,
        })
    }

    /// Generic parameters and inheritance clauses are outside the subset.
    fn decl_head(&mut self, kind: &str) -> R<()> {
        if *self.peek() == Tok::Lt {
            return Err(unsupported(self.pos(), &format!("generic {kind}es")));
        }
        if let Some(w @ ("extends" | "implements" | "impl" | "conforms")) = self.word_at(0) {
            return Err(unsupported(self.pos(), &format!("'{w}' on a {kind}")));
        }
        Ok(())
    }

    /// After `define enum`: `Name as Variant { Variant } end`.
    fn parse_enum(&mut self) -> R<EnumDecl> {
        let (name, pos) = self.ident("an enum name")?;
        self.decl_head("enum")?;
        self.expect_word("as")?;
        let mut variants: Vec<(String, Pos)> = Vec::new();
        while !self.eat_word("end") {
            if variants.len() >= MAX_MEMBERS {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_SOURCE_LIMIT",
                    format!("more than {MAX_MEMBERS} enum variants"),
                ));
            }
            let (v, vpos) = self.ident("an enum variant or 'end'")?;
            match self.peek() {
                Tok::LParen => return Err(unsupported(self.pos(), "enum variants with payloads")),
                Tok::Assign => return Err(unsupported(self.pos(), "enum variants with explicit values")),
                Tok::Comma => return Err(self.expected("an enum variant on its own line")),
                _ => {}
            }
            if variants.iter().any(|(w, _)| *w == v) {
                return Err(Diagnostic::new(
                    vpos,
                    "E_DUPLICATE",
                    format!("duplicate variant '{v}' in enum '{name}'"),
                ));
            }
            variants.push((v, vpos));
        }
        if variants.is_empty() {
            return Err(Diagnostic::new(
                pos,
                "E_SYNTAX",
                format!("enum '{name}' needs at least one variant"),
            ));
        }
        Ok(EnumDecl { name, pos, variants })
    }

    fn at_close(&self, close: &str) -> bool {
        if close == "}" {
            *self.peek() == Tok::RBrace
        } else {
            self.is_word(close)
        }
    }

    /// Class or actor members up to and including `close` (`end` or `}`).
    fn parse_members(&mut self, owner: &str, actor: bool, close: &str) -> R<Members> {
        let kind = if actor { "actor" } else { "class" };
        let mut m = Members::default();
        let mut count = 0usize;
        loop {
            if self.at_close(close) {
                self.advance();
                break;
            }
            if *self.peek() == Tok::Eof {
                return Err(self.expected(&format!("'{close}' to close {kind} '{owner}'")));
            }
            count += 1;
            if count > MAX_MEMBERS {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_SOURCE_LIMIT",
                    format!("more than {MAX_MEMBERS} members in {kind} '{owner}'"),
                ));
            }
            if *self.peek() == Tok::At {
                return Err(unsupported(self.pos(), "annotations"));
            }
            let mpos = self.pos();
            let public = if self.eat_word("pub") {
                true
            } else {
                self.eat_word("private");
                false
            };
            // Admit the inert susp marker on an actor's attached receive
            // entrypoint. Any actually suspended body must still be rejected.
            if actor && self.is_word("suspends") && self.word_at(1) == Some("receive")
                && *self.peek_at(2) == Tok::LParen
            {
                self.advance();
            }
            let word = self.word_at(0).map(str::to_owned);
            match word.as_deref() {
                Some("static") => {
                    if actor {
                        return Err(unsupported(self.pos(), "static functions in actors"));
                    }
                    self.advance();
                    if let Some(w @ ("async" | "structural")) = self.word_at(0) {
                        return Err(unsupported(self.pos(), &format!("'{w}' static functions")));
                    }
                    self.expect_word("fnc")?;
                    let method = self.parse_method(public, true)?;
                    m.methods.push(method);
                }
                Some("constructor") => {
                    if actor {
                        return Err(Diagnostic::new(
                            self.pos(),
                            "E_ACTOR",
                            "actors do not declare constructors; initialize actor-local state with fields",
                        ));
                    }
                    if m.ctor.is_some() {
                        return Err(Diagnostic::new(
                            self.pos(),
                            "E_DUPLICATE",
                            format!("class '{owner}' may declare at most one constructor"),
                        ));
                    }
                    self.advance();
                    self.expect(Tok::LParen, "'('")?;
                    let params = self.parse_params(false)?;
                    let body = self.parse_block()?;
                    m.ctor = Some(Constructor { pos: mpos, params, body });
                }
                Some("final") => {
                    self.advance();
                    if matches!(self.peek(), Tok::Ident(_)) && *self.peek_at(1) == Tok::Colon {
                        let f = self.parse_named_field(public, actor)?;
                        m.fields.push(f);
                    } else if actor {
                        return Err(self.expected("'name: type' after 'final' in an actor"));
                    } else {
                        self.eat_word("mut");
                        let f = self.parse_type_first_field(public, FieldMode::Final)?;
                        m.fields.push(f.field);
                    }
                }
                Some("mut") if !actor => {
                    self.advance();
                    let f = self.parse_type_first_field(public, FieldMode::Constructor)?;
                    m.fields.push(f.field);
                }
                Some(w @ ("let" | "val" | "const")) => {
                    let mode = match (w, actor) {
                        ("let", true) => FieldMode::ActorState,
                        ("let", false) => FieldMode::Constructor,
                        _ => FieldMode::Final,
                    };
                    self.advance();
                    if self.is_word("mut") && w != "val" {
                        self.advance();
                    }
                    if matches!(self.peek(), Tok::Ident(_)) && *self.peek_at(1) == Tok::Colon {
                        return Err(self.expected("a field type before the field name"));
                    }
                    let f = self.parse_type_first_field(public, mode)?;
                    if f.init_was_colon {
                        return Err(Diagnostic::new(
                            f.field.init.pos,
                            "E_SYNTAX",
                            "a 'let'/'val'/'const' field initializer uses '='",
                        ));
                    }
                    m.fields.push(f.field);
                }
                Some("fnc") => {
                    self.advance();
                    let method = self.parse_method(public, false)?;
                    m.methods.push(method);
                }
                Some("method")
                    if matches!(self.peek_at(1), Tok::Ident(_))
                        && matches!(self.peek_at(2), Tok::LParen | Tok::Lt) =>
                {
                    return Err(Diagnostic::new(self.pos(), "E_SYNTAX", "obsolete method introducer; use mtd"));
                }
                Some("mtd")
                    if matches!(self.peek_at(1), Tok::Ident(_))
                        && matches!(self.peek_at(2), Tok::LParen | Tok::Lt) =>
                {
                    // Contextual marker: a bare mtd() or method() is a name.
                    self.advance();
                    let method = self.parse_method(public, false)?;
                    m.methods.push(method);
                }
                Some(w @ ("async" | "suspend" | "suspends" | "structural" | "abstract" | "nlex" | "shared")) => {
                    return Err(unsupported(self.pos(), &format!("'{w}' members")));
                }
                Some("receive") if actor && *self.peek_at(1) == Tok::LParen => {
                    if public {
                        return Err(Diagnostic::new(
                            mpos,
                            "E_ACTOR",
                            format!("receive handler '{owner}.receive' is actor-local and cannot be pub"),
                        ));
                    }
                    if m.receive.is_some() {
                        return Err(Diagnostic::new(
                            self.pos(),
                            "E_DUPLICATE",
                            format!("actor '{owner}' already declares receive"),
                        ));
                    }
                    let r = self.parse_receive(owner)?;
                    m.receive = Some(r);
                }
                Some(_) if *self.peek_at(1) == Tok::LParen => {
                    let method = self.parse_method(public, false)?;
                    m.methods.push(method);
                }
                Some(_) if *self.peek_at(1) == Tok::Colon && !actor => {
                    let f = self.parse_named_field(public, actor)?;
                    m.fields.push(f);
                }
                Some(_) if matches!(self.peek_at(1), Tok::Ident(_)) && !actor => {
                    let f = self.parse_type_first_field(public, FieldMode::Constructor)?;
                    m.fields.push(f.field);
                }
                _ => {
                    return Err(self.expected(if actor {
                        "an actor member (field, method or receive)"
                    } else {
                        "a class member (field, constructor or method)"
                    }))
                }
            }
        }
        Ok(m)
    }

    /// `name: type [= expr]` (immutable field).
    fn parse_named_field(&mut self, public: bool, _actor: bool) -> R<Field> {
        let (name, pos) = self.ident("a field name")?;
        self.expect(Tok::Colon, "':'")?;
        let ty = self.parse_type()?;
        if !self.eat(&Tok::Assign) {
            return Err(unsupported(self.pos(), "fields without an initializer"));
        }
        let init = self.parse_expr()?;
        self.terminator()?;
        Ok(Field {
            name,
            pos,
            ty,
            public,
            mode: FieldMode::Final,
            init,
        })
    }

    /// `type name (":" | "=") expr`.
    fn parse_type_first_field(&mut self, public: bool, mode: FieldMode) -> R<TypeFirstField> {
        let ty = self.parse_type()?;
        let (name, pos) = self.ident("a field name")?;
        let init_was_colon = if self.eat(&Tok::Colon) {
            true
        } else if self.eat(&Tok::Assign) {
            false
        } else {
            return Err(unsupported(self.pos(), "fields without an initializer"));
        };
        let init = self.parse_expr()?;
        self.terminator()?;
        Ok(TypeFirstField {
            field: Field {
                name,
                pos,
                ty,
                public,
                mode,
                init,
            },
            init_was_colon,
        })
    }

    /// Method head after any `fnc`/`method`/`static fnc` keyword.
    fn parse_method(&mut self, public: bool, is_static: bool) -> R<Method> {
        let (name, pos) = self.ident("a method name")?;
        if *self.peek() == Tok::Lt {
            return Err(unsupported(self.pos(), "generic methods"));
        }
        self.expect(Tok::LParen, "'('")?;
        if self.is_word("self") {
            return Err(unsupported(self.pos(), "explicit 'self' receivers"));
        }
        let params = self.parse_params(false)?;
        if *self.peek() == Tok::LParen {
            return Err(unsupported(self.pos(), "curried method parameter lists"));
        }
        let ret = self.parse_return_spec()?;
        if *self.peek() != Tok::LBrace {
            return Err(unsupported(self.pos(), "methods without a body"));
        }
        let body = self.parse_block()?;
        Ok(Method {
            name,
            pos,
            public,
            is_static,
            params,
            ret,
            body,
        })
    }

    /// `receive ( ActorMail < T > name ) [ : void ] block`.
    fn parse_receive(&mut self, owner: &str) -> R<Receive> {
        let pos = self.pos();
        self.advance();
        self.expect(Tok::LParen, "'('")?;
        if !self.is_word("ActorMail") {
            return Err(Diagnostic::new(
                self.pos(),
                "E_TYPE",
                format!("actor mailbox handler '{owner}.receive' parameter must be ActorMail<T>"),
            ));
        }
        self.advance();
        if *self.peek() != Tok::Lt {
            return Err(Diagnostic::new(
                self.pos(),
                "E_TYPE",
                "ActorMail requires an explicit payload type, for example ActorMail<int>",
            ));
        }
        self.advance();
        let payload = self.parse_type()?;
        self.expect(Tok::Gt, "'>'")?;
        let (mail, _) = self.ident("a mail parameter name")?;
        if *self.peek() == Tok::Comma {
            return Err(Diagnostic::new(
                self.pos(),
                "E_TYPE",
                format!("actor mailbox handler '{owner}.receive' takes exactly one ActorMail<T> parameter"),
            ));
        }
        self.expect(Tok::RParen, "')'")?;
        if self.parse_return_spec()? != Type::Void {
            return Err(Diagnostic::new(
                pos,
                "E_TYPE",
                format!("actor mailbox handler '{owner}.receive' must return void"),
            ));
        }
        let body = self.parse_block()?;
        Ok(Receive { pos, mail, payload, body })
    }

    /// grammar.ebnf: `return_spec = [ ( ":" | "->" ) type_ref ]`.
    fn parse_return_spec(&mut self) -> R<Type> {
        if self.eat(&Tok::Colon) || self.eat(&Tok::Arrow) {
            self.parse_type()
        } else {
            Ok(Type::Void)
        }
    }

    /// Parameters after `(` up to and including `)`.
    fn parse_params(&mut self, allow_omittable: bool) -> R<Vec<Param>> {
        let mut params: Vec<Param> = Vec::new();
        if *self.peek() != Tok::RParen {
            loop {
                if params.len() >= MAX_PARAMS {
                    return Err(Diagnostic::new(
                        self.pos(),
                        "E_SOURCE_LIMIT",
                        format!("more than {MAX_PARAMS} parameters"),
                    ));
                }
                let p = self.parse_param(allow_omittable)?;
                if params.iter().any(|q| q.name == p.name) {
                    return Err(Diagnostic::new(
                        p.pos,
                        "E_DUPLICATE",
                        format!("duplicate parameter name '{}'", p.name),
                    ));
                }
                params.push(p);
                if !self.eat(&Tok::Comma) {
                    break;
                }
            }
        }
        self.expect(Tok::RParen, "')'")?;
        Ok(params)
    }

    /// The restricted browser frontend accepts scalar return-first headers.
    /// The marker is contextual; a separated '< -' is *not* a declaration.
    fn return_first_marker_here(&self) -> bool {
        if !matches!(self.peek(), Tok::Ident(_)) {
            return false;
        }
        if *self.peek_at(1) == Tok::Bullet {
            return true;
        }
        if *self.peek_at(1) == Tok::Lt && *self.peek_at(2) == Tok::Minus {
            let left = self.toks[(self.i + 1).min(self.toks.len() - 1)].pos;
            let right = self.toks[(self.i + 2).min(self.toks.len() - 1)].pos;
            return left.line == right.line && right.col == left.col + 1;
        }
        false
    }

    fn consume_return_first_marker(&mut self) -> R<()> {
        if self.eat(&Tok::Bullet) {
            return Ok(());
        }
        if *self.peek() == Tok::Lt && *self.peek_at(1) == Tok::Minus {
            let lt = self.pos();
            let minus = self.toks[(self.i + 1).min(self.toks.len() - 1)].pos;
            if lt.line != minus.line || minus.col != lt.col + 1 {
                return Err(Diagnostic::new(minus, "E_SYNTAX",
                    "return-first '<-' must be contiguous"));
            }
            self.advance();
            self.advance();
            return Ok(());
        }
        Err(self.expected("return-first '<-' or U+2022 '•'"))
    }

    fn parse_return_first_body(&mut self) -> R<Vec<Stmt>> {
        if *self.peek() == Tok::LBrace {
            return self.parse_block();
        }
        // Explicit begin/end and bare statements/end use ordinary statement
        // semantics, with no implicit return from the last expression.
        self.eat_word("begin");
        let statements = self.parse_stmts_until(&["end"])?;
        self.expect_word("end")?;
        self.eat(&Tok::Dot);
        self.eat(&Tok::Semi);
        Ok(statements)
    }

    fn parse_func(&mut self) -> R<Func> {
        if *self.peek() == Tok::At {
            return Err(unsupported(self.pos(), "annotations"));
        }
        let start = self.pos();
        self.eat_word("pub");
        // Header-level susp does not make unsupported await/resume code legal.
        let _susp = self.eat_word("suspends");
        if self.is_word("private") {
            return Err(unsupported(self.pos(), "'private' visibility"));
        }
        for m in [
            "async",
            "suspend",
            "suspends",
            "nlex",
            "abstract",
            "structural",
            "static",
            "generator",
        ] {
            if self.is_word(m) {
                return Err(unsupported(self.pos(), &format!("'{m}' callables")));
            }
        }
        let named_keyword = self.eat_word("fnc") || self.eat_word("routine");
        // Return-first is a declaration-only context. Keep '<' and '-' as
        // independent expression tokens: 1<-2 remains 1 < -2.
        let return_first = self.return_first_marker_here();
        if !named_keyword && !return_first {
            if let Some(w) = self.word_at(0) {
                if UNSUPPORTED_STMT.contains(&w) || matches!(w, "val" | "let" | "const") {
                    return Err(unsupported(self.pos(), &format!("top-level '{w}'")));
                }
            }
            return Err(Diagnostic::new(
                self.pos(),
                "E_TOPLEVEL",
                format!("expected a 'fnc' or return-first declaration, found {}", describe(self.peek())),
            ));
        }
        let is_generator = if named_keyword && *self.peek() == Tok::Star {
            self.advance();
            self.expect_word("gen")?;
            true
        } else {
            false
        };
        let prefix_type = if return_first {
            let ty = self.parse_type_with_return_marker(true)?;
            self.consume_return_first_marker()?;
            Some(ty)
        } else {
            None
        };
        let (name, pos) = self.ident("a function name")?;
        if *self.peek() == Tok::Lt {
            return Err(unsupported(self.pos(), "generic functions"));
        }
        self.expect(Tok::LParen, "'('")?;
        let params = self.parse_params(true)?;
        let ret = if let Some(ty) = prefix_type {
            if matches!(self.peek(), Tok::Arrow | Tok::Colon) {
                return Err(Diagnostic::new(self.pos(), "E_SYNTAX",
                    "return-first callable cannot declare a second result type"));
            }
            ty
        } else {
            self.parse_return_spec()?
        };
        if is_generator && ret != Type::GeneratorInt {
            return Err(Diagnostic::new(pos, "E_TYPE",
                "a 'fnc *gen' declaration must return Generator<int> in this frontend stage"));
        }
        self.in_generator = is_generator;
        let body = if prefix_type.is_some() {
            self.parse_return_first_body()?
        } else if self.eat(&Tok::ColonColon) {
            // Compatibility slice: explicit parenthesized scalar expression
            // followed by end. Do not infer return from a statement block.
            if is_generator || ret != Type::Int {
                return Err(unsupported(self.pos(), "non-int or generator :: expression body"));
            }
            let expression_pos = self.pos();
            self.expect(Tok::LParen, "'(' after '::'")?;
            let value = self.parse_expr()?;
            self.expect(Tok::RParen, "')' after expression body")?;
            self.expect_word("end")?;
            vec![Stmt::Return(Some(value), expression_pos)]
        } else {
            self.parse_block()?
        };
        self.in_generator = false;
        let _ = start;
        Ok(Func {
            name,
            pos,
            is_generator,
            params,
            ret,
            body,
        })
    }

    fn parse_param(&mut self, allow_omittable: bool) -> R<Param> {
        let pos = self.pos();
        if self.is_word("structural") {
            return Err(unsupported(pos, "structural parameters"));
        }
        let name_first = matches!(self.peek(), Tok::Ident(_))
            && (*self.peek_at(1) == Tok::Colon
                || (*self.peek_at(1) == Tok::Question && *self.peek_at(2) == Tok::Colon));
        if name_first {
            let (name, pos) = self.ident("a parameter name")?;
            let omittable = self.eat(&Tok::Question);
            self.expect(Tok::Colon, "':' after parameter name")?;
            let ty = self.parse_type()?;
            let mutable = self.eat_word("mut");
            return self.finish_param(name, ty, mutable, omittable, allow_omittable, pos);
        }
        let ty = self.parse_type()?;
        let mutable = self.eat_word("mut");
        let (name, pos) = self.ident("a parameter name")?;
        let omittable = self.eat(&Tok::Question);
        self.finish_param(name, ty, mutable, omittable, allow_omittable, pos)
    }

    fn finish_param(&self, name: String, ty: Type, mutable: bool, omittable: bool,
        allow_omittable: bool, pos: Pos) -> R<Param> {
        if ty == Type::Void {
            return Err(Diagnostic::new(pos, "E_TYPE", "a parameter cannot have type void"));
        }
        if *self.peek() == Tok::Assign {
            return Err(unsupported(self.pos(), "default parameter values"));
        }
        if omittable && (!allow_omittable || mutable || !matches!(ty, Type::OptionInt | Type::OptionResultIntStr)) {
            return Err(Diagnostic::new(pos, "E_OMITTABLE_DEFAULT",
                "this direct-call slice admits only immutable omittable Option formals with implicit None"));
        }
        Ok(Param { name, ty, mutable, omittable, pos })
    }

    // ----------------------------------------------------------- statements

    fn parse_block(&mut self) -> R<Vec<Stmt>> {
        self.expect(Tok::LBrace, "'{'")?;
        self.enter()?;
        let mut out = Vec::new();
        while *self.peek() != Tok::RBrace {
            if *self.peek() == Tok::Eof {
                return Err(self.expected("'}'"));
            }
            out.push(self.parse_stmt()?);
        }
        self.advance();
        self.leave();
        Ok(out)
    }

    fn parse_stmts_until(&mut self, stops: &[&str]) -> R<Vec<Stmt>> {
        self.enter()?;
        let mut out = Vec::new();
        loop {
            if let Some(w) = self.word_at(0) {
                if stops.contains(&w) {
                    break;
                }
            }
            if *self.peek() == Tok::Eof {
                let want: Vec<String> = stops.iter().map(|s| format!("'{s}'")).collect();
                return Err(self.expected(&want.join(" or ")));
            }
            if *self.peek() == Tok::RBrace {
                return Err(self.expected(&format!("'{}'", stops.last().unwrap())));
            }
            out.push(self.parse_stmt()?);
        }
        self.leave();
        Ok(out)
    }

    fn parse_loop_body(&mut self) -> R<Vec<Stmt>> {
        if *self.peek() == Tok::LBrace {
            self.parse_block()
        } else if self.eat_word("do") {
            let body = self.parse_stmts_until(&["done"])?;
            self.expect_word("done")?;
            Ok(body)
        } else {
            Err(self.expected("'do' or '{'"))
        }
    }

    /// Java `Parser.consumeStatementTerminator`: a `;`, a safe statement
    /// boundary (`}` `fi` `end` `else` `elseif` `done` or end of input), or an
    /// implicit newline terminator (the next token starts on a later line).
    fn terminator(&mut self) -> R<()> {
        if self.eat(&Tok::Semi) {
            return Ok(());
        }
        let boundary = matches!(self.peek(), Tok::RBrace | Tok::Eof)
            || matches!(
                self.word_at(0),
                Some("fi" | "end" | "done" | "elif" | "elseif" | "else" | "catch" | "finally")
            );
        let newline = self.i > 0 && *self.peek() != Tok::Eof && self.toks[self.i - 1].pos.line < self.pos().line;
        if boundary || newline {
            Ok(())
        } else {
            Err(self.expected("';'"))
        }
    }

    fn parse_stmt(&mut self) -> R<Stmt> {
        let pos = self.pos();
        if *self.peek() == Tok::At {
            return Err(unsupported(pos, "annotations"));
        }
        if *self.peek() == Tok::LBrace || *self.peek() == Tok::LBracket {
            return Err(unsupported(pos, "destructuring"));
        }
        // Contiguous yield* is a dedicated token, not multiplication.
        if *self.peek() == Tok::YieldStar {
            if !self.in_generator {
                return Err(Diagnostic::new(pos, "E_YIELD",
                    "'yield*' is only valid inside a 'fnc *gen' generator"));
            }
            self.advance();
            let source = self.parse_expr()?;
            self.terminator()?;
            return Ok(Stmt::YieldFrom(source, pos));
        }
        // Statement-position '^' is the return alias. This frontend does not
        // yet lower expression-position XOR; keep that syntax distinct.
        let word = if *self.peek() == Tok::Caret {
            Some("return".to_owned())
        } else {
            self.word_at(0).map(str::to_owned)
        };
        match word.as_deref() {
            Some("val" | "const" | "let") => {
                let s = self.parse_binding()?;
                self.terminator()?;
                Ok(s)
            }
            // Java Parser.looksLikePrimitiveTypeFirstImmutableBinding: exactly
            // `int|bool|string|String name = expr` is an immutable binding.
            Some("int" | "bool" | "string" | "String")
                if matches!(self.peek_at(1), Tok::Ident(_)) && *self.peek_at(2) == Tok::Assign =>
            {
                let ty = self.parse_type()?;
                let (name, _) = self.ident("a variable name")?;
                self.advance();
                let init = self.parse_expr()?;
                self.terminator()?;
                Ok(Stmt::Bind {
                    kind: BindKind::Const,
                    name,
                    ty: Some(ty),
                    init,
                    pos,
                })
            }
            Some(_) if self.is_type_word(0) && matches!(self.peek_at(1), Tok::Ident(_)) => Err(Diagnostic::new(
                pos,
                "E_SYNTAX",
                "a local binding starts with 'val', 'const' or 'let' (for example 'val x: int = 1;'); only int, bool and string have the immutable 'T name = e' form",
            )),
            Some("yield") => {
                if !self.in_generator {
                    return Err(Diagnostic::new(pos, "E_YIELD",
                        "'yield' is only valid inside a 'fnc *gen' generator"));
                }
                self.advance();
                let value = self.parse_expr()?;
                self.terminator()?;
                Ok(Stmt::Yield(value, pos))
            }
            Some("return") => {
                self.advance();
                if self.is_word("match") {
                    if self.in_generator {
                        return Err(Diagnostic::new(pos, "E_RETURN",
                            "generator cannot return a match value"));
                    }
                    return self.parse_return_match(pos);
                }
                let at_end = matches!(self.peek(), Tok::Semi | Tok::RBrace)
                    || matches!(self.word_at(0), Some("fi" | "end" | "done" | "elif" | "elseif" | "else"));
                let explicit_void = self.is_word("void");
                if explicit_void { self.advance(); }
                let value = if at_end || explicit_void { None } else { Some(self.parse_expr()?) };
                if self.in_generator && value.is_some() {
                    return Err(Diagnostic::new(pos, "E_RETURN",
                        "generators may terminate with 'return;' but cannot return a value"));
                }
                self.terminator()?;
                Ok(Stmt::Return(value, pos))
            }
            Some("throw") => {
                self.advance();
                // Only the typed Error constructor is admitted by the
                // checker. A raw string is never silently throwable.
                let error = self.parse_expr()?;
                self.terminator()?;
                Ok(Stmt::Throw(error, pos))
            }
            Some("guard") => self.parse_guard(),
            Some("if") => self.parse_if(),
            Some("while") => {
                self.advance();
                let cond = self.parse_cond_head()?;
                self.eat(&Tok::Semi);
                let body = self.parse_loop_body()?;
                Ok(Stmt::While { cond, body, pos })
            }
            Some("loop") => {
                self.advance();
                let body = self.parse_loop_body()?;
                Ok(Stmt::Loop { body, pos })
            }
            Some("for") => self.parse_for(),
            Some("block") => {
                self.advance();
                Ok(Stmt::Block(self.parse_block()?))
            }
            Some("break") => {
                self.advance();
                self.terminator()?;
                Ok(Stmt::Break(pos))
            }
            Some("continue") => {
                self.advance();
                self.terminator()?;
                Ok(Stmt::Continue(pos))
            }
            Some("match") => self.parse_match(false),
            Some("do") if self.word_at(1) == Some("sync") && self.word_at(2) == Some("task") => {
                // Check the explicit payload before parsing the body. A
                // nonvoid annotation is always E_TASK_RETURN_TYPE, even if
                // the body contains other unsupported effects.
                if *self.peek_at(3) == Tok::Lt && self.word_at(4) != Some("void") {
                    return Err(Diagnostic::new(pos, "E_TASK_RETURN_TYPE",
                        "do sync task accepts only void; use sync task<T> without do"));
                }
                self.advance();
                let expr = self.parse_expr()?;
                // In the "do" form the payload is void-only, including
                // when omitted and inferred. Never silently discard a value.
                match &expr.kind {
                    ExprKind::InlineTask { declared, result, .. }
                        if declared.is_none_or(|ty| ty == Type::Void) && result.is_none() => {}
                    _ => return Err(Diagnostic::new(pos, "E_TASK_RETURN_TYPE",
                        "do sync task requires void and must not return a value")),
                }
                self.terminator()?;
                Ok(Stmt::Expr(expr))
            }
            Some("do") if self.word_at(1) == Some("task") => {
                Err(unsupported(pos, "supervised task with cancellation/cleanup"))
            }
            Some("do") if self.word_at(1) == Some("match") => {
                self.advance();
                self.parse_match(true)
            }
            Some("do") if matches!(self.word_at(1), Some("switch" | "select" | "nb")) => {
                Err(unsupported(pos, &format!("'do {}'", self.word_at(1).unwrap())))
            }
            Some("sync") if self.word_at(1) == Some("task") =>
                Err(Diagnostic::new(pos, "E_SYNTAX",
                    "standalone sync task requires 'do sync task'; use nonvoid tasks in a value context")),
            Some("trap") => {
                // Caller-side trap also works in expression-statement position.
                let e = self.parse_expr()?;
                self.terminator()?;
                Ok(Stmt::Expr(e))
            }
            Some(w) if UNSUPPORTED_STMT.contains(&w) => Err(unsupported(pos, &format!("'{w}'"))),
            Some("fnc" | "routine") => Err(unsupported(pos, "nested functions")),
            _ => {
                let e = self.parse_expr()?;
                // Compound addition uses the existing checked binary addition.
                // Restrict it to locals: field/index targets need once-only lvalue lowering.
                if *self.peek() == Tok::PlusAssign {
                    let at = self.pos();
                    self.advance();
                    let value = self.parse_expr()?;
                    self.terminator()?;
                    return match e.kind {
                        ExprKind::Var(name) => Ok(Stmt::Assign {
                            name: name.clone(),
                            value: Expr {
                                kind: ExprKind::Binary(BinOp::Add, Box::new(Expr {
                                    kind: ExprKind::Var(name), pos,
                                }), Box::new(value)),
                                pos: at,
                            },
                            pos,
                        }),
                        _ => Err(Diagnostic::new(at, "E_ASSIGN_TARGET",
                            "compound '+=' currently requires a mutable local variable")),
                    };
                }
                if *self.peek() == Tok::Assign {
                    let at = self.pos();
                    self.advance();
                    let value_of = |p: &mut Self| -> R<Expr> {
                        let value = p.parse_expr()?;
                        if *p.peek() == Tok::Assign {
                            return Err(unsupported(p.pos(), "chained assignment"));
                        }
                        p.terminator()?;
                        Ok(value)
                    };
                    return match e.kind {
                        ExprKind::Var(name) => {
                            let value = value_of(self)?;
                            Ok(Stmt::Assign { name, value, pos })
                        }
                        ExprKind::Member(target, field) => {
                            let value = value_of(self)?;
                            Ok(Stmt::FieldAssign {
                                target: *target,
                                field,
                                value,
                                pos,
                            })
                        }
                        _ => Err(Diagnostic::new(
                            at,
                            "E_ASSIGN_TARGET",
                            "only a local variable or a 'self' field can be assigned",
                        )),
                    };
                }
                if matches!(self.peek(), Tok::PlusPlus | Tok::MinusMinus) {
                    return Err(Diagnostic::new(
                        self.pos(),
                        "E_UNSUPPORTED",
                        "'++' and '--' exist only in a for update clause",
                    ));
                }
                if matches!(e.kind, ExprKind::InlineTask { .. }) {
                    return Err(Diagnostic::new(e.pos, "E_SYNTAX",
                        "standalone sync task requires 'do sync task'"));
                }
                self.terminator()?;
                Ok(Stmt::Expr(e))
            }
        }
    }

    /// grammar.ebnf `binding_decl`: `("val"|"const"|"let") (IDENT ":" type_ref | [type_ref] IDENT) "=" expression`.
    fn parse_binding(&mut self) -> R<Stmt> {
        let pos = self.pos();
        let kind = if self.eat_word("val") {
            BindKind::Val
        } else if self.eat_word("const") {
            BindKind::Const
        } else if self.eat_word("let") {
            BindKind::Let
        } else {
            return Err(self.expected("'val', 'const' or 'let'"));
        };
        // Java Parser.parseReferentMutability: `const mut` / `let mut` request a
        // mutable referent (no effect on scalar values); `val` already means
        // `const mut`, so `val mut` is a duplicate qualifier.
        let explicit_mut = self.is_word("mut");
        if explicit_mut {
            if kind == BindKind::Val {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_SYNTAX",
                    "'val' already means 'const mut'; 'val mut' is a duplicate mutability qualifier",
                ));
            }
            self.advance();
        }
        let (name, ty) = if matches!(self.peek(), Tok::Ident(_)) && *self.peek_at(1) == Tok::Colon {
            let (name, _) = self.ident("a variable name")?;
            self.advance();
            (name, Some(self.parse_type()?))
        } else if matches!(self.peek(), Tok::Ident(_))
            && (matches!(self.peek_at(1), Tok::Ident(_))
                || (matches!(self.word_at(0), Some("Option" | "Result" | "ActorRef"))
                    && *self.peek_at(1) == Tok::Lt))
        {
            // Recognize generic type-first bindings without treating
            // 'Option' as a variable name and '<' as a comparison.
            let ty = self.parse_type()?;
            let (name, _) = self.ident("a variable name")?;
            (name, Some(ty))
        } else {
            let (name, _) = self.ident("a variable name")?;
            (name, None)
        };
        if ty == Some(Type::Void) {
            return Err(Diagnostic::new(pos, "E_TYPE", "a variable cannot have type void"));
        }
        if *self.peek() != Tok::Assign {
            return Err(Diagnostic::new(
                self.pos(),
                "E_UNINITIALIZED",
                format!("binding '{name}' needs an initializer"),
            ));
        }
        self.advance();
        let init = if self.struct_target && self.is_word("copy") && *self.peek_at(1) == Tok::Bang {
            let copy_pos = self.pos();
            self.advance();
            self.advance();
            let (source, source_pos) = self.ident("a local scalar struct after copy!")?;
            Expr { kind: ExprKind::Call("$struct_copy_operand".to_owned(),
                vec![Expr { kind: ExprKind::Var(source), pos: source_pos }]), pos: copy_pos }
        } else {
            self.parse_expr()?
        };
        if (kind == BindKind::Val || explicit_mut) && kind != BindKind::Const {
            if let Some(bindings) = self.struct_value_bindings.as_mut() {
                bindings.push(pos);
            }
        }
        Ok(Stmt::Bind { kind, name, ty, init, pos })
    }

    /// AttoLang dual-form early-exit guard. Both surface spellings lower
    /// through the same checked negated If statement. Unsupported payload
    /// narrowing remains checked elsewhere and must never be invented.
    fn parse_guard(&mut self) -> R<Stmt> {
        let pos = self.pos();
        self.expect_word("guard")?;
        let predicate = lazy_guard_predicate(self.parse_cond_head()?);
        let failure = if *self.peek() == Tok::LBrace {
            // An enclosing keyword if owns its following else/fi.
            // A stray else outside that context remains a syntax error.
            self.parse_block()?
        } else if self.eat_word("else") {
            let block = self.parse_stmts_until(&["end"])?;
            self.expect_word("end")?;
            block
        } else {
            return Err(Diagnostic::new(self.pos(), "E_GUARD",
                "guard requires '{ ... }' or 'else ... end' failure block"));
        };
        if guard_has_nonreturn_control_exit(&failure) || !guard_failure_returns(&failure) {
            return Err(Diagnostic::new(pos, "E_GUARD",
                "guard failure block must explicitly return on all reachable paths"));
        }
        Ok(Stmt::If {
            arms: vec![(
                Expr { kind: ExprKind::Unary(UnOp::Not, Box::new(predicate)), pos },
                failure,
            )],
            otherwise: None,
            pos,
        })
    }

    fn parse_if(&mut self) -> R<Stmt> {
        let pos = self.pos();
        self.expect_word("if")?;
        let cond = self.parse_cond_head()?;
        let mut arms = Vec::new();
        let mut otherwise = None;
        if *self.peek() == Tok::LBrace {
            arms.push((cond, self.parse_block()?));
            loop {
                if self.eat_word("elif")
                    || self.eat_word("elseif")
                    || (self.is_word("else") && self.word_at(1) == Some("if") && {
                        self.advance();
                        self.advance();
                        true
                    })
                {
                    self.grow(1)?;
                    let c = self.parse_cond_head()?;
                    arms.push((c, self.parse_block()?));
                } else if self.eat_word("else") {
                    otherwise = Some(self.parse_block()?);
                    break;
                } else {
                    break;
                }
            }
        } else {
            self.eat(&Tok::Semi);
            if !(self.eat_word("then") || self.eat_word("do")) {
                return Err(self.expected("'then'"));
            }
            let stops = ["elif", "elseif", "else", "fi"];
            arms.push((cond, self.parse_stmts_until(&stops)?));
            loop {
                let is_elif = self.eat_word("elif")
                    || self.eat_word("elseif")
                    || (self.is_word("else") && self.word_at(1) == Some("if") && {
                        self.advance();
                        self.advance();
                        true
                    });
                if is_elif {
                    self.grow(1)?;
                    let c = self.parse_cond_head()?;
                    self.eat(&Tok::Semi);
                    if !(self.eat_word("then") || self.eat_word("do")) {
                        return Err(self.expected("'then'"));
                    }
                    arms.push((c, self.parse_stmts_until(&stops)?));
                } else if self.eat_word("else") {
                    otherwise = Some(self.parse_stmts_until(&["fi"])?);
                    break;
                } else {
                    break;
                }
            }
        }
        if !self.eat_word("fi") {
            return Err(Diagnostic::new(
                self.pos(),
                if *self.peek() == Tok::Eof { "E_EOF" } else { "E_FI" },
                format!(
                    "expected 'fi' to close the if started at {}:{}, found {}",
                    pos.line,
                    pos.col,
                    describe(self.peek())
                ),
            ));
        }
        // each elif arm nests one level deeper in the lowered if/else chain
        self.height -= arms.len() as u32 - 1;
        Ok(Stmt::If { arms, otherwise, pos })
    }

    /// A value-producing return-position match reuses the existing checked
    /// exhaustive Match lowering rather than creating a second evaluator.
    fn parse_return_match(&mut self, pos: Pos) -> R<Stmt> {
        self.expect_word("match")?;
        let subject = self.parse_expr()?;
        self.expect_word("over")?;
        let mut arms = Vec::new();
        while !self.is_word("end") {
            if *self.peek() == Tok::Eof {
                return Err(Diagnostic::new(self.pos(), "E_EOF",
                    "expected 'end' to close return match"));
            }
            self.grow(1)?;
            let apos = self.pos();
            let pattern = if self.eat_word("in") {
                self.parse_int_membership()?
            } else if self.is_word("is") {
                self.parse_static_type_pattern()?
            } else {
                self.expect_word("on")?;
                self.parse_pattern()?
            };
            let guard = if self.eat_word("when") {
                Some(self.parse_expr()?)
            } else {
                None
            };
            self.expect(Tok::Arrow, "'->' in a return match arm")?;
            let braced = *self.peek() == Tok::LBrace;
            let body = if braced {
                self.parse_block()?
            } else {
                let value = self.parse_expr()?;
                vec![Stmt::Return(Some(value), apos)]
            };
            arms.push(MatchArm { pattern, guard, body, pos: apos });
            if braced {
                self.eat(&Tok::Comma);
            } else if !self.eat(&Tok::Comma) && !self.is_word("end") {
                return Err(Diagnostic::new(self.pos(), "E_SYNTAX",
                    "expected ',' between return match value arms"));
            }
        }
        self.expect_word("end")?;
        self.height -= arms.len() as u32;
        if arms.is_empty() {
            return Err(Diagnostic::new(pos, "E_SYNTAX",
                "return match requires at least one arm"));
        }
        self.terminator()?;
        Ok(Stmt::Match { subject, arms, pos })
    }

    /// Java Parser.parseMatch. Canonical `match x over on P -> { } end`
    /// (or `over { on P { } }`); legacy `match [first] x [;] P -> { } end`
    /// with `else` as the catch-all. `do match` requires `over`.
    fn parse_match(&mut self, procedural: bool) -> R<Stmt> {
        let pos = self.pos();
        self.expect_word("match")?;
        if *self.peek() == Tok::Lt {
            return Err(unsupported(self.pos(), "value match ('match<T>')"));
        }
        self.eat_word("first");
        let subject = self.parse_expr()?;
        let over = self.eat_word("over");
        if !over {
            self.eat(&Tok::Semi);
        }
        let braced = over && self.eat(&Tok::LBrace);
        if procedural && !over {
            return Err(Diagnostic::new(
                self.pos(),
                "E_SYNTAX",
                "'do match' requires 'over' before its arms",
            ));
        }
        let mut arms = Vec::new();
        loop {
            let done = if braced { *self.peek() == Tok::RBrace } else { self.is_word("end") };
            if done || *self.peek() == Tok::Eof {
                break;
            }
            self.grow(1)?;
            let apos = self.pos();
            let membership = over && self.eat_word("in");
            let type_test = over && self.is_word("is");
            if over && !membership && !type_test && !self.eat_word("on") {
                let msg = if self.is_word("else") || self.is_word("default") {
                    "match has no default/else keyword; use 'on _ -> { ... }'"
                } else {
                    "every 'match ... over' arm must start with 'on', 'in', or 'is'"
                };
                return Err(Diagnostic::new(self.pos(), "E_SYNTAX", msg));
            }
            let pattern = if membership {
                self.parse_int_membership()?
            } else if !over && self.eat_word("else") {
                Pattern::Wild
            } else {
                self.parse_pattern()?
            };
            let guard = if self.eat_word("when") { Some(self.parse_expr()?) } else { None };
            if *self.peek() == Tok::Assign && *self.peek_at(1) == Tok::Gt {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_SYNTAX",
                    "match implementations use the slim arrow '->'; '=>' is reserved for type definitions",
                ));
            }
            if !self.eat(&Tok::Arrow) && !braced {
                return Err(Diagnostic::new(self.pos(), "E_SYNTAX", "match arms in 'over ... end' use '->'"));
            }
            let body = self.parse_block()?;
            arms.push(MatchArm {
                pattern,
                guard,
                body,
                pos: apos,
            });
        }
        if braced {
            self.expect(Tok::RBrace, "'}' to close braced match")?;
        } else if !self.eat_word("end") {
            return Err(Diagnostic::new(
                self.pos(),
                if *self.peek() == Tok::Eof { "E_EOF" } else { "E_SYNTAX" },
                "expected 'end' to close match",
            ));
        }
        self.height -= arms.len() as u32;
        if arms.is_empty() {
            return Err(Diagnostic::new(pos, "E_SYNTAX", "match requires at least one arm"));
        }
        Ok(Stmt::Match { subject, arms, pos })
    }

    fn pattern_ref(&mut self, array: bool) -> R<Pattern> {
        let pos = self.pos();
        let (name, _) = self.ident("compile-time pattern name")?;
        let (actual, pattern) = self.pattern_constants.get(&name).ok_or_else(||
            Diagnostic::new(pos, "E_PATTERN_NAME", "unknown or forward/cyclic Pattern reference"))?;
        if *actual != array {
            return Err(Diagnostic::new(pos, "E_PATTERN_TYPE", "Pattern and Pattern[] are distinct"));
        }
        self.pattern_name_uses.insert(self.i - 1);
        Ok(pattern.clone())
    }

    fn combine_patterns(parts: Vec<Pattern>, pos: Pos) -> R<Pattern> {
        let mut ints = Vec::new();
        let mut bools = Vec::new();
        let mut types = Vec::new();
        for part in parts {
            let (iv, bv, tv) = match part {
                Pattern::Int(v) => (vec![v], vec![], vec![]),
                Pattern::InInts(v) => (v, vec![], vec![]),
                Pattern::Bool(v) => (vec![], vec![v], vec![]),
                Pattern::StaticTypes(v) => (vec![], vec![], v),
                Pattern::ConstSet { ints, bools, types } => (ints, bools, types),
                _ => return Err(unsupported(pos, "stored binding or non-scalar pattern")),
            };
            for v in iv {
                if ints.contains(&v) { return Err(Diagnostic::new(pos, "E_MATCH", "duplicate pattern literal")); }
                ints.push(v);
            }
            for v in bv {
                if bools.contains(&v) { return Err(Diagnostic::new(pos, "E_MATCH", "duplicate boolean pattern")); }
                bools.push(v);
            }
            for v in tv {
                if types.contains(&v) { return Err(Diagnostic::new(pos, "E_MATCH", "duplicate canonical pattern type")); }
                types.push(v);
            }
            if ints.len() + bools.len() + types.len() > 64 {
                return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "expanded Pattern set exceeds 64 alternatives"));
            }
        }
        if ints.is_empty() && bools.is_empty() && types.is_empty() {
            return Err(Diagnostic::new(pos, "E_MATCH", "empty Pattern set"));
        }
        Ok(Pattern::ConstSet { ints, bools, types })
    }

    fn parse_pattern_array(&mut self, depth: u32) -> R<Pattern> {
        let pos = self.pos();
        if depth >= 32 { return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "nested Pattern limit")); }
        self.expect(Tok::LBracket, "'[' in Pattern array")?;
        let mut parts = Vec::new();
        loop {
            if parts.len() >= 64 {
                return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "Pattern array limit"));
            }
            parts.push(self.parse_pattern_descriptor(depth + 1)?);
            if self.eat(&Tok::RBracket) { break; }
            self.expect(Tok::Comma, "comma between patterns")?;
            if self.eat(&Tok::RBracket) { break; }
        }
        Self::combine_patterns(parts, pos)
    }

    fn parse_pattern_descriptor(&mut self, depth: u32) -> R<Pattern> {
        let pos = self.pos();
        if depth >= 32 { return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "nested Pattern limit")); }
        if self.eat_word("on") {
            if self.is_word("true") || self.is_word("false") {
                let b = self.is_word("true"); self.advance(); return Ok(Pattern::Bool(b));
            }
            if matches!(self.peek(), Tok::Ident(_)) { return self.pattern_ref(false); }
            let neg = self.eat(&Tok::Minus);
            let n = match self.peek() {
                Tok::Int(Some(v)) => if neg { -(*v as i64) } else { *v as i64 },
                Tok::Int(None) if neg => i64::MIN,
                _ => return Err(unsupported(pos, "stored pattern literal must be int or bool")),
            };
            self.advance(); return Ok(Pattern::Int(n));
        }
        if self.is_word("is") { return self.parse_static_type_pattern(); }
        if self.eat_word("in") {
            if *self.peek() == Tok::LBracket &&
                (matches!(self.word_at(1), Some("on" | "is" | "in")) ||
                 self.word_at(1).is_some_and(|name| self.pattern_constants.contains_key(name))) {
                return self.parse_pattern_array(depth + 1);
            }
            return self.parse_int_membership();
        }
        self.pattern_ref(false)
    }

    fn parse_pattern_constant(&mut self) -> R<()> {
        let pos = self.pos();
        self.expect_word("const")?;
        self.expect_word("Pattern")?;
        let array = self.eat(&Tok::LBracket);
        if array { self.expect(Tok::RBracket, "']' in Pattern[]")?; }
        let name_index = self.i;
        let (name, _) = self.ident("compile-time pattern name")?;
        if matches!(name.as_str(), "Pattern" | "Int" | "int" | "bool" | "boolean" | "float" | "f64" | "String" | "string") ||
            self.pattern_constants.contains_key(&name) || self.pattern_type_aliases.contains_key(&name) {
            return Err(Diagnostic::new(pos, "E_PATTERN_NAME", "duplicate or reserved Pattern name"));
        }
        if self.pattern_constants.len() >= 128 {
            return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "too many Pattern declarations"));
        }
        self.expect(Tok::Assign, "'=' in Pattern declaration")?;
        let pattern = if array { self.parse_pattern_array(0)? } else { self.parse_pattern_descriptor(0)? };
        self.expect(Tok::Semi, "';' after Pattern declaration")?;
        self.pattern_name_uses.insert(name_index);
        self.pattern_constants.insert(name, (array, pattern));
        Ok(())
    }

    /// First membership slice: a bounded, nonempty literal int list.
    /// No dynamic collection expression or implicit runtime array allocation.
    fn parse_int_membership(&mut self) -> R<Pattern> {
        let pos = self.pos();
        if matches!(self.peek(), Tok::Ident(name) if self.pattern_constants.contains_key(name)) {
            return self.pattern_ref(true);
        }
        if *self.peek() == Tok::LBracket &&
            (matches!(self.word_at(1), Some("on" | "is" | "in")) ||
             self.word_at(1).is_some_and(|name| self.pattern_constants.contains_key(name))) {
            return self.parse_pattern_array(0);
        }
        if !self.eat(&Tok::LBracket) {
            return Err(unsupported(pos, "runtime collection membership arms"));
        }
        if self.eat(&Tok::RBracket) {
            return Err(Diagnostic::new(pos, "E_MATCH", "empty membership arm cannot match any value"));
        }
        let mut values = Vec::new();
        loop {
            if values.len() == 64 {
                return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "membership arm exceeds 64 literal values"));
            }
            let at = self.pos();
            let negative = self.eat(&Tok::Minus);
            let value = match self.peek().clone() {
                Tok::Int(Some(v)) => if negative { -(v as i64) } else { v as i64 },
                Tok::Int(None) if negative => i64::MIN,
                Tok::Int(None) => return Err(Diagnostic::new(at, "E_INTEGER_RANGE", "integer literal out of range")),
                _ => return Err(unsupported(at, "nonliteral or noninteger membership element")),
            };
            self.advance();
            if values.contains(&value) {
                return Err(Diagnostic::new(at, "E_MATCH", "duplicate integer membership element"));
            }
            values.push(value);
            if self.eat(&Tok::RBracket) { break; }
            self.expect(Tok::Comma, "comma between membership values")?;
            if self.eat(&Tok::RBracket) { break; }
        }
        Ok(Pattern::InInts(values))
    }

    /// Static scalar type disjunction. The ordinary checker verifies the
    /// subject type and every reachable body before emitting any guest.
    fn parse_pattern_type_atom(&mut self) -> R<Vec<Type>> {
        let pos = self.pos();
        let spelling = match self.peek() {
            Tok::Ident(w) => w.clone(),
            _ => return Err(Diagnostic::new(pos, "E_SYNTAX", "expected a scalar pattern type")),
        };
        if let Some(types) = self.pattern_type_aliases.get(&spelling).cloned() {
            self.pattern_alias_uses.insert(self.i);
            self.advance();
            return Ok(types);
        }
        let ty = match spelling.as_str() {
            "int" | "Int" => Type::Int,
            "bool" | "boolean" => Type::Bool,
            "float" | "f64" => Type::Float,
            "string" | "String" => Type::Str,
            _ => return Err(unsupported(pos, "unknown, forward/cyclic or non-scalar pattern type")),
        };
        self.pattern_type_spellings.insert(spelling, pos);
        self.advance();
        Ok(vec![ty])
    }

    fn append_pattern_types(target: &mut Vec<Type>, alternatives: Vec<Type>, pos: Pos) -> R<()> {
        for ty in alternatives {
            if target.contains(&ty) {
                return Err(Diagnostic::new(pos, "E_MATCH", "duplicate canonical type in pattern type set"));
            }
            if target.len() >= 64 {
                return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "pattern type expansion limit"));
            }
            target.push(ty);
        }
        Ok(())
    }

    fn parse_pattern_type_alias(&mut self) -> R<()> {
        let pos = self.pos();
        self.expect_word("type")?;
        let name_index = self.i;
        let (name, _) = self.ident("scalar pattern type alias")?;
        if matches!(name.as_str(), "Pattern" | "Int" | "int" | "bool" | "boolean" | "float" | "f64" | "String" | "string")
            || self.pattern_constants.contains_key(&name) || self.pattern_type_aliases.contains_key(&name) {
            return Err(Diagnostic::new(pos, "E_PATTERN_NAME", "duplicate or reserved scalar pattern type alias"));
        }
        if self.pattern_type_aliases.len() >= 128 {
            return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "scalar pattern alias declaration limit"));
        }
        self.expect(Tok::Assign, "'=' in scalar pattern type alias")?;
        let mut types = Vec::new();
        loop {
            let next = self.parse_pattern_type_atom()?;
            Self::append_pattern_types(&mut types, next, pos)?;
            if !self.eat(&Tok::Pipe) { break; }
        }
        self.expect(Tok::Semi, "';' after scalar pattern type alias")?;
        self.pattern_alias_uses.insert(name_index);
        self.pattern_type_aliases.insert(name, types);
        Ok(())
    }

    fn parse_static_type_pattern(&mut self) -> R<Pattern> {
        let pos = self.pos();
        self.expect_word("is")?;
        let plural = self.eat_word("in");
        if plural && *self.peek() != Tok::LBracket {
            let pattern = self.pattern_ref(true)?;
            return match pattern {
                Pattern::ConstSet { ints, bools, types } if ints.is_empty() && bools.is_empty() =>
                    Ok(Pattern::StaticTypes(types)),
                _ => Err(Diagnostic::new(pos, "E_PATTERN_TYPE", "is in requires type-test-only Pattern[]")),
            };
        }
        if plural { self.expect(Tok::LBracket, "'[' in type list")?; }
        let mut types = Vec::new();
        loop {
            if *self.peek() == Tok::RBracket {
                return Err(Diagnostic::new(self.pos(), "E_SYNTAX", "type list must contain a type"));
            }
            let alternatives = self.parse_pattern_type_atom()?;
            Self::append_pattern_types(&mut types, alternatives, pos)?;
            if !plural || self.eat(&Tok::RBracket) { break; }
            self.expect(Tok::Comma, "comma between type alternatives")?;
        }
        if self.is_word("as") {
            return Err(unsupported(pos, "static type-list binding requires dedicated narrowing"));
        }
        Ok(Pattern::StaticTypes(types))
    }

    /// Java Parser.parsePattern, restricted to the forms this compiler lowers.
    fn parse_pattern(&mut self) -> R<Pattern> {
        let pos = self.pos();
        if matches!(self.peek(), Tok::Ident(name) if self.pattern_constants.contains_key(name)) {
            return self.pattern_ref(false);
        }
        match self.peek().clone() {
            Tok::Int(Some(v)) => {
                self.advance();
                Ok(Pattern::Int(v as i64))
            }
            Tok::Int(None) => Err(Diagnostic::new(
                pos,
                "E_INTEGER_RANGE",
                "integer literal does not fit in a signed 64-bit int",
            )),
            Tok::Float(_) => Err(unsupported(pos, "float literal patterns")),
            Tok::Str(_) => Err(unsupported(pos, "string literal patterns")),
            Tok::LParen => Err(unsupported(pos, "tuple patterns")),
            Tok::LBracket => Err(Diagnostic::new(
                pos,
                "E_SYNTAX",
                "list patterns are not supported; use tuples for fixed structural matching",
            )),
            Tok::Ident(w) => match w.as_str() {
                "true" | "false" => {
                    self.advance();
                    Ok(Pattern::Bool(w == "true"))
                }
                "None" => {
                    self.advance();
                    if *self.peek() == Tok::LParen {
                        return Err(Diagnostic::new(self.pos(), "E_ARITY",
                            "None is a zero-payload variant; write 'on None'"));
                    }
                    Ok(Pattern::Adt(w, None))
                }
                "Some" | "Ok" | "Err" => {
                    self.advance();
                    self.expect(Tok::LParen, "'(' after payload variant")?;
                    let (name, _) = self.ident("a payload binding name")?;
                    self.expect(Tok::RParen, "')' after payload binding")?;
                    Ok(Pattern::Adt(w, Some(name)))
                },
                "_" => {
                    self.advance();
                    Ok(Pattern::Wild)
                }
                "is" => self.parse_static_type_pattern(),
                "structural" => Err(unsupported(pos, "structural type patterns")),
                _ => {
                    self.advance();
                    if *self.peek() == Tok::Dot {
                        self.advance();
                        let (variant, _) = self.ident("an enum variant")?;
                        if self.is_word("as") || *self.peek() == Tok::Dot {
                            return Err(unsupported(self.pos(), "variant payload patterns"));
                        }
                        return Ok(Pattern::Variant(w, variant));
                    }
                    if *self.peek() == Tok::LParen {
                        return Err(unsupported(pos, "constructor patterns"));
                    }
                    if let Tok::Ident(next) = self.peek() {
                        if next != "when" && next != "_" {
                            return Err(unsupported(pos, "typed binding patterns ('Type name')"));
                        }
                        // A binding followed by 'when' is a guarded
                        // binder, not a typed pattern. parse_match and
                        // parse_return_match will read the guard in the arm
                        // scope, after the bound name is introduced.
                    }
                    if w.chars().next().is_some_and(|c| c.is_uppercase()) {
                        return Err(unsupported(pos, "constructor patterns"));
                    }
                    if is_reserved(&w) {
                        return Err(Diagnostic::new(pos, "E_RESERVED", format!("'{w}' is a reserved word")));
                    }
                    Ok(Pattern::Bind(w))
                }
            },
            _ => Err(Diagnostic::new(pos, "E_SYNTAX", "expected match pattern")),
        }
    }

    fn parse_for(&mut self) -> R<Stmt> {
        let pos = self.pos();
        self.expect_word("for")?;
        if matches!(self.peek(), Tok::Ident(_)) && self.word_at(1) == Some("in") {
            let (name, _) = self.ident("an iteration variable")?;
            self.expect_word("in")?;
            let source = self.parse_expr()?;
            let body = self.parse_loop_body()?;
            return Ok(Stmt::ForIn { name, source, body, pos });
        }
        let paren = self.eat(&Tok::LParen);
        if matches!(self.word_at(0), Some("const" | "let")) && self.word_at(2) == Some("of") {
            return Err(unsupported(self.pos(), "'for ... of' loops"));
        }
        let init = if *self.peek() == Tok::Semi {
            None
        } else if matches!(self.word_at(0), Some("val" | "const" | "let")) {
            Some(Box::new(self.parse_binding()?))
        } else if self.is_type_word(0) && matches!(self.peek_at(1), Tok::Ident(_)) {
            // grammar.ebnf for_init: type_ref IDENT "=" expression (immutable loop variable is useless; treat as let)
            let pos = self.pos();
            let ty = self.parse_type()?;
            let (name, _) = self.ident("a loop variable")?;
            self.expect(Tok::Assign, "'='")?;
            let init = self.parse_expr()?;
            Some(Box::new(Stmt::Bind {
                kind: BindKind::Let,
                name,
                ty: Some(ty),
                init,
                pos,
            }))
        } else {
            Some(Box::new(self.parse_simple_assign()?))
        };
        self.expect(Tok::Semi, "';'")?;
        let cond = if *self.peek() == Tok::Semi {
            None
        } else {
            Some(self.parse_cond()?)
        };
        self.expect(Tok::Semi, "';'")?;
        let update_end = if paren { Tok::RParen } else { Tok::LBrace };
        let update = if *self.peek() == update_end || (!paren && self.is_word("do")) {
            None
        } else {
            let upos = self.pos();
            let (name, _) = self.ident("a loop variable")?;
            if self.eat(&Tok::PlusPlus) {
                Some(Box::new(incr(name, upos, BinOp::Add)))
            } else if self.eat(&Tok::MinusMinus) {
                Some(Box::new(incr(name, upos, BinOp::Sub)))
            } else {
                self.expect(Tok::Assign, "'++', '--' or '='")?;
                let value = self.parse_expr()?;
                Some(Box::new(Stmt::Assign { name, value, pos: upos }))
            }
        };
        if paren {
            self.expect(Tok::RParen, "')'")?;
        }
        let body = self.parse_loop_body()?;
        Ok(Stmt::For {
            init,
            cond,
            update,
            body,
            pos,
        })
    }

    fn parse_simple_assign(&mut self) -> R<Stmt> {
        let pos = self.pos();
        let (name, _) = self.ident("a variable name")?;
        self.expect(Tok::Assign, "'='")?;
        let value = self.parse_expr()?;
        Ok(Stmt::Assign { name, value, pos })
    }

    // ---------------------------------------------------------- expressions

    fn parse_cond_head(&mut self) -> R<Expr> {
        let c = self.parse_cond()?;
        if *self.peek() == Tok::Assign {
            return Err(Diagnostic::new(
                self.pos(),
                "E_ASSIGN_CONDITION",
                "assignment is not allowed in a condition; use 'eq' to compare",
            ));
        }
        if self.is_word("as") {
            return Err(unsupported(self.pos(), "conditional 'as let' bindings"));
        }
        Ok(c)
    }

    /// `condition = condition_and { "|" condition_and } ;
    ///  condition_and = equality { "," equality } ;`
    fn parse_cond(&mut self) -> R<Expr> {
        self.cond += 1;
        let r = (|| {
            let mut links = 0;
            let mut left = self.parse_cond_and()?;
            while *self.peek() == Tok::Pipe {
                let pos = self.pos();
                self.advance();
                self.grow(1)?;
                links += 1;
                let right = self.parse_cond_and()?;
                left = Expr {
                    kind: ExprKind::Binary(BinOp::Or, Box::new(left), Box::new(right)),
                    pos,
                };
            }
            self.height -= links;
            Ok(left)
        })();
        self.cond -= 1;
        r
    }

    fn parse_cond_and(&mut self) -> R<Expr> {
        let mut links = 0;
        let mut left = self.parse_logical_or()?;
        while *self.peek() == Tok::Comma {
            let pos = self.pos();
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_logical_or()?;
            left = Expr {
                kind: ExprKind::Binary(BinOp::And, Box::new(left), Box::new(right)),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    pub fn parse_expr(&mut self) -> R<Expr> {
        self.enter()?;
        let saved = self.cond;
        self.cond = 0;
        let r = self.parse_ternary();
        self.cond = saved;
        self.leave();
        r
    }

    /// Short-circuit logical operators are distinct from comma/pipe condition
    /// operators, which are eagerly evaluated. The lexer deliberately keeps
    /// paired pipes as two tokens: initial || still starts a zero-arg lambda.
    /// In infix position only, a pair is logical OR.
    fn adjacent_operator_pair(&self, token: Tok) -> bool {
        let Some(next) = self.toks.get(self.i + 1) else { return false; };
        let current = &self.toks[self.i];
        current.tok == token && next.tok == token
            && current.pos.line == next.pos.line
            && current.pos.col.checked_add(1) == Some(next.pos.col)
    }

    fn parse_logical_or(&mut self) -> R<Expr> {
        let mut left = self.parse_logical_and()?;
        let mut links = 0;
        while self.adjacent_operator_pair(Tok::Pipe) {
            let pos = self.pos();
            self.advance();
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_logical_and()?;
            left = Expr {
                kind: ExprKind::Ternary(
                    Box::new(left),
                    Box::new(Expr { kind: ExprKind::Bool(true), pos }),
                    Box::new(right),
                ),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    /// a && b lowers to a ? b : false. The normal typed ternary emitter
    /// evaluates a exactly once and b only when a is true. No eager builtin
    /// and no changed meaning for unary & (reserved ownership/borrow form).
    fn parse_logical_and(&mut self) -> R<Expr> {
        let mut left = self.parse_equality()?;
        let mut links = 0;
        while self.adjacent_operator_pair(Tok::Amp) {
            let pos = self.pos();
            if self.struct_target {
                let next = self.toks[(self.i + 1).min(self.toks.len() - 1)].pos;
                if pos.line != next.line || pos.col.checked_add(1) != Some(next.col) {
                    return Err(Diagnostic::new(pos, "E_SYNTAX", "'&&' must be contiguous"));
                }
            }
            self.advance();
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_equality()?;
            left = Expr {
                kind: ExprKind::Ternary(
                    Box::new(left),
                    Box::new(right),
                    Box::new(Expr { kind: ExprKind::Bool(false), pos }),
                ),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    fn parse_ternary(&mut self) -> R<Expr> {
        let mut c = self.parse_logical_or()?;
        // Functional pipeline: value |> f |> g(extra) lowers to g(f(value), extra).
        // Only ordinary named functions are admitted: no computed callees,
        // receiver calls or actor piping. This preserves argument evaluation
        // order and leaves arity/type checking to the existing call checker.
        let mut links = 0;
        while *self.peek() == Tok::PipeGt {
            let pos = self.pos();
            self.advance();
            let stage = self.parse_postfix()?;
            let (name, mut args) = match stage.kind {
                ExprKind::Var(name) => (name, Vec::new()),
                ExprKind::Call(name, args) => (name, args),
                _ => return Err(unsupported(pos, "pipeline stages other than named function calls")),
            };
            if args.len() >= MAX_PARAMS {
                return Err(Diagnostic::new(pos, "E_SOURCE_LIMIT", "pipeline exceeds the call argument limit"));
            }
            self.grow(1)?;
            links += 1;
            args.insert(0, c);
            c = Expr { kind: ExprKind::Call(name, args), pos };
        }
        self.height -= links;
        match self.peek() {
            Tok::Pipe => {
                return Err(Diagnostic::new(
                    self.pos(),
                    "E_UNSUPPORTED",
                    "'|' is a boolean OR only inside an if/while condition; use Or(a, b) elsewhere",
                ))
            }
            Tok::Question => {
                let pos = self.pos();
                self.advance();
                let a = self.parse_expr()?;
                self.expect(Tok::Colon, "':'")?;
                self.enter()?;
                let b = self.parse_ternary();
                self.leave();
                return Ok(Expr {
                    kind: ExprKind::Ternary(Box::new(c), Box::new(a), Box::new(b?)),
                    pos,
                });
            }
            _ => {}
        }
        Ok(c)
    }

    fn parse_equality(&mut self) -> R<Expr> {
        let mut links = 0;
        let mut left = self.parse_comparison()?;
        loop {
            let pos = self.pos();
            let op = match self.peek() {
                Tok::EqEq => BinOp::Eq,
                Tok::NotEq => BinOp::Ne,
                Tok::Ident(s) if s == "eq" => BinOp::Eq,
                Tok::Ident(s) if s == "neq" => BinOp::Ne,
                Tok::Bang if self.word_at(1) == Some("eq") => {
                    self.advance();
                    BinOp::Ne
                }
                Tok::Ident(s) if s == "is" && self.word_at(1) == Some("omitted") => {
                    if !matches!(&left.kind, ExprKind::Var(_)) {
                        return Err(Diagnostic::new(pos, "E_OMISSION_PROVENANCE", "is omitted requires a formal binding"));
                    }
                    self.advance();
                    self.advance();
                    self.grow(1)?;
                    links += 1;
                    left = Expr { kind: ExprKind::Call("\0is_omitted".into(), vec![left]), pos };
                    continue;
                }
                Tok::Ident(s) if s == "is" => return Err(unsupported(pos, "the identity operator 'is'")),
                _ => break,
            };
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_comparison()?;
            left = Expr {
                kind: ExprKind::Binary(op, Box::new(left), Box::new(right)),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    fn parse_comparison(&mut self) -> R<Expr> {
        let mut links = 0;
        let mut left = self.parse_additive()?;
        loop {
            let pos = self.pos();
            let op = match self.peek() {
                Tok::Lt => BinOp::Lt,
                Tok::Le => BinOp::Le,
                Tok::Gt => BinOp::Gt,
                Tok::Ge => BinOp::Ge,
                Tok::Ident(s) if s == "as" || s == "matches" => return Err(unsupported(pos, &format!("'{s}'"))),
                _ => break,
            };
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_additive()?;
            left = Expr {
                kind: ExprKind::Binary(op, Box::new(left), Box::new(right)),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    fn parse_additive(&mut self) -> R<Expr> {
        let mut links = 0;
        let mut left = self.parse_multiplicative()?;
        loop {
            let pos = self.pos();
            let op = match self.peek() {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                _ => break,
            };
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_multiplicative()?;
            left = Expr {
                kind: ExprKind::Binary(op, Box::new(left), Box::new(right)),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    fn parse_multiplicative(&mut self) -> R<Expr> {
        let mut links = 0;
        let mut left = self.parse_unary()?;
        loop {
            let pos = self.pos();
            let op = match self.peek() {
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                _ => break,
            };
            self.advance();
            self.grow(1)?;
            links += 1;
            let right = self.parse_unary()?;
            left = Expr {
                kind: ExprKind::Binary(op, Box::new(left), Box::new(right)),
                pos,
            };
        }
        self.height -= links;
        Ok(left)
    }

    fn parse_unary(&mut self) -> R<Expr> {
        self.enter()?;
        let r = self.parse_unary_inner();
        self.leave();
        r
    }

    fn parse_unary_inner(&mut self) -> R<Expr> {
        let pos = self.pos();
        let op = match self.peek() {
            Tok::Minus => UnOp::Neg,
            Tok::Plus => UnOp::Plus,
            Tok::Bang | Tok::UnicodeNot => UnOp::Not,
            Tok::Amp => return Err(unsupported(pos, "borrow expressions")),
            Tok::PlusPlus | Tok::MinusMinus => {
                return Err(Diagnostic::new(
                    pos,
                    "E_UNSUPPORTED",
                    "'++' and '--' exist only in a for update clause",
                ))
            }
            Tok::Ident(s) if s == "await" => return Err(unsupported(pos, "'await'")),
            Tok::Ident(s) if s == "trap" => {
                self.advance();
                let inner = self.parse_unary()?;
                return Ok(Expr { kind: ExprKind::Trap(Box::new(inner)), pos });
            }
            _ => return self.parse_postfix(),
        };
        self.advance();
        let inner = self.parse_unary()?;
        Ok(Expr {
            kind: ExprKind::Unary(op, Box::new(inner)),
            pos,
        })
    }

    /// Call arguments after `(` up to and including `)`.
    fn parse_args(&mut self) -> R<Vec<Expr>> {
        self.expect(Tok::LParen, "'('")?;
        let mut args = Vec::new();
        if *self.peek() != Tok::RParen {
            loop {
                if matches!(self.peek(), Tok::Ident(_)) && *self.peek_at(1) == Tok::Colon {
                    return Err(unsupported(self.pos(), "named arguments"));
                }
                if *self.peek() == Tok::Dot {
                    return Err(unsupported(self.pos(), "spread arguments"));
                }
                args.push(self.parse_expr()?);
                if args.len() > MAX_PARAMS {
                    return Err(Diagnostic::new(self.pos(), "E_SOURCE_LIMIT", "too many call arguments"));
                }
                if !self.eat(&Tok::Comma) {
                    break;
                }
            }
        }
        self.expect(Tok::RParen, "')' to close the call")?;
        Ok(args)
    }

    fn parse_postfix(&mut self) -> R<Expr> {
        let mut chain = 0u32;
        let r = self.parse_postfix_chain(&mut chain);
        // member links nest one tree level each, like operator chain links
        self.height -= chain;
        r
    }

    // A '?' is ternary only when a top-level ':' follows its true arm.
    // Otherwise it is postfix propagation. Stop at the current expression
    // boundary and do not accidentally search into the next statement.
    fn question_opens_ternary(&self) -> bool {
        let mut depth = 0usize;
        for t in self.toks.iter().skip(self.i + 1) {
            match &t.tok {
                Tok::LParen | Tok::LBracket | Tok::LBrace => depth += 1,
                Tok::RParen | Tok::RBracket | Tok::RBrace if depth == 0 => return false,
                Tok::RParen | Tok::RBracket | Tok::RBrace => depth -= 1,
                Tok::Colon if depth == 0 => return true,
                Tok::Semi | Tok::Comma | Tok::Eof if depth == 0 => return false,
                Tok::Ident(w) if depth == 0 && matches!(
                    w.as_str(), "fi" | "end" | "done" | "elif" | "elseif" | "else"
                ) => return false,
                _ => {}
            }
        }
        false
    }

    fn parse_postfix_chain(&mut self, chain: &mut u32) -> R<Expr> {
        let pos = self.pos();
        let mut e = self.parse_primary()?;
        loop {
            match self.peek() {
                Tok::LParen => {
                    let name = match &e.kind {
                        ExprKind::Var(n) => n.clone(),
                        _ => return Err(unsupported(self.pos(), "calling a computed value")),
                    };
                    let args = self.parse_args()?;
                    e = Expr {
                        kind: ExprKind::Call(name, args),
                        pos,
                    };
                    if matches!(self.peek(), Tok::LParen) {
                        return Err(unsupported(self.pos(), "calling a computed value"));
                    }
                }
                Tok::Dot => {
                    let dot = self.pos();
                    if let ExprKind::Var(base) = &e.kind {
                        if base == "stdio" {
                            if let Some(m @ ("print" | "println")) = self.word_at(1) {
                                let qualified = format!("stdio.{m}");
                                self.advance();
                                self.advance();
                                if *self.peek() != Tok::LParen {
                                    return Err(self.expected("'('"));
                                }
                                e = Expr {
                                    kind: ExprKind::Var(qualified),
                                    pos,
                                };
                                continue;
                            }
                        }
                    }
                    if matches!(&e.kind, ExprKind::Var(b) if b == "stdio") {
                        return Err(unsupported(dot, "stdio members other than print and println"));
                    }
                    self.advance();
                    let member = match self.peek().clone() {
                        Tok::Ident(m) if !is_reserved(&m) => m,
                        _ => return Err(self.expected("a member name after '.'")),
                    };
                    self.advance();
                    self.grow(1)?;
                    *chain += 1;
                    if *self.peek() == Tok::LParen {
                        let args = self.parse_args()?;
                        e = Expr {
                            kind: ExprKind::MethodCall(Box::new(e), member, args),
                            pos: dot,
                        };
                    } else {
                        e = Expr {
                            kind: ExprKind::Member(Box::new(e), member),
                            pos: dot,
                        };
                    }
                }
                Tok::LBracket => return Err(unsupported(self.pos(), "indexing")),
                Tok::Question if !self.question_opens_ternary() => {
                    let question = self.pos();
                    self.advance();
                    self.grow(1)?;
                    *chain += 1;
                    e = Expr { kind: ExprKind::Propagate(Box::new(e)), pos: question };
                }
                Tok::Question => break, // true ternary: parsed by parse_ternary

                _ => break,
            }
        }
        if let ExprKind::Var(n) = &e.kind {
            if n.contains('.') {
                return Err(self.expected("'('"));
            }
        }
        Ok(e)
    }

    fn parse_primary(&mut self) -> R<Expr> {
        let pos = self.pos();
        let t = self.peek().clone();
        let kind = match t {
            Tok::Int(v) => {
                self.advance();
                ExprKind::Int(v)
            }
            Tok::Str(s) => {
                self.advance();
                ExprKind::Str(s)
            }
            Tok::Float(bits) => {
                self.advance();
                ExprKind::Float(bits)
            }
            Tok::LParen => {
                self.advance();
                let inner = if self.cond > 0 { self.parse_cond()? } else { self.parse_expr()? };
                if *self.peek() == Tok::Comma {
                    return Err(unsupported(self.pos(), "tuples"));
                }
                self.expect(Tok::RParen, "')'")?;
                return Ok(Expr { kind: inner.kind, pos });
            }
            Tok::Ident(s) => match s.as_str() {
                "true" => {
                    self.advance();
                    ExprKind::Bool(true)
                }
                "false" => {
                    self.advance();
                    ExprKind::Bool(false)
                }
                "null" => return Err(Diagnostic::new(pos, "E_NULL", "AttoLang has no null value; use Option<T>")),
                "self" => {
                    self.advance();
                    ExprKind::SelfRef
                }
                "new" => {
                    self.advance();
                    let (class, _) = self.ident("a class name after 'new'")?;
                    if *self.peek() == Tok::Lt {
                        return Err(unsupported(self.pos(), "generic class instantiation"));
                    }
                    if *self.peek() != Tok::LParen {
                        return Err(self.expected("'(' after the class name"));
                    }
                    let args = self.parse_args()?;
                    ExprKind::New(class, args)
                }
                "spawn" => {
                    self.advance();
                    let (actor, _) = self.ident("an actor name after 'spawn'")?;
                    self.expect(Tok::LParen, "'(' after the actor name")?;
                    if *self.peek() != Tok::RParen {
                        return Err(Diagnostic::new(
                            self.pos(),
                            "E_ACTOR",
                            format!(
                                "actor '{actor}' does not accept constructor/startup arguments; initialize actor-local state with fields"
                            ),
                        ));
                    }
                    self.advance();
                    ExprKind::Spawn(actor)
                }
                "sync" => {
                    self.advance();
                    self.expect_word("task")?;
                    let declared = if self.eat(&Tok::Lt) {
                        let payload = self.parse_type()?;
                        self.expect(Tok::Gt, "'>' after task payload type")?;
                        Some(payload)
                    } else {
                        None
                    };
                    let mut prelude = self.parse_block()?;
                    let result = if matches!(prelude.last(), Some(Stmt::Return(..))) {
                        match prelude.pop() {
                            Some(Stmt::Return(expr, _)) => expr.map(Box::new),
                            _ => unreachable!("checked terminal return"),
                        }
                    } else {
                        None
                    };
                    if !prelude.iter().all(inline_task_pure_stmt)
                        || result.as_ref().is_some_and(|v| !inline_task_pure_expr(v)) {
                        return Err(unsupported(pos,
                            "sync task requires a proven non-suspending scalar body; continuation, cleanup and nested exits are not lowered"));
                    }
                    ExprKind::InlineTask { declared, prelude, result }
                }
                "task" if matches!(self.peek_at(1), Tok::Lt | Tok::LBrace) => {
                    return Err(unsupported(pos, "task expression: typed Future, scoped cancellation and lock cleanup"));
                }
                "await" | "super" | "match" | "select" | "readch" | "writech" | "obj" | "arr" | "typeof" => {
                    return Err(unsupported(pos, &format!("'{s}' expressions")))
                }
                w if is_reserved(w) => {
                    return Err(Diagnostic::new(
                        pos,
                        "E_SYNTAX",
                        format!("expected an expression, found keyword '{w}'"),
                    ))
                }
                _ => {
                    self.advance();
                    if *self.peek() == Tok::Lt && self.looks_generic() {
                        return Err(unsupported(self.pos(), "generic calls"));
                    }
                    ExprKind::Var(s)
                }
            },
            Tok::Pipe => return Err(unsupported(pos, "lambdas")),
            Tok::LBracket => return Err(unsupported(pos, "sequence literals")),
            Tok::LBrace => return Err(unsupported(pos, "record literals")),
            _ => return Err(self.expected("an expression")),
        };
        Ok(Expr { kind, pos })
    }

    /// `f<int>(..)` heuristic: `<` Type `>` `(`.
    fn looks_generic(&self) -> bool {
        matches!(self.peek_at(1), Tok::Ident(_)) && *self.peek_at(2) == Tok::Gt && *self.peek_at(3) == Tok::LParen
    }
}

// The initial executable inline-task profile is deliberately effect-free.
// In particular, a call is not proven transitive-no-susp just because it has
// a scalar return. This predicate runs over parsed AST nodes, never source
// text, so comments/strings cannot bypass admission.
fn inline_task_pure_expr(e: &Expr) -> bool {
    match &e.kind {
        ExprKind::Int(_) | ExprKind::Bool(_) | ExprKind::Var(_) => true,
        ExprKind::Unary(_, inner) => inline_task_pure_expr(inner),
        ExprKind::Binary(_, a, b) => inline_task_pure_expr(a) && inline_task_pure_expr(b),
        ExprKind::Ternary(c, a, b) => inline_task_pure_expr(c)
            && inline_task_pure_expr(a) && inline_task_pure_expr(b),
        ExprKind::InlineTask { prelude, result, .. } => prelude.iter().all(inline_task_pure_stmt)
            && result.as_ref().is_none_or(|x| inline_task_pure_expr(x)),
        _ => false,
    }
}

fn inline_task_pure_stmt(stmt: &Stmt) -> bool {
    match stmt {
        Stmt::Bind { init, .. } => inline_task_pure_expr(init),
        Stmt::Assign { value, .. } => inline_task_pure_expr(value),
        Stmt::Expr(value) => inline_task_pure_expr(value),
        Stmt::Block(body) => body.iter().all(inline_task_pure_stmt),
        Stmt::If { arms, otherwise, .. } => arms.iter().all(|(test, body)|
            inline_task_pure_expr(test) && body.iter().all(inline_task_pure_stmt))
            && otherwise.as_ref().is_none_or(|body| body.iter().all(inline_task_pure_stmt)),
        _ => false,
    }
}

fn incr(name: String, pos: Pos, op: BinOp) -> Stmt {
    let var = Expr {
        kind: ExprKind::Var(name.clone()),
        pos,
    };
    let one = Expr {
        kind: ExprKind::Int(Some(1)),
        pos,
    };
    Stmt::Assign {
        name,
        value: Expr {
            kind: ExprKind::Binary(op, Box::new(var), Box::new(one)),
            pos,
        },
        pos,
    }
}

fn describe(t: &Tok) -> String {
    match t {
        Tok::Ident(s) => format!("'{s}'"),
        Tok::Int(_) => "an integer".into(),
        Tok::Str(_) => "a string".into(),
        Tok::Eof => "end of input".into(),
        other => {
            let s = match other {
                Tok::LParen => "(",
                Tok::RParen => ")",
                Tok::LBrace => "{",
                Tok::RBrace => "}",
                Tok::LBracket => "[",
                Tok::RBracket => "]",
                Tok::Semi => ";",
                Tok::Comma => ",",
                Tok::Colon => ":",
                Tok::ColonColon => "::",
                Tok::Arrow => "->",
                Tok::Bullet => "•",
                Tok::Dot => ".",
                Tok::Assign => "=",
                Tok::EqEq => "==",
                Tok::NotEq => "!=",
                Tok::Lt => "<",
                Tok::Le => "<=",
                Tok::Gt => ">",
                Tok::Ge => ">=",
                Tok::Plus => "+",
                Tok::PlusPlus => "++",
                Tok::PlusAssign => "+=",
                Tok::Minus => "-",
                Tok::MinusMinus => "--",
                Tok::Star => "*",
                Tok::YieldStar => "yield*",
                Tok::Slash => "/",
                Tok::Bang => "!",
                Tok::UnicodeNot => "¬",
                Tok::Question => "?",
                Tok::Pipe => "|",
                Tok::PipeGt => "|>",
                Tok::Amp => "&",
                Tok::Caret => "^",
                Tok::At => "@",
                _ => "?",
            };
            format!("'{s}'")
        }
    }
}
