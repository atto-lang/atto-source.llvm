"""Exercise the current-main sealed actor-state source contract."""
import pathlib
import subprocess
import sys
import tempfile

compiler, clang, root = sys.argv[1:]
root = pathlib.Path(root)

def compile_source(source: str):
    with tempfile.NamedTemporaryFile("w", suffix=".ores", delete=False) as f:
        f.write(source)
        path = f.name
    try:
        return subprocess.run([compiler, path], capture_output=True, text=True, timeout=15)
    finally:
        pathlib.Path(path).unlink(missing_ok=True)

with tempfile.TemporaryDirectory() as tmp:
    ir = subprocess.run(
        [compiler, str(root / "tests/fixtures/actor-sealed-state.ores")],
        capture_output=True, text=True, timeout=15)
    assert ir.returncode == 0, ir.stderr
    assert "oreslang_actor_Counter_on_message" in ir.stdout
    ll = pathlib.Path(tmp) / "actor-state.ll"
    ll.write_text(ir.stdout)
    exe = pathlib.Path(tmp) / "actor-state"
    build = subprocess.run([
        clang, str(ll),
        str(root / "tests/native_actor_source_harness.c"),
        str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
        "-I" + str(root / "src/main/cpp"), "-o", str(exe)
    ], capture_output=True, text=True, timeout=45)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert run.returncode == 0, (run.returncode, run.stderr)

cases = [
    ("""define actor A as {
  state { x: int = 0; }
  pub fnc on_message(int n): int with state.write {
    x = n;
    return x;
  }
}
""", "actor state access requires scoped handle_state callback"),
    ("""define actor A as {
  state { x: int = 0; }
  pub fnc on_message(int n): int with state.read {
    const r = self.handle_state(|state| -> {
      state.x = n;
      return state.x;
    });
    return r;
  }
}
""", "read-only actor state loan cannot mutate state"),
    ("""define actor A as {
  state { x: int = 0; }
  pub fnc on_message(int n): int {
    const r = self.handle_state(critical |state| -> {
      state.x = n;
      return state.x;
    });
    return r;
  }
}
""", "with clause omits state.write effect"),
    ("""define actor A as {
  state { x: int = 0; }
  pub fnc on_message(int n): int with state.read {
    const r = self.handle_state(critical |state| -> {
      state.x = n;
      return state.x;
    });
    return r;
  }
}
""", "with clause omits state.write effect"),
    ("""define actor A as {
  state { x: int = 0; }
  pub fnc on_message(int n): int with state.read {
    const r = self.handle_state(|state| -> {
      return state.y;
    });
    return r;
  }
}
""", "state loan may access only declared state fields"),
]

for source, diagnostic in cases:
    result = compile_source(source)
    assert result.returncode != 0, source
    assert diagnostic in result.stderr, (diagnostic, result.stderr)
