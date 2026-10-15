#!/usr/bin/env python3
"""Compiler source-actor admission; never silently fall back to host code."""
import argparse
import pathlib
import subprocess
import tempfile


def verify(ok, message):
    if not ok:
        raise RuntimeError(message)


def compile_guest(compiler, path):
    return subprocess.run([compiler, "--target=thumbv7m-none-eabi", str(path)],
                          capture_output=True, text=True, timeout=60)


def main():
    p=argparse.ArgumentParser()
    p.add_argument("--compiler",required=True)
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-cortexm-actors-") as d:
        source=pathlib.Path(d)/"actors.ores"
        actor=lambda name: (f"define actor {name} as {{\n"
          "let total: int = 0;\n"
          "pub fnc on_message(int n): int { total = total + n; return total; }\n"
          "}\n")
        bootstrap=("pub fnc main(): int {\n"
          "const a = spawn Producer();\nconst b = spawn Consumer();\n"
          "a.send(1);\nb.send(2);\nstop a;\nstop b;\nreturn 43;\n}\n")
        source.write_text(actor("Producer")+actor("Consumer")+bootstrap)
        good=compile_guest(a.compiler,source)
        verify(good.returncode==0,good.stderr)
        for name in ("oreslang_cortexm_bootstrap",
                     "oreslang_actor_Producer_init",
                     "oreslang_actor_Producer_on_message",
                     "oreslang_actor_Consumer_init",
                     "oreslang_actor_Consumer_on_message",
                     "oreslang_source_actor_send",
                     "oreslang_source_actor_stop"):
            verify(name in good.stdout, "missing guest runtime ABI: "+name)
        verify("define i32 @main(" not in good.stdout,
               "guest main must not shadow Zephyr C main")
        source.write_text("".join(actor("Worker"+str(i)) for i in range(9))+
                          "pub fnc main(): int { return 43; }\n")
        bad=compile_guest(a.compiler,source)
        verify(bad.returncode!=0 and not bad.stdout,
               "too many Cortex-M actor definitions admitted")
        source.write_text(actor("Producer")+
             "pub fnc main(): int { const a = spawn Producer(); "
             "a.send(1); stop a; return 43; }\n")
        good=compile_guest(a.compiler,source)
        verify(good.returncode==0,good.stderr)
        # Exact runtime queue capacity: reject before emitting IR.
        for count, admitted in ((16, True), (17, False)):
            source.write_text(actor("Producer")+
                "pub fnc main(): int { const a = spawn Producer(); "+
                "a.send(1); "*count+" stop a; return 43; }\n")
            outcome=compile_guest(a.compiler,source)
            if admitted:
                verify(outcome.returncode==0 and "oreslang_source_actor_send" in outcome.stdout,
                       "16 queued messages must fit admitted mailbox: "+outcome.stderr)
            else:
                verify(outcome.returncode!=0 and not outcome.stdout and
                       "mailbox capacity" in outcome.stderr.lower(),
                       "17 queued messages must be rejected at compile-time: "+outcome.stderr)
        # Separate static queues each admit sixteen messages, in total 32.
        source.write_text(actor("Producer")+actor("Consumer")+
            "pub fnc main(): int { const a = spawn Producer(); const b = spawn Consumer(); "+
            "a.send(1); "*16+"b.send(2); "*16+"stop a; stop b; return 43; }\n")
        balanced=compile_guest(a.compiler,source)
        verify(balanced.returncode==0,balanced.stderr)
        source.write_text(actor("Producer").replace(
            "return total;", "return total + unsupported(1);")+
             "pub fnc main(): int { const a = spawn Producer(); "
             "a.send(1); stop a; return 43; }\n")
        bad=compile_guest(a.compiler,source)
        verify(bad.returncode!=0 and not bad.stdout,
               "unsafe actor handler call accepted")
    print("Cortex-M source actor IR/admission PASS")


if __name__=="__main__":
    main()
