#!/usr/bin/env node
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const MANIFEST = join(ROOT, "contracts/atto-scalar-v1/cases.json");
const compiler = realpathSync(process.env.ORESLANG_LLVMC ?? join(ROOT, "build/oreslang-llvmc"));
const clang = process.env.CLANG ?? "clang";
const manifest = JSON.parse(readFileSync(MANIFEST, "utf8"));
const work = mkdtempSync(join(tmpdir(), "atto-scalar-v1-"));
const sha256 = bytes => createHash("sha256").update(bytes).digest("hex");
const gitBlobSha1 = bytes => createHash("sha1")
  .update(Buffer.from(`blob ${bytes.length}\0`)).update(bytes).digest("hex");
const bounded = value => String(value ?? "").slice(0, 3500).trim();

function run(program, args, options = {}) {
  const result = spawnSync(program, args, {
    cwd: ROOT,
    encoding: "utf8",
    timeout: options.timeout ?? 40_000,
    maxBuffer: 16 * 1024 * 1024,
    ...options
  });
  if (result.error) throw new Error(`${program} failed to start: ${result.error.message}`);
  return result;
}
function requireTrue(value, message) {
  if (!value) throw new Error(message);
}
function compileHarness(id, ir) {
  const wrapper = /^define\s+i32\s+@main\(\)([^\n{]*)\{/gm;
  const matches = [...ir.matchAll(wrapper)];
  requireTrue(matches.length === 1, `${id}: expected exactly one native i32 main wrapper`);
  requireTrue(/^define\s+i64\s+@__oreslang_guest_main\(\)/m.test(ir),
    `${id}: missing exported i64 guest main`);
  const patched = ir.replace(wrapper, "define i32 @__atto_host_main_unused()$1{");
  const irPath = join(work, id + ".observer.ll");
  const objPath = join(work, id + ".o");
  const binPath = join(work, id + ".bin");
  writeFileSync(irPath, patched);
  const object = run(clang, ["-O2", "-c", "-x", "ir", irPath, "-o", objPath]);
  requireTrue(object.status === 0 && !object.signal,
    `${id}: clang IR compile failed: ${bounded(object.stderr)}`);
  const link = run(clang, ["-O2", join(work, "observer.c"), objPath, "-o", binPath]);
  requireTrue(link.status === 0 && !link.signal,
    `${id}: observer link failed: ${bounded(link.stderr)}`);
  return { binPath, irSha256: sha256(Buffer.from(ir, "utf8")),
    artifactSha256: sha256(readFileSync(binPath)) };
}
function compileSource(c) {
  const source = resolve(ROOT, c.path);
  requireTrue(source.startsWith(ROOT + "/"), `${c.id}: source escaped repository root`);
  const bytes = readFileSync(source);
  requireTrue(gitBlobSha1(bytes) === c.git_blob_sha1,
    `${c.id}: canonical Git blob identity mismatch`);
  const result = run(compiler, ["--target=host", source]);
  if (result.signal) throw new Error(`${c.id}: compiler crashed with signal ${result.signal}`);
  return { source, sourceSha256: sha256(bytes), result };
}

writeFileSync(join(work, "observer.c"), `#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
extern int64_t __oreslang_guest_main(void);
int main(void) {
  const int64_t value = __oreslang_guest_main();
  printf("%" PRId64 "\\n", value);
  return 0;
}
`);

requireTrue(manifest.schema === "attolang.scalar-core.v1", "wrong scalar manifest schema");
requireTrue(Array.isArray(manifest.compiler_routes) && manifest.compiler_routes.includes("llvm"),
  "manifest does not admit LLVM route");
requireTrue(manifest.cases.length === 19, "scalar-core v1 requires exactly 19 cases");

const cases = [];
let positive = 0, compileNegative = 0, runtimeNegative = 0;
try {
  for (const c of manifest.cases) {
    const { sourceSha256, result: compile } = compileSource(c);
    const diagnostic = bounded(compile.stderr) || c.diagnostic_family || "compiler rejection";
    if (c.kind === "positive") {
      requireTrue(compile.status === 0, `${c.id}: positive compilation failed: ${diagnostic}`);
      requireTrue(compile.stdout.length > 0, `${c.id}: compiler emitted empty LLVM IR`);
      const built = compileHarness(c.id, compile.stdout);
      const guest = run(built.binPath, [], { timeout: 20_000 });
      requireTrue(!guest.signal && guest.status === 0,
        `${c.id}: positive guest failed: signal=${guest.signal} status=${guest.status}`);
      const observed = guest.stdout.trim();
      requireTrue(/^-?(0|[1-9][0-9]*)$/.test(observed),
        `${c.id}: observer did not return canonical signed i64 text`);
      requireTrue(observed === c.expected_i64,
        `${c.id}: expected i64 ${c.expected_i64}, observed ${observed}`);
      cases.push({
        id: c.id, source_sha256: sourceSha256, kind: "positive",
        compile_exit: 0, artifact_sha256: built.artifactSha256,
        compiler_ir_sha256: built.irSha256, executed: true,
        result_i64: observed, expected_i64: c.expected_i64,
        result_channel: "host-call"
      });
      positive++;
      continue;
    }

    requireTrue(c.kind === "negative", `${c.id}: unknown case kind`);
    if (compile.status !== 0) {
      requireTrue(compile.status !== null, `${c.id}: nonnumeric compiler failure`);
      requireTrue(compile.stdout.length === 0,
        `${c.id}: compiler published LLVM IR before rejecting negative source`);
      cases.push({
        id: c.id, source_sha256: sourceSha256, kind: "negative",
        compile_exit: compile.status, executed: false, failure_stage: "compile",
        diagnostic
      });
      compileNegative++;
      continue;
    }

    requireTrue(c.diagnostic_family === "checked-overflow",
      `${c.id}: ${c.diagnostic_family} must reject at compile time, not execute`);
    requireTrue(compile.stdout.length > 0, `${c.id}: successful compile emitted empty IR`);
    const built = compileHarness(c.id, compile.stdout);
    const guest = run(built.binPath, [], { timeout: 20_000 });
    requireTrue(Boolean(guest.signal) || guest.status !== 0,
      `${c.id}: checked-overflow negative returned normally: ${guest.stdout.trim()}`);
    const runtimeOutcome = guest.signal ? "trap" : "error";
    cases.push({
      id: c.id, source_sha256: sourceSha256, kind: "negative",
      compile_exit: 0, artifact_sha256: built.artifactSha256,
      compiler_ir_sha256: built.irSha256, executed: true,
      failure_stage: "runtime", runtime_outcome: runtimeOutcome,
      diagnostic: `${c.diagnostic_family}: ${guest.signal ?? "status " + guest.status}`
    });
    runtimeNegative++;
  }

  const head = run("git", ["rev-parse", "HEAD"]).stdout.trim();
  const tree = run("git", ["rev-parse", "HEAD^{tree}"]).stdout.trim();
  const compilerBytes = readFileSync(compiler);
  const receipt = {
    schema: "attolang.parity-receipt/2",
    backend: "llvm",
    head_sha: head,
    tree_sha: tree,
    compiler: basename(compiler),
    compiler_sha256: sha256(compilerBytes),
    run_id: Number(process.env.GITHUB_RUN_ID ?? 1),
    run_attempt: Number(process.env.GITHUB_RUN_ATTEMPT ?? 1),
    executed_steps: Number(process.env.ATTO_EXECUTED_STEPS ?? 1),
    job_conclusion: "success",
    cases
  };
  requireTrue(Number.isSafeInteger(receipt.run_id) && receipt.run_id > 0, "invalid run id");
  requireTrue(Number.isSafeInteger(receipt.run_attempt) && receipt.run_attempt > 0, "invalid run attempt");
  requireTrue(Number.isSafeInteger(receipt.executed_steps) && receipt.executed_steps > 0,
    "invalid executed-step observation");
  const receiptPath = process.env.ATTO_RECEIPT_PATH ?? join(work, "llvm-parity-receipt-v2.json");
  writeFileSync(receiptPath, JSON.stringify(receipt, null, 2) + "\n");
  console.log(`ATTO_SCALAR_LLVM_EXECUTION_PASS positive=${positive} compile_negative=${compileNegative} runtime_negative=${runtimeNegative}`);
  console.log(`ATTO_SCALAR_LLVM_RECEIPT_V2=${receiptPath}`);
  console.log(JSON.stringify(receipt));
} finally {
  if (!process.env.ATTO_KEEP_WORK) rmSync(work, { recursive: true, force: true });
}
