// SOURCE-ONLY subset execution: NOT a native Atto compiler or guest proof.
// This deliberately recognizes just the five straight-line arena policy fncs.
// Reject unexpected syntax rather than pretending unsupported lowering works.
import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../stdlib/alloc/arena_plan.atto", import.meta.url), "utf8");
const manifest = JSON.parse(readFileSync(new URL("../contracts/atto-arena-policy-v1.json", import.meta.url), "utf8"));
const gitBlob = createHash("sha1")
  .update(`blob ${Buffer.byteLength(source, "utf8")}\0`)
  .update(source, "utf8")
  .digest("hex");

test("exact pinned arena source and honest evidence boundary", () => {
  assert.equal(gitBlob, manifest.source_git_blob_sha);
  assert.equal(manifest.status, "SOURCE_ONLY_NOT_EXECUTED_BY_NATIVE_ATTO_COMPILER");
  assert.equal(manifest.cases.length, 38);
});

// This test does not admit imports, side effects, loops, function calls or
// allocation. The real compiler must independently execute positive/negative
// .atto guest programs before advertising any available library API.
const stripped = source.replace(/\/\/[^\n]*/g, "");
const exportPattern = /^pub fnc ([a-z_][a-z0-9_]*)\(([^)]*)\): (int|bool) \{([\s\S]*?)^\}/gm;
const exports = [...stripped.matchAll(exportPattern)];
const expectedNames = [
  "arena_reservation_end",
  "arena_rewind_target",
  "arena_tail_release_valid",
  "arena_can_close",
  "arena_next_generation"
];
const definitions = exports.map(match => {
  const names = match[2].split(",").map(p => {
    const found = /^(int) ([a-z_][a-z0-9_]*)$/.exec(p.trim());
    assert.ok(found, `unsupported parameter: ${p}`);
    return found[2];
  });
  const body = match[4].replace(
    /if ([^\n;]+) then return ([^\n;]+); fi/g,
    "if ($1) return $2;"
  );
  // Restrict this subset to arithmetic/relational conditions and returns.
  assert.doesNotMatch(body, /\b(?:fi|then|new|import|rt|await|throw|while|for|fnc)\b/);
  const allowed = body
    .replace(/\b(?:if|return|true|false)\b/g, "")
    .replace(/[a-z_][a-z0-9_]*/g, "")
    .replace(/[0-9]+/g, "")
    .replace(/[+\-<>=!&;()\s]/g, "");
  assert.equal(allowed, "", "unrecognized source syntax");
  const bigintBody = body.replace(/(?<![a-zA-Z0-9_])(-?\d+)(?![a-zA-Z0-9_])/g, "$1n");
  return `function ${match[1]}(${names.join(",")}) { ${bigintBody} }`;
});

test("only the five declared arithmetic policies are present", () => {
  assert.deepEqual(exports.map(m => m[1]), expectedNames);
  const untouched = stripped.replace(exportPattern, "").trim();
  assert.equal(untouched, "", "unparsed source outside functions");
});

const compiled = runInNewContext(
  `${definitions.join("\n")}\n({${expectedNames.join(",")}})`,
  Object.create(null),
  { timeout: 1000 }
);
for (const item of manifest.cases) {
  test(`source subset: ${item.id} ${item.fn}`, () => {
    assert.ok(Object.hasOwn(compiled, item.fn), "unknown tested function");
    const args = item.args.map(x => BigInt(x));
    const got = compiled[item.fn](...args);
    const want = typeof item.expected === "boolean"
      ? item.expected : BigInt(item.expected);
    assert.equal(got, want);
  });
}
