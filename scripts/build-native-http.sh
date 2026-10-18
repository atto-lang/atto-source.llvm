#!/usr/bin/env bash
# Build compiled Oreslang actor + native HTTP adapter; no Java/JIT at runtime.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
output="${1:-$root/build/native-http}"
mkdir -p "$output"
output="$(cd "$output" && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
cxx="${CXX:-clang++}"
flags=(-O2 -g)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined); fi
"$compiler" "$root/examples/native-http/worker.ores" > "$output/worker.ll"
"$clang" "${flags[@]}" -x ir -c "$output/worker.ll" -o "$output/worker.o"
for name in NativeActorScheduler ActorOwnedI64Heap; do
  "$clang" "${flags[@]}" -std=c11 -pthread -I "$root/src/main/cpp" -c "$root/src/main/cpp/oreslang/runtime/$name.c" -o "$output/$name.o"
done
"$cxx" "${flags[@]}" -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread -I "$root/src/main/cpp" \
  "$root/examples/native-http/server.cpp" "$output/worker.o" "$output/NativeActorScheduler.o" "$output/ActorOwnedI64Heap.o" -o "$output/native-http-server"
python3 - "$root" "$output" "$compiler" "$clang" "$cxx" <<'PYTHON'
import hashlib, json, pathlib, subprocess, sys
root, output = map(pathlib.Path, sys.argv[1:3])
def git(*args): return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
files = sorted((root/'src/main/cpp/oreslang/runtime').glob('*'))
files += sorted((root/'examples/native-http').glob('*.ores'))
files += [root/'examples/native-http/server.cpp']
sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
receipt = {'source_commit': git('rev-parse', 'HEAD'), 'source_tree': git('rev-parse', 'HEAD^{tree}'),
           'dirty': bool(git('status', '--porcelain')), 'sources_sha256': sources,
           'binary_sha256': hashlib.sha256((output/'native-http-server').read_bytes()).hexdigest(),
           'compiler_sha256': hashlib.sha256(pathlib.Path(sys.argv[3]).read_bytes()).hexdigest(),
           'clang': subprocess.check_output([sys.argv[4], '--version'], text=True).splitlines()[0],
           'cxx': subprocess.check_output([sys.argv[5], '--version'], text=True).splitlines()[0]}
(output/'build-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
PYTHON
printf '%s\n' "$output/native-http-server"
