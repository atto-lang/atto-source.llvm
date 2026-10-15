#!/usr/bin/env python3
"""Real native source-compiler admission gate, not Option/Result parity proof."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
control = "pub fnc main(): int { return (5+3)*7-14; }"
cases = {
    "option_flat_map": """
        pub fnc main(): int {
          val Option<int> value = Some(3);
          return value.flat_map(|int n| -> { return Some(n + 1); }).unwrap();
        }
    """,
    "result_map_err": """
        pub fnc main(): int {
          val Result<int, string> value = Err("bad");
          return value.map_err(|string e| -> { return e; }).unwrap();
        }
    """,
    "future_flat_map": """
        async fnc seed(): int { return 4; }
        pub fnc main(): int {
          val Future<int> done = seed().flat_map(|int x| -> { return seed(); });
          return await done;
        }
    """,
    "observable_filter_map": """
        pub fnc main(): int {
          val Observable<int> s = from_values([1,2,3]);
          val Observable<int> filtered = filter_map(s, |int n| -> { return Some(n); });
          return 0;
        }
    """,
    "stream_filter_map": """
        pub fnc main(): int {
          val Stream<int> s = make_stream();
          val Stream<int> selected = s.filter_map(|int n| -> { return Some(n); });
          return 0;
        }
    """,
    "channel_not_observable": """
        pub fnc main(): int {
          val Channel<int> c = Channel.new<int>(0);
          val Observable<int> o = c;
          return 0;
        }
    """,
    "postfix_result_return": """
        fnc read_value(): Result<int, string> {
          val Result<int, string> x = Err("bad");
          val int n = x?;
          return Ok(n);
        }
        pub fnc main(): int { return 42; }
    """,
    "postfix_option_return": """
        fnc read_value(): Option<int> {
          val Option<int> x = None;
          val int n = x?;
          return Some(n);
        }
        pub fnc main(): int { return 42; }
    """,
    "postfix_try_catch_throw": """
        fnc read_value(): int throws Error {
          try {
            val Result<int, Error> x = Err(Error("bad"));
            return x?;
          } catch (err) { return 42; }
        }
        pub fnc main(): int { return read_value(); }
    """,
    "raise_bypasses_catch": """
        pub fnc main(): int {
          try { raise Error("raised"); } catch (err) { return 42; }
          return 0;
        }
    """,
    "panic_supervisor": """
        pub fnc main(): int { panic Error("fatal"); return 42; }
    """,
}
with tempfile.TemporaryDirectory(prefix="ores-elm-admission-") as td:
    path = Path(td) / "main.ores"
    path.write_text(control, encoding="utf-8")
    ok = subprocess.run([compiler, str(path)], capture_output=True, text=True, timeout=20)
    if ok.returncode != 0 or not ok.stdout.strip():
        raise AssertionError(("control compiler unavailable", ok.returncode, ok.stderr))
    for name, source in cases.items():
        path.write_text(source, encoding="utf-8")
        result = subprocess.run([compiler, str(path)], capture_output=True, text=True, timeout=20)
        if result.returncode != 1 or result.stdout or "oreslang-llvmc:" not in result.stderr:
            raise AssertionError((name, result.returncode, result.stdout[:300], result.stderr[:300]))
        print(f"{name}: unsupported source correctly rejected before IR")
