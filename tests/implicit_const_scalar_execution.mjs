#!/usr/bin/env node
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve, join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const compiler = process.env.ORESLANG_LLVMC ?? join(root, "build/oreslang-llvmc");
const clang = process.env.CLANG ?? "clang";
const cases = [
  ["literal-equivalence", "positive", "a8e49769215a78946009f2cd2ac685b7c34c7804", "42"],
  ["const-reference", "positive", "a1ebef245f6357ca712b30d9e02d379da3e65abc", "42"],
  ["const-arithmetic", "positive", "44daf131a7793f3b341c41a7e934cf8afe51f9db", "42"],
  ["bool-local", "positive", "52c7909c0d2ba760c000fef2cc4434aefbd91f97", "42"],
  ["runtime-val-let", "positive", "fb47b57cb715ff9162de6a757ae515c1a9c19a30", "42"],
  ["implicit-nonconst", "negative", "c3e5221ad48bfc8710249be41422acea41fccc8c", "E_NON_CONST_INITIALIZER"],
  ["explicit-nonconst", "negative", "5a38a3999e3e7e2784c289ab764c68bbeefcf1af", "E_NON_CONST_INITIALIZER"],
  ["const-mut-scalar", "negative", "2ff0af5fdcd84d1db293a6d6c1ebd82076e2e582", "E_CONST_MUT_UNSUPPORTED"],
  ["const-overflow", "negative", "496fe64c927b7ecee891c9e59d3583b0f2d58bed", "E_CONST_OVERFLOW"],
  ["implicit-rebind", "negative", "ebc0c31b84869774bd398c9c469bfda58334acc0", "E_IMMUTABLE_REBIND"],
  ["explicit-rebind", "negative", "87dd9e7483254a0ecb8865d72e5e6e9ee337d4e7", "E_IMMUTABLE_REBIND"]
];
const scratch = mkdtempSync(join(tmpdir(), "atto-implicit-const-"));
function run(program, args) {
  const result = spawnSync(program, args, {
    cwd: root, encoding: "utf8", timeout: 60_000, maxBuffer: 8 * 1024 * 1024
  });
  if (result.error || result.signal) throw Error(
    program + ": " + (result.error?.message ?? result.signal));
  return result;
}
function insist(yes, message) { if (!yes) throw Error(message); }
function blob(bytes) {
  return createHash("sha1").update("blob " + bytes.length + "\0").update(bytes).digest("hex");
}
try {
  const observer = join(scratch, "observer.c");
  writeFileSync(observer, [
    "#include <inttypes.h>",
    "#include <stdio.h>",
    "extern int64_t __oreslang_guest_main(void);",
    "int main(void) {",
    '  printf("%" PRId64 "\\n", __oreslang_guest_main());',
    "  return 0;",
    "}"
  ].join("\n") + "\n");
  for (const [name, kind, expectedBlob, expected] of cases) {
    const source = join(root, "contracts", "implicit-const-scalar-v1", name + ".atto");
    const bytes = readFileSync(source);
    insist(blob(bytes) === expectedBlob, name + ": unreviewed source blob");
    const compiled = run(compiler, ["--target=host", source]);
    if (kind === "negative") {
      insist(compiled.status !== 0, name + ": invalid source compiled");
      insist(compiled.stdout.length === 0, name + ": rejected source published LLVM IR");
      insist(compiled.stderr.includes(expected),
        name + ": diagnostic mismatch: " + compiled.stderr.slice(0, 1200));
      process.stdout.write("NEGATIVE " + name + " " + expected + " NO_IR\n");
      continue;
    }
    insist(compiled.status === 0, name + ": compiler failed: " + compiled.stderr.slice(0, 1600));
    insist(compiled.stdout.includes("define i64 @__oreslang_guest_main()"),
      name + ": missing compiled guest entry");
    const wrapper = /^define\s+i32\s+@main\(\)([^\n{]*)\{/gm;
    const found = [...compiled.stdout.matchAll(wrapper)];
    insist(found.length === 1, name + ": expected exactly one host wrapper");
    const ir = compiled.stdout.replace(wrapper, "define i32 @__atto_host_main_unused()$1{");
    const ll = join(scratch, name + ".ll");
    writeFileSync(ll, ir);
    for (const opt of ["0", "2"]) {
      const bin = join(scratch, name + "-O" + opt);
      const object = join(scratch, name + "-O" + opt + ".o");
      const built = run(clang, ["-O" + opt, "-c", "-x", "ir", ll, "-o", object]);
      insist(built.status === 0, name + ": IR compile failed: " + built.stderr);
      const linked = run(clang, ["-O" + opt, observer, object, "-o", bin]);
      insist(linked.status === 0, name + ": link failed: " + linked.stderr);
      const guest = run(bin, []);
      insist(guest.status === 0 && guest.stdout.trim() === expected,
        name + ": expected " + expected + ", got " + guest.status + ": " + guest.stdout);
      process.stdout.write("GUEST " + name + " O" + opt + " VALUE=" + guest.stdout.trim() + "\n");
    }
  }
  process.stdout.write("ATTO_IMPLICIT_CONST_LLVM_SCALAR_GUEST_SUITE_PASSED\n");
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
