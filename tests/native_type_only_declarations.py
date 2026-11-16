#!/usr/bin/env python3
"""Exact-source LLVM parser/linker admission tests for compiler-only nominal kinds.

The test requires LLVM IR on success and no LLVM IR on failure. Scalar aliases
erase to the checked int/bool ABI; trait/interface objects, cross-unit imports
and general models remain unsupported.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def compile_source(compiler, source_text, target):
    target.write_text(source_text, encoding="utf-8")
    return subprocess.run(
        [compiler, str(target)], capture_output=True, text=True, timeout=40
    )


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--compiler", required=True)
    opts = args.parse_args()
    main_fn = "\npub fnc main(): int { return 0; }\n"
    positive = [
        ("canonical private and public type aliases",
         "declare type LocalId = int;\ndeclare pub type PublicId = bool;\n" + main_fn),
        ("root trait and interface signatures",
         "declare trait Tagged as\n fnc tag(): int;\nend\n"
         "declare pub interface Endpoint as\n fnc request() => bool;\nend\n" + main_fn),
        ("module-contained public declarations",
         "define module api as\n"
         " declare pub type Id = int;\n"
         " declare pub trait Tagged as\n fnc tag(int value) => int;\nend\n"
         " declare pub interface Endpoint as\n fnc request() => int;\nend\n"
         " pub fnc main(): int { return 0; }\nend\n"),
        ("legacy define spellings",
         "define type Id = int;\ndefine trait Tagged as end\n"
         "define interface Endpoint as end\n" + main_fn),
        ("compatibility public visibility order",
         "pub declare type Id = int;\npub declare trait Tagged as end\n"
         "pub declare interface Endpoint as end\n" + main_fn),
        ("legacy public defined types inside a module",
         "define module api as\n"
         " pub define type Id = int;\n"
         " pub define trait Tagged as end\n"
         " pub define interface Endpoint as end\n"
         " pub fnc main(): int { return 0; }\nend\n"),
        ("same-kind inheritance",
         "declare trait Parent as end\n"
         "declare pub trait Child extends Parent as end\n"
         "declare interface Api as end\n"
         "declare interface Extended extends Api as end\n" + main_fn),
        ("contextual declare function identifier",
         "fnc declare(): int { return 0; }\n" + main_fn),
        ("method arity-overload signatures",
         "declare interface Api as\n"
         " fnc get() => int;\n fnc get(int key) => int;\nend\n" + main_fn),
        ("named type-only method formals",
         "declare trait Tagged { fnc tag(value: int): bool; }\n" + main_fn),
    ]

    positive.extend([
        ("transparent int alias in function parameter and return",
         "declare type UserId = int;\n"
         "fnc identity(UserId id): UserId { return id; }\n"
         "pub fnc main(): int { return identity(42); }\n"),
        ("transparent bool alias in function parameter and return",
         "declare type Flag = bool;\n"
         "fnc identity(Flag flag): Flag { return flag; }\n"
         "pub fnc main(): int { return 1; }\n"),
        ("alias chaining preserves scalar ABI",
         "declare type BaseId = int;\n"
         "declare pub type UserId = BaseId;\n"
         "fnc identity(UserId id): BaseId { return id; }\n"
         "pub fnc main(): int { return identity(7); }\n"),
        ("forward target aliases validated before IR",
         "declare type Outer = Inner;\n"
         "declare type Inner = int;\n"
         "fnc identity(Outer id): Inner { return id; }\n"
         "pub fnc main(): int { return identity(9); }\n"),
        ("aliases accepted in typed lexical locals",
         "declare type Count = int;\n"
         "pub fnc main(): int { val total: Count = 7; return total; }\n"),
        ("aliases accepted in type-only method signatures",
         "declare type Count = int;\n"
         "declare interface Api as fnc get(Count id): Count; end\n"
         + main_fn),
        ("bool alias accepted in typed lexical local",
         "declare type Ready = bool;\n"
         "pub fnc main(): int { const ready: Ready = true; return 1; }\n"),
        ("int alias accepted in generic call argument",
         "declare type Id = int;\n"
         "fnc identity<T>(T value): T { return value; }\n"
         "pub fnc main(): int { return identity<Id>(8); }\n"),
        ("int alias accepted in generic default type",
         "declare type Id = int;\n"
         "fnc identity<T=Id>(T value): T { return value; }\n"
         "pub fnc main(): int { return identity(8); }\n"),
    ])
    positive.extend([
        ("declare complete alias equivalent to define",
         "declare type Code = int;\n"
         "fnc identity(Code value): Code { return value; }\n"
         "pub fnc main(): int { return identity(3); }\n"),
        ("define complete alias equivalent to declare",
         "define type Code = int;\n"
         "fnc identity(Code value): Code { return value; }\n"
         "pub fnc main(): int { return identity(3); }\n"),
        ("declare and define complete bool aliases",
         "declare type Flag = bool;\n"
         "define type Count = int;\n"
         "fnc identity(Flag value): Flag { return value; }\n"
         "pub fnc main(): int { val n: Count = 7; return n; }\n"),
        ("completed aliases support transitive targets",
         "declare type UserId = Code;\n"
         "define type Code = int;\n"
         "fnc identity(UserId value): UserId { return value; }\n"
         "pub fnc main(): int { return identity(7); }\n"),
        ("complete aliases in interface method signatures",
         "declare type Code = int;\n"
         "declare interface Api as fnc lookup(Code id): Code; end\n" + main_fn),
    ])
    negative = [
        ("reject concrete declare model",
         "declare model M as Pass() end\n" + main_fn, "TYPE_ONLY_INVALID_KIND"),
        ("reject concrete declare actor",
         "declare actor A as end\n" + main_fn, "NATIVE_ACTOR_FORWARD_BODY"),
        ("reject unsupported scalar alias target",
         "declare type Id = string;\n" + main_fn, "TYPE_ALIAS_UNKNOWN_TARGET"),
        ("reject generic trait without native contract proof",
         "declare trait Generic<T> as end\n" + main_fn, None),
        ("reject trait extends interface",
         "declare interface Parent as end\ndeclare trait Child extends Parent as end\n"
         + main_fn, "TYPE_PARENT_KIND_MISMATCH"),
        ("reject interface extends trait",
         "declare trait Parent as end\ndeclare interface Child extends Parent as end\n"
         + main_fn, "TYPE_PARENT_KIND_MISMATCH"),
        ("reject unknown trait parent",
         "declare trait Child extends Missing as end\n" + main_fn, "TYPE_PARENT_UNKNOWN"),
        ("reject cycles",
         "declare trait A extends B as end\ndeclare trait B extends A as end\n"
         + main_fn, "TYPE_PARENT_CYCLE"),
        ("reject duplicate type declarations",
         "declare type Item = int;\ndeclare interface Item as end\n"
         + main_fn, "TYPE_DECL_NAME_COLLISION"),
        ("reject interface/class name collision",
         "declare interface Item as end\ndefine class Item as\n"
         " pub let int field = 1;\nend\n"
         + main_fn, "TYPE_DECL_NAME_COLLISION"),
        ("reject duplicate method overload slot",
         "declare trait Api as\n fnc read() => int;\n fnc read(): bool;\nend\n"
         + main_fn, "TYPE_ONLY_DUPLICATE_MEMBER"),
        ("reject invalid signature arrow",
         "declare interface Api as\n fnc read() = > int;\nend\n"
         + main_fn, "TYPE_ONLY_BAD_ARROW"),
        ("reject duplicate visibility",
         "declare pub pub interface Bad as end\n" + main_fn, "TYPE_ONLY_DUPLICATE_VISIBILITY"),
        ("reject runtime use of type-only trait",
         "declare trait Tagged as end\npub fnc main(): int { return Tagged; }\n",
         None),
        ("reject LLVM model execution until parity verified",
         "define model M as Pass() end\n" + main_fn, "MODEL_BACKEND_UNSUPPORTED"),
        ("reject effectful trait method body",
         "declare trait Api as fnc read(): int { return 1; } end\n" + main_fn,
         None),
    ]

    negative.extend([
        ("missing scalar alias target fails even if unused",
         "declare type Broken = NotDefined;\n" + main_fn,
         "TYPE_ALIAS_UNKNOWN_TARGET"),
        ("unused alias cycles fail before codegen",
         "declare type First = Second;\n"
         "declare type Second = First;\n" + main_fn,
         "TYPE_ALIAS_CYCLE"),
        ("self-referential alias fails",
         "declare type Self = Self;\n" + main_fn, "TYPE_ALIAS_CYCLE"),
        ("alias cannot target trait",
         "declare trait Tagged as end\n"
         "declare type Item = Tagged;\n" + main_fn,
         "TYPE_ALIAS_KIND_MISMATCH"),
        ("alias cannot target interface",
         "declare interface Api as end\n"
         "declare type Item = Api;\n" + main_fn,
         "TYPE_ALIAS_KIND_MISMATCH"),
        ("bool alias cannot bypass borrowing prohibition",
         "declare type Flag = bool;\n"
         "fnc invalid(borrow Flag value): Flag { return value; }\n" + main_fn,
         "native bool borrow ABI"),
        ("bool alias preserves return type checking",
         "declare type Flag = bool;\n"
         "fnc wrong(): Flag { return 1; }\n" + main_fn,
         None),
        ("alias cannot create a type-only runtime value",
         "declare type Id = int;\n"
         "pub fnc main(): int { return Id; }\n",
         None),
        ("unknown alias cannot be used in a native scalar local",
         "pub fnc main(): int { val total: Missing = 1; return total; }\n",
         None),
        ("compiler-owned Entry cannot be redefined as scalar alias",
         "declare type Entry = int;\n" + main_fn, "TYPE_DECL_RESERVED_BUILTIN"),
        ("compiler-owned Callable cannot be redefined as interface",
         "declare interface Callable as end\n" + main_fn, "TYPE_DECL_RESERVED_BUILTIN"),
        ("bool alias cannot be a native i64 generic argument",
         "declare type Flag = bool;\n"
         "fnc identity<T>(T value): int { return value; }\n"
         "pub fnc main(): int { return identity<Flag>(8); }\n",
         "native scalar generic type requires int ABI"),
        ("bool alias cannot be a native i64 generic default",
         "declare type Flag = bool;\n"
         "fnc identity<T=Flag>(T value): int { return value; }\n"
         + main_fn, "native scalar generic default requires int ABI"),
    ])

    negative.extend([
        ("reject duplicate formal names",
         "declare trait Api as fnc read(int id, int id) => int; end\n"
         + main_fn, "TYPE_ONLY_DUPLICATE_PARAM"),
        ("reject type catalog exhaustion",
         "".join(f"declare type T{i} = int;\n" for i in range(1025))
         + main_fn, "TYPE_ONLY_DECL_BUDGET"),
        ("reject inheritance depth exhaustion",
         "".join(
             f"declare trait T{i} extends T{i - 1} as end\n"
             for i in range(128, 0, -1)
         ) + "declare trait T0 as end\n" + main_fn,
         "TYPE_PARENT_DEPTH_BUDGET"),
    ])
    positive.extend([
        ("native root alias extension dispatch",
         "declare type Code = int;\n"
         "define mtd double on Code(): int { return self + self; }\n"
         "fnc apply(Code code): int { return code.double(); }\n"
         "pub fnc main(): int { return apply(21); }\n"),
        ("native module alias extension dispatch",
         "define module api as\n"
         " declare type Code = int;\n"
         " define mtd double on Code(): int { return self + self; }\n"
         " pub fnc main(): int { val code: Code = 21; return code.double(); }\nend\n"),
    ])
    negative.extend([
        ("declare alias requires full definition", "declare type Code;\n" + main_fn,
         "TYPE_ONLY_INCOMPLETE_DEFINITION"),
        ("define alias requires full definition", "define type Code;\n" + main_fn,
         "TYPE_ONLY_INCOMPLETE_DEFINITION"),
        ("declare public alias requires full definition",
         "declare pub type Code;\n" + main_fn, "TYPE_ONLY_INCOMPLETE_DEFINITION"),
        ("define public alias requires full definition",
         "define pub type Code;\n" + main_fn, "TYPE_ONLY_INCOMPLETE_DEFINITION"),
        ("declare then define same name collide",
         "declare type Code = int;\ndefine type Code = bool;\n" + main_fn,
         "TYPE_DECL_NAME_COLLISION"),
        ("define then declare same name collide",
         "define type Code = int;\ndeclare type Code = bool;\n" + main_fn,
         "TYPE_DECL_NAME_COLLISION"),
        ("completed alias cannot hide cycle",
         "declare type Code = Other;\ndefine type Other = Code;\n" + main_fn,
         "TYPE_ALIAS_CYCLE"),
        ("completed alias cannot target unknown type",
         "declare type Code = Unknown;\n" + main_fn,
         "TYPE_ALIAS_UNKNOWN_TARGET"),
        ("bool alias cannot bypass borrow rule",
         "declare type Flag = bool;\n"
         "fnc unchecked_flag(borrow Flag value): int { return value; }\n" + main_fn,
         "native bool borrow ABI"),
    ])
    with tempfile.TemporaryDirectory(prefix="ores-native-typeonly-") as root:
        source = Path(root) / "typeonly.ores"
        equivalent_ir = {}
        for label, program in positive:
            result = compile_source(opts.compiler, program, source)
            if result.returncode or result.stderr or not result.stdout.startswith("; ModuleID"):
                raise AssertionError((label, result.returncode, result.stderr, result.stdout[:500]))
            if label in (
                "declare complete alias equivalent to define",
                "define complete alias equivalent to declare",
            ):
                equivalent_ir[label] = result.stdout
            # Type-only declarations cannot silently lower a named runtime value.
            for forbidden in (
                "@Tagged", "@Endpoint", "@LocalId", "@PublicId",
                "@UserId", "@BaseId", "@Outer", "@Inner", "@Flag",
                "@Ready", "@Count", "@Id", "@Code", "@UserId", "@Flag"
            ):
                if forbidden in result.stdout:
                    raise AssertionError((label, "type-only runtime emission", forbidden))
            if label in (
                "transparent int alias in function parameter and return",
                "transparent bool alias in function parameter and return",
                "alias chaining preserves scalar ABI",
                "forward target aliases validated before IR",
            ):
                # Both scalar kinds use the native i64 transport representation.
                # A nominal object or extra pointer slot would violate ABI parity.
                if "define i64 @identity(i64 " not in result.stdout:
                    raise AssertionError((label, "scalar alias changed ABI", result.stdout[:800]))
        if equivalent_ir.get("declare complete alias equivalent to define") != equivalent_ir.get(
            "define complete alias equivalent to declare"
        ):
            raise AssertionError("declare/define type must emit identical LLVM IR")
        for label, program, diagnostic in negative:
            result = compile_source(opts.compiler, program, source)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError((label, "invalid source admitted", result.stdout[:500],
                                      result.stderr[:500]))
            if diagnostic and diagnostic not in result.stderr:
                raise AssertionError((label, "wrong diagnostic", diagnostic, result.stderr))
    print(f"PASS {len(positive)} positive and {len(negative)} negative native type-only cases")


if __name__ == "__main__":
    main()
