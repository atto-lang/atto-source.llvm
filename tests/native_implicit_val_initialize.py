#!/usr/bin/env python3
"""Native compiled-class implicit VAL and typed initialize subset acceptance.

Every positive case compiles genuine Oreslang source, links emitted LLVM IR,
executes the resulting binary, and compares exit code 42. No fake backend.
Every negative case must reject before publishing LLVM IR.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

def run(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=60)

def class_source(field, header, body, return_expression="c.answer"):
    return (f"pub define class Connection as\n  {field}\n  {header}\n"
            f"    {body}\n"
            + ("  }\n" if "{" in header else "  end\n")
            + "end\n"
            + f"pub fnc main(): int {{ let Connection c = new Connection(42); return {return_expression}; }}\n")

POSITIVE = {
    "typed-arrow-double-colon": class_source("pub int answer;", "initialize(int id) -> void ::", "self.answer = id;"),
    "typed-arrow-newline": class_source("pub val int answer;", "initialize(int id) -> void", "self.answer = id;"),
    "typed-colon-newline": class_source("pub final int answer;", "initialize(int id): void", "self.answer = id;"),
    "typed-colon-braces": class_source("pub int answer;", "initialize(int id): void {", "self.answer = id;"),
    "typed-with-return": class_source("pub int answer;", "initialize(int id) -> void ::", "self.answer = id;\n    return;"),
    "two-fixed-fields": ("pub define class Connection as\n  pub int a;\n  pub int b;\n"
        "  initialize(int left, int right) -> void ::\n    self.a = left;\n    self.b = right;\n  end\nend\n"
        "pub fnc main(): int { let Connection c = new Connection(20,22); return c.a + c.b; }\n"),
    "mutable-let-still-reassignable": ("pub define class Connection as\n  pub let int answer: 40;\nend\n"
        "pub fnc main(): int { let mut Connection c = new Connection(); c.answer = 42; return c.answer; }\n"),
    "implicit-val-has-fixed-initializer": ("pub define class Connection as\n  pub int answer: 42;\nend\n"
        "pub fnc main(): int { let Connection c = new Connection(); return c.answer; }\n")
}

NEGATIVE = {
    "callable-as": (class_source("pub int answer;", "initialize(int id) as", "self.answer = id;"),
                    "explicit"),
    "nonvoid": (class_source("pub int answer;", "initialize(int id) -> int ::", "self.answer = id;"),
                "must return void"),
    "missing-void": (class_source("pub int answer;", "initialize(int id) {", "self.answer = id;"),
                     "requires an explicit"),
    "missing-field-assignment": ("pub define class Connection as\n  pub int answer;\n"
        "  initialize(int id) -> void {}\nend\n"
        "pub fnc main(): int { let Connection c = new Connection(42); return c.answer; }\n",
        "must assign every"),
    "duplicate-field-assignment": (class_source("pub int answer;", "initialize(int id) -> void ::",
        "self.answer = id;\n    self.answer = id;"), "more than once"),
    "wrong-field": (class_source("pub int answer;", "initialize(int id) -> void ::",
        "self.unknown = id;"), "unknown class field"),
    "call-before-full-initialization": (class_source("pub int answer;", "initialize(int id) -> void ::",
        "publish(self);\n    self.answer = id;"), "expected"),
    "early-return": (class_source("pub int answer;", "initialize(int id) -> void ::",
        "return;\n    self.answer = id;"), "complete field assignment"),
    "implicit-val-reassign": ("pub define class Connection as\n  pub int answer: 40;\nend\n"
        "pub fnc main(): int { let mut Connection c = new Connection(); c.answer = 42; return c.answer; }\n",
        "final class field"),
    "unproven-branch": (class_source("pub int answer;", "initialize(int id) -> void ::",
        "if id eq 0 then self.answer = id; fi"), "expected"),
}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--compiler",required=True)
    ap.add_argument("--clang",required=True)
    opts=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-typed-initialize-") as work:
        root=Path(work)
        for name,source in POSITIVE.items():
            case=root/name
            case.mkdir()
            src=case/"main.ores"
            src.write_text(source,encoding="utf-8")
            comp=run([opts.compiler,str(src)])
            if comp.returncode != 0 or comp.stderr or not comp.stdout.startswith("; ModuleID"):
                raise AssertionError((name,"compile",comp.returncode,comp.stderr,comp.stdout[:180]))
            ir=case/"main.ll"
            ir.write_text(comp.stdout,encoding="utf-8")
            exe=case/"run"
            linked=run([opts.clang,"-O1","-x","ir",str(ir),"-o",str(exe)])
            if linked.returncode != 0:
                raise AssertionError((name,"link",linked.stderr))
            actual=run([str(exe)])
            if actual.returncode != 42 or actual.stderr:
                raise AssertionError((name,"execute",actual.returncode,actual.stdout,actual.stderr))
        for name,(source,expected) in NEGATIVE.items():
            case=root/("reject-"+name)
            case.mkdir()
            src=case/"main.ores"
            src.write_text(source,encoding="utf-8")
            rejected=run([opts.compiler,str(src)])
            if rejected.returncode != 1 or rejected.stdout or expected not in rejected.stderr:
                raise AssertionError((name,"negative-admission",
                    rejected.returncode,rejected.stdout[:120],rejected.stderr))
    print(f"PASS native implicit VAL and initialize subset: {len(POSITIVE)} executed, {len(NEGATIVE)} rejected")

if __name__ == "__main__":
    main()
