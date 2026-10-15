#!/usr/bin/env python3
"""Compile state scopes, execute real heap turns, optionally compare JVM gates.

The JVM reference is an explicit Git revision, never inferred from a stale
working-tree build. Only production parser/state/effect verifier sources are
compiled; this does not claim full Graal interpreter or Native Image execution.
"""
import argparse
import hashlib
import json
import pathlib
import random
import subprocess
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, timeout=90, **kwargs)


def checked(command, **kwargs):
    result = run(command, **kwargs)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result.stdout


def java_verdicts(args, cases, temp):
    ref = checked(["git", "-C", args.java_repo, "rev-parse", args.java_ref]).strip()
    base = "src/main/java/dev/oreslang/"
    files = ["ast/Ast.java", "parser/Lexer.java", "parser/Token.java", "parser/Parser.java",
             "types/ActorStateContract.java", "types/DeclaredEffectVerifier.java", "types/FunctionEffects.java"]
    sources = []
    identities = {}
    for name in files:
        contents = checked(["git", "-C", args.java_repo, "show", ref + ":" + base + name])
        path = temp / "java" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        sources.append(str(path))
        identities[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    runner = temp / "java" / "StateParity.java"
    runner.write_text('''import java.nio.file.*;
import dev.oreslang.parser.Parser;
import dev.oreslang.types.ActorStateContract;
import dev.oreslang.types.DeclaredEffectVerifier;
public class StateParity {
  public static void main(String[] files) throws Exception {
    for (String file : files) {
      try {
        var ast = Parser.parse(Files.readString(Path.of(file)));
        ActorStateContract.verify(ast);
        DeclaredEffectVerifier.verify(ast);
        System.out.println("ACCEPT");
      } catch (IllegalArgumentException failure) {
        System.out.println("REJECT " + failure.getMessage());
      }
    }
  }
}
''')
    classes = temp / "java-classes"
    checked(["javac", "-d", str(classes), *sources, str(runner)])
    paths = []
    for case in cases:
        path = temp / (case["name"] + ".ores")
        path.write_text(case["source"])
        paths.append(str(path))
    answers = checked(["java", "-cp", str(classes), "StateParity", *paths]).splitlines()
    assert len(answers) == len(cases), answers
    print(json.dumps({"java_reference_commit": ref, "java_source_sha256": identities}), flush=True)
    for case, answer in zip(cases, answers):
        if (answer == "ACCEPT") != case["accepted"]:
            print("JVM mismatch:", case["name"], answer, flush=True)
    return [answer == "ACCEPT" for answer in answers]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--java-repo")
    parser.add_argument("--java-ref", default="42dd7ceaadf43a4b467dd1d83f065dc50c84c0cd")
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    cases = json.loads((root / "tests/with_using_state_cases.json").read_text())
    with tempfile.TemporaryDirectory(prefix="with-using-parity-") as folder:
        temp = pathlib.Path(folder)
        reference = java_verdicts(args, cases, temp) if args.java_repo else None
        for index, case in enumerate(cases):
            source = temp / (case["name"] + ".ores")
            source.write_text(case["source"])
            compiled = run([args.compiler, str(source)])
            accepted = compiled.returncode == 0
            assert accepted == case["accepted"], (case["name"], compiled.stderr)
            if reference is not None:
                assert accepted == reference[index], (case["name"], "JVM", reference[index], "LLVM", accepted)
            if not accepted:
                assert not compiled.stdout.strip(), (case["name"], "partial IR on rejection")
                print("PASS rejected:", case["name"], flush=True)
                continue
            if case["result"] is None:
                print("PASS accepted:", case["name"], flush=True)
                continue
            ir = temp / "actor.ll"
            ir.write_text(compiled.stdout)
            # There must be no implicit snapshots in write-only acquisition,
            # nor stores through a read scope (aside from constructor init).
            body = compiled.stdout.split("define i64 @oreslang_actor_Counter_run", 1)[1]
            body = body.split("\n}", 1)[0]
            if case["name"] in {"write-only", "explicit-write", "noop-write"}:
                assert "call i64 @ores_actor_i64_heap_field_read" not in body, body
            if case["name"].startswith("read-") or case["name"] in {"handle-read", "explicit-read", "noop-read"}:
                assert "call void @ores_actor_i64_heap_field_write" not in body, body
            harness = temp / "harness.c"
            harness.write_text('''#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_run(OresActorI64Heap*,uint64_t,uint64_t);
int main(void) {
  OresActorI64Heap* heap = ores_actor_i64_heap_open(123, 7, 2);
  OresActorTurn init = ores_actor_i64_heap_enter(heap,123,7);
  oreslang_actor_Counter_init(heap,init.domain,init.epoch);
  ores_actor_i64_heap_leave(heap,init);
  OresActorTurn next = ores_actor_i64_heap_enter(heap,123,7);
  int64_t result = oreslang_actor_Counter_run(heap,next.domain,next.epoch);
  ores_actor_i64_heap_leave(heap,next);
  ores_actor_i64_heap_retire(heap);
  if (ores_actor_i64_heap_try_destroy(heap) != 1) return 81;
  return result == EXPECTED ? 0 : 82;
}
'''.replace("EXPECTED", str(case["result"])))
            binary = temp / "actor"
            sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
            checked([args.clang, "-O2", *sanitizer, str(ir), str(harness),
                     str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
                     "-I" + str(root / "src/main/cpp"), "-o", str(binary)])
            checked([str(binary)])
            print("PASS executed:", case["name"], flush=True)
        # Independent scalar-state model: randomized sequential assignments
        # catch stale eager snapshots and uninitialized first-use projections.
        rng = random.Random(0x517A7E)
        methods, declarations, entries = [], [], []
        for index in range(32):
            state = {"x": 2, "y": 3, "z": 5}
            statements = []
            for _ in range(8):
                target, left, right = [rng.choice(list(state)) for _ in range(3)]
                statements.append(f"s.{target} = s.{left} + s.{right} + amount;")
                state[target] = state[left] + state[right] + 2
            methods.append(f"case_{index}(int amount): int with state.read, state.write {{"
                           + "return using state.write(critical |s| -> {"
                           + " ".join(statements) + " return s.x + s.y + s.z; }); }")
            declarations.append(f"extern int64_t oreslang_actor_Counter_case_{index}(OresActorI64Heap*,uint64_t,uint64_t,int64_t);")
            entries.append(f"{{oreslang_actor_Counter_case_{index}, {sum(state.values())}}}")
        model_source = temp / "model.ores"
        model_source.write_text("define actor Counter as state { x: int = 2; y: int = 3; z: int = 5; }"
                                + "\n".join(methods) + " end")
        ir = temp / "model.ll"
        ir.write_text(checked([args.compiler, str(model_source)]))
        harness = temp / "model.c"
        harness.write_text('''#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*,uint64_t,uint64_t);
DECLARATIONS
struct Case { int64_t (*invoke)(OresActorI64Heap*,uint64_t,uint64_t,int64_t); int64_t expected; };
int main(void) {
  const struct Case cases[] = { ENTRIES };
  for (unsigned i=0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(123,7,3);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap,123,7);
    oreslang_actor_Counter_init(heap,turn.domain,turn.epoch);
    ores_actor_i64_heap_leave(heap,turn);
    turn = ores_actor_i64_heap_enter(heap,123,7);
    int64_t result = cases[i].invoke(heap,turn.domain,turn.epoch,2);
    ores_actor_i64_heap_leave(heap,turn);
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1 || result != cases[i].expected) return 83;
  }
  return 0;
}
'''.replace("DECLARATIONS", "\n".join(declarations)).replace("ENTRIES", ",".join(entries)))
        binary = temp / "model"
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        checked([args.clang, "-O2", *sanitizer, str(ir), str(harness),
                 str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
                 "-I" + str(root / "src/main/cpp"), "-o", str(binary)])
        checked([str(binary)])
        print("PASS 32 compiled sequential state histories against independent model", flush=True)
        fixture = root / "tests/fixtures/actor-with-using-methods.ores"
        ir = temp / "methods.ll"
        ir.write_text(checked([args.compiler, str(fixture)]))
        binary = temp / "method-turns"
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        checked([args.clang, "-O2", "-pthread", *sanitizer, str(ir),
                 str(root / "tests/with_using_method_turns.c"),
                 str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
                 "-I" + str(root / "src/main/cpp"), "-o", str(binary)])
        checked([str(binary)])
        print("PASS state methods across four separate actor turns and carriers", flush=True)
    print(f"PASS {len(cases)} native state capability cases" + (" matched JVM gates" if reference else ""))


if __name__ == "__main__":
    main()
