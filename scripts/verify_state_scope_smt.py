#!/usr/bin/env python3
"""Fail-closed LLVM-SSA -> QF_BV verification of production admission checks.

Supported instructions are deliberately small, pure, total bit-vector ops.
Memory, poison/undef, loops, flagged arithmetic and unknown calls are rejected.
The turn wrapper has one allowed effect: the production abort rejection edge.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result.stdout


def bv(value, width):
    return f"(_ bv{value % (1 << width)} {width})"


def truth(value):
    return f"(= {value} {bv(1, 1)})"


def translate(ir, name):
    function = re.search(r"^define [^\n]*@" + re.escape(name) + r"\((.*?)\).*?\{\n(.*?)^}",
                         ir, re.M | re.S)
    if not function:
        raise ValueError("missing proof function " + name)
    arguments = []
    for argument in function[1].split(","):
        match = re.fullmatch(r"i(\d+) (?:[\w()]+ )*%([\w.$-]+)", argument.strip())
        if not match:
            raise ValueError("unsupported function input: " + argument)
        arguments.append((match[2], int(match[1])))
    blocks, current = {}, None
    normalized, pending = [], []
    for raw in function[2].splitlines():
        line = raw.split(";", 1)[0].strip()
        if pending:
            pending.append(line)
            if line == "]":
                normalized.append(" ".join(pending))
                pending = []
        elif line.startswith("switch ") and line.endswith("["):
            pending = [line]
        else:
            normalized.append(line)
    if pending:
        raise ValueError("unterminated switch")
    for raw in normalized:
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        label = re.fullmatch(r"([\w.$-]+):", line)
        if label:
            current = label[1]
            if current in blocks:
                raise ValueError("duplicate block")
            blocks[current] = []
        elif current is None:
            raise ValueError("instruction outside block")
        else:
            blocks[current].append(line)
    if not blocks:
        raise ValueError("empty function")
    terminals = []

    def value(token, width, env):
        token = token.strip()
        if token.startswith("%"):
            result, actual_width = env[token[1:]]
            if actual_width != width:
                raise ValueError("type mismatch")
            return result
        if token in ("true", "false") and width == 1:
            return bv(int(token == "true"), 1)
        if re.fullmatch(r"-?\d+", token):
            return bv(int(token), width)
        raise ValueError("unsupported/undefined value: " + token)

    def visit(label, predecessor, env, path, visited):
        if label in visited:
            raise ValueError("loop in proof function")
        env = dict(env)
        lines = blocks[label]
        for index, line in enumerate(lines):
            if line == "unreachable":
                if index == 0 or not re.fullmatch(r"(?:tail )?call void @abort\(\)(?: #\d+)?", lines[index - 1]):
                    raise ValueError("unreachable without a modeled rejection")
                if index != len(lines) - 1:
                    raise ValueError("instructions after terminator")
                terminals.append((path, bv(0, 32)))
                return
            if re.fullmatch(r"(?:tail )?call void @abort\(\)(?: #\d+)?", line):
                if index + 1 != len(lines) - 1 or lines[index + 1] != "unreachable":
                    raise ValueError("abort must be terminal")
                continue
            ret = re.fullmatch(r"ret i32 (.+)", line)
            if ret:
                if index != len(lines) - 1:
                    raise ValueError("instructions after return")
                terminals.append((path, value(ret[1], 32, env)))
                return
            switch = re.fullmatch(r"switch i(\d+) ([^,]+), label %([\w.$-]+) \[(.*)\]", line)
            if switch:
                if index != len(lines) - 1:
                    raise ValueError("instructions after switch")
                width = int(switch[1])
                operand = value(switch[2], width, env)
                cases = re.findall(r"i(\d+) (-?\d+), label %([\w.$-]+)", switch[4])
                if re.sub(r"i(\d+) (-?\d+), label %([\w.$-]+)", "", switch[4]).strip():
                    raise ValueError("unsupported switch case")
                conditions, unique = [], set()
                for case_width, literal, target in cases:
                    constant = bv(int(literal), width)
                    if int(case_width) != width or constant in unique:
                        raise ValueError("invalid switch cases")
                    unique.add(constant)
                    condition = f"(= {operand} {constant})"
                    conditions.append(condition)
                    visit(target, label, env, f"(and {path} {condition})", visited | {label})
                others = "(or " + " ".join(conditions) + ")"
                visit(switch[3], label, env, f"(and {path} (not {others}))", visited | {label})
                return
            branch = re.fullmatch(r"br label %([\w.$-]+)", line)
            conditional = re.fullmatch(r"br i1 (.+), label %([\w.$-]+), label %([\w.$-]+)", line)
            if branch or conditional:
                if index != len(lines) - 1:
                    raise ValueError("instructions after branch")
                if branch:
                    visit(branch[1], label, env, path, visited | {label})
                else:
                    condition = truth(value(conditional[1], 1, env))
                    visit(conditional[2], label, env, f"(and {path} {condition})", visited | {label})
                    visit(conditional[3], label, env, f"(and {path} (not {condition}))", visited | {label})
                return
            assignment = re.fullmatch(r"%([\w.$-]+) = (.+)", line)
            if not assignment:
                raise ValueError("unsupported instruction: " + line)
            destination, operation = assignment.groups()
            binary = re.fullmatch(r"(and|or|xor|add|sub) i(\d+) ([^,]+), (.+)", operation)
            compare = re.fullmatch(r"icmp (eq|ne|ult|ule|ugt|uge|slt|sle|sgt|sge) i(\d+) ([^,]+), (.+)", operation)
            cast = re.fullmatch(r"(zext|trunc) i(\d+) (.+) to i(\d+)", operation)
            select = re.fullmatch(r"select i1 ([^,]+), i(\d+) ([^,]+), i(\d+) (.+)", operation)
            phi = re.fullmatch(r"phi i(\d+) (.+)", operation)
            if binary:
                op, width, left, right = binary.groups()
                width = int(width)
                expression = f"(bv{op} {value(left, width, env)} {value(right, width, env)})"
            elif compare:
                op, width, left, right = compare.groups()
                width = int(width)
                left, right = value(left, width, env), value(right, width, env)
                predicate = f"(= {left} {right})" if op in ("eq", "ne") else f"(bv{op} {left} {right})"
                if op == "ne":
                    predicate = f"(not {predicate})"
                expression, width = f"(ite {predicate} {bv(1, 1)} {bv(0, 1)})", 1
            elif cast:
                op, before, operand, after = cast.groups()
                before, after = int(before), int(after)
                operand = value(operand, before, env)
                if op == "zext" and after > before:
                    expression = f"((_ zero_extend {after - before}) {operand})"
                elif op == "trunc" and after < before:
                    expression = f"((_ extract {after - 1} 0) {operand})"
                else:
                    raise ValueError("invalid cast")
                width = after
            elif select:
                condition, width, left, other_width, right = select.groups()
                width = int(width)
                if width != int(other_width):
                    raise ValueError("select type mismatch")
                expression = f"(ite {truth(value(condition, 1, env))} {value(left, width, env)} {value(right, width, env)})"
            elif phi:
                width = int(phi[1])
                entries = re.findall(r"\[\s*([^,\]]+),\s*%([\w.$-]+)\s*\]", phi[2])
                if re.sub(r"\[\s*([^,\]]+),\s*%([\w.$-]+)\s*\]", "", phi[2]).replace(",", "").strip():
                    raise ValueError("unsupported phi")
                selected = [operand for operand, origin in entries if origin == predecessor]
                if len(selected) != 1:
                    raise ValueError("missing/ambiguous phi predecessor")
                expression = value(selected[0], width, env)
            else:
                raise ValueError("unsupported instruction: " + operation)
            if destination in env:
                raise ValueError("SSA redefinition")
            env[destination] = (expression, width)
        raise ValueError("block without terminator")

    visit(next(iter(blocks)), None, {key: (key, width) for key, width in arguments}, "true", set())
    result = bv(0, 32)
    for path, terminal in reversed(terminals):
        result = f"(ite {path} {terminal} {result})"
    coverage = "(or " + " ".join(path for path, _ in terminals) + ")"
    declarations = "\n".join(f"(declare-fun {key} () (_ BitVec {width}))" for key, width in arguments)
    return arguments, declarations, result, coverage


def contracts(kind):
    if kind == "state":
        flag = lambda bit: f"(not (= (bvand flags {bv(bit, 8)}) {bv(0, 8)}))"
        effect = lambda bit: f"(not (= (bvand inferred {bv(bit, 32)}) {bv(0, 32)}))"
        permission = lambda bit: f"(not (= (bvand allowed {bv(bit, 32)}) {bv(0, 32)}))"
        return "(and " + " ".join([
            flag(1), flag(4), flag(8), f"(not {flag(16)})", f"(not {flag(32)})",
            f"(bvult allowed {bv(1024, 32)})", f"(bvult inferred {bv(1024, 32)})",
            f"(= (bvand allowed inferred) inferred)", f"(not {effect(4)})",
            f"(or (and (= mode {bv(1, 8)}) (not {effect(2)}) {permission(1)})"
            f" (and (= mode {bv(2, 8)}) {flag(2)} {permission(2)}))",
        ]) + ")"
    return f"(and (not (= active {bv(0, 32)})) (= retiring {bv(0, 32)}) (= live_domain requested_domain) (= live_epoch requested_epoch))"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--z3", default="z3")
    args = parser.parse_args()
    root, out = pathlib.Path(args.root).resolve(), pathlib.Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").unlink(missing_ok=True)
    include = root / "src/main/cpp"
    sources = root / "spec/formal/state-scopes"
    policy = "oreslang/types/CriticalHandleStateContract.hpp"
    heap = "oreslang/runtime/ActorOwnedI64Heap.c"
    reports = []

    def verify(kind, name, replacement=None):
        directory = out / name
        directory.mkdir(exist_ok=True)
        command = [args.cxx if kind == "state" else args.clang,
                   "-std=c++17" if kind == "state" else "-std=c11", "-O2", "-S", "-emit-llvm",
                   "-fno-discard-value-names"]
        if replacement:
            file, old, new = replacement
            original = (include / file).read_text()
            if original.count(old) != 1:
                raise AssertionError("mutation source changed: " + name)
            overlay = directory / "overlay"
            path = overlay / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(original.replace(old, new))
            command += ["-I" + str(overlay)]
        probe = sources / ("admission_probe.cpp" if kind == "state" else "turn_probe.c")
        ir_path = directory / "predicate.ll"
        command += ["-I" + str(include), str(probe), "-o", str(ir_path)]
        run(command)
        arguments, declarations, expression, coverage = translate(ir_path.read_text(),
                                                                  "state_admits" if kind == "state" else "turn_admits")
        base = "(set-logic QF_BV)\n" + declarations + f"\n(define-fun impl_result () (_ BitVec 32) {expression})\n"
        base += f"(define-fun impl_ok () Bool (= impl_result {bv(1, 32)}))\n"
        base += f"(define-fun spec_ok () Bool {contracts(kind)})\n"
        queries = [("coverage", f"(not {coverage})", "unsat"),
                   ("boolean_result", f"(not (or (= impl_result {bv(0, 32)}) (= impl_result {bv(1, 32)})))", "unsat")]
        queries.append(("unsafe_acceptance" if replacement else "contract_equivalence",
                        "(and impl_ok (not spec_ok))" if replacement else "(not (= impl_ok spec_ok))",
                        "sat" if replacement else "unsat"))
        record = {"name": name, "kind": kind, "llvm_sha256": hashlib.sha256(ir_path.read_bytes()).hexdigest(), "checks": []}
        witness = None
        for theorem, violation, expected in queries:
            extra = "(get-value (" + " ".join(key for key, _ in arguments) + " impl_ok spec_ok))" if expected == "sat" else "(get-proof)"
            text = "(set-option :produce-proofs true)\n" + base + f"(assert {violation})\n(check-sat)\n" + extra + "\n"
            query = directory / (theorem + ".smt2")
            query.write_text(text)
            result = run([args.z3, "-smt2", str(query)])
            (directory / (theorem + ".result")).write_text(result)
            if result.splitlines()[0] != expected or "(error" in result or "unknown" == result.splitlines()[0]:
                raise AssertionError((name, theorem, result))
            record["checks"].append({"theorem": theorem, "result": expected})
            if expected == "sat":
                witness = {}
                for key, width in arguments:
                    match = re.search(r"\(" + key + r"\s+(#x[0-9a-f]+|#b[01]+)\)", result)
                    if not match:
                        raise AssertionError("missing concrete witness for " + key)
                    witness[key] = int(match[1][2:], 16 if match[1][1] == "x" else 2)
        if replacement:
            # Replay the solver's unsafe-acceptance witness against a native
            # executable compiled from exactly the same mutated source overlay.
            replay = directory / ("replay.cpp" if kind == "state" else "replay.c")
            values = [str(witness[key]) + ("ULL" if width == 64 else "U") for key, width in arguments]
            signature = ("extern \"C\" unsigned state_admits(unsigned char,unsigned char,unsigned,unsigned,unsigned char);"
                         if kind == "state" else
                         "#include <stdint.h>\nextern int turn_admits(uint64_t,uint64_t,uint64_t,uint64_t,int,int);")
            function_name = "state_admits" if kind == "state" else "turn_admits"
            replay.write_text(signature + "\nint main(void) { return " + function_name
                              + "(" + ",".join(values) + ") == 1 ? 0 : 1; }\n")
            binary = directory / "replay"
            run([args.cxx if kind == "state" else args.clang, "-O2", "-I" + str(directory / "overlay"),
                 "-I" + str(include), str(probe), str(replay), "-o", str(binary)])
            run([str(binary)])
            record.update(witness=witness, counterexample_replayed=True)
        reports.append(record)
        print("PASS", name, json.dumps(record["checks"]), flush=True)

    verify("state", "production_state")
    verify("turn", "production_turn")
    verify("state", "mutant_read_write", (policy,
        "if (mode == CriticalStateMode::Read\n            && callback.inferred.has(FunctionEffect::StateWrite))",
        "if (false)"))
    verify("state", "mutant_suspend", (policy,
        "if (callback.inferred.has(FunctionEffect::Suspend)\n            || callback.state_loan_live_across_suspend)", "if (false)"))
    verify("state", "mutant_noop_permission", (policy,
        "if ((mode == CriticalStateMode::Read\n                && !with_effects.has(FunctionEffect::StateRead))\n            || (mode == CriticalStateMode::Write\n                && !with_effects.has(FunctionEffect::StateWrite)))", "if (false)"))
    verify("turn", "mutant_epoch", (heap, " || heap->turn_epoch != turn.epoch", ""))
    verify("turn", "mutant_retirement", (heap, "heap->retiring || !heap->turn_active", "!heap->turn_active"))
    verify("turn", "mutant_active", (heap, "heap->retiring || !heap->turn_active", "heap->retiring"))
    # Fixed-width monotonicity lemma: the guarded uint64 increment cannot
    # wrap or reuse zero. This lemma assumes the production exhaustion guard.
    lemma = out / "guarded_nonce_increment.smt2"
    lemma.write_text("(set-option :produce-proofs true)\n(set-logic QF_BV)\n"
        "(declare-fun old () (_ BitVec 64))\n"
        f"(assert (not (= old {bv((1 << 64) - 1, 64)})))\n"
        f"(assert (not (bvugt (bvadd old {bv(1, 64)}) old)))\n(check-sat)\n(get-proof)\n")
    result = run([args.z3, "-smt2", str(lemma)])
    if result.splitlines()[0] != "unsat":
        raise AssertionError(result)
    (out / "guarded_nonce_increment.result").write_text(result)
    summary = {"z3": run([args.z3, "-version"]).strip(),
               "clang": run([args.clang, "--version"]).splitlines()[0],
               "cxx": run([args.cxx, "--version"]).splitlines()[0],
               "production_sha256": {file: hashlib.sha256((include / file).read_bytes()).hexdigest() for file in (policy, heap)},
               "guarded_nonce_increment": "unsat", "predicates": reports}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
