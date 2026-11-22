// Source contract audit only: DOES NOT compile or execute Atto guest code.
// Run: node --test tests/std-metrics-json-zorde-source-contract.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const read = p => readFileSync(path.join(repo, p), "utf8");
const modules = {
  metrics: read("stdlib/metrics.atto"),
  json: read("stdlib/json.atto"),
  zorde: read("stdlib/zorde.atto"),
};

function stripComments(s) {
  return s.split("\n").map(line => line.replace(/\/\/.*$/, "")).join("\n");
}

test("three independent native Atto hosted modules, no host interop", () => {
  for (const [name, content] of Object.entries(modules)) {
    assert.match(content, new RegExp("define module " + name + " as"));
    const source = stripComments(content);
    assert.doesNotMatch(source, /\bjava:|\bSystem\.|\bHostInterop\.|\bffi\.|\bNativeIo\./);
    assert.doesNotMatch(source, /\bawait\s|\bstdio\.|\bnetwork\.|\bprocess\./);
    assert.ok(content.length > 100);
  }
});

test("JSON parser has explicit numeric, Unicode, duplicate-key and quota guards", () => {
  const json = modules.json;
  for (const invariant of [
    "keys_equal", "hex4", "55296", "56319", "56320", "57343",
    "1048576", "262144", "keys.size >= 128",
    "capture", "read_string(false)", "read_string(true)", "key_comparisons >= 65536",
    "IntegerRange", "floor = -9223372036854775807",
    "if negative; do floor = floor - 1; fi",
    "decode_i32", "parse(String text)", "stringify(Document document)",
  ]) assert.ok(json.includes(invariant), "JSON safeguard missing: " + invariant);
});

test("Zorde varuint and frame admission reject ambiguity before exposure", () => {
  const z = modules.zorde;
  for (const invariant of [
    "NonCanonicalVaruint", "VaruintOverflow", "TruncatedVaruint",
    "write_length_delimited", "read_length_delimited",
    "self.position = start", "InvalidWireByte", "WrongSchema",
    "UnsupportedSchemaVersion", "length != r.remaining()",
    "encode_bool_frame", "decode_bool_frame",
    "encode_i31_frame", "decode_i31_frame",
  ]) assert.ok(z.includes(invariant), "Zorde safeguard missing: " + invariant);
});

test("Metrics counters and histograms explicitly reject unsafe updates", () => {
  const m = modules.metrics;
  for (const invariant of [
    "ceiling - self.current", "amount < 0",
    "self.samples == 2147483647", "2147483647 - self.total",
    "self.rejected < 2147483647", "bounds.size > 64",
    "value <= bounds.get(i - 1)", "bucket_count",
  ]) assert.ok(m.includes(invariant), "metrics safeguard missing: " + invariant);
});

test("Java compiler-owned registry and test-vector counts, when present", () => {
  const registryPath = "src/main/java/dev/oreslang/stdlib/CoreLibrarySources.java";
  if (!existsSync(path.join(repo, registryPath))) return;
  const registry = read(registryPath);
  for (const name of Object.keys(modules)) {
    assert.ok(registry.includes('"' + name + '.atto"'));
  }
  const suite = read("src/test/java/dev/oreslang/runtime/NativeAttoMetricsJsonZordeTest.java");
  const cases = { metrics: 18, json: 36, zorde: 29 };
  for (const [name, count] of Object.entries(cases)) {
    const fixture = read("src/test/resources/stdlib-parity/metrics-json-zorde/" + name + ".atto");
    const actual = (fixture.match(/stdio\.println\(/g) || []).length;
    assert.equal(actual, count, name + " guest fixture output count");
    const pattern = new RegExp('assertGuest\\("' + name + '",([\\s\\S]*?)\\);');
    const match = suite.match(pattern);
    assert.ok(match, "missing Java assertion for " + name);
    assert.equal((match[1].match(/"[^"]*"/g) || []).length, count,
      name + " golden expected-output count");
  }
});

test("deterministic Zorde v1 little wire reference vectors (source-free oracle)", () => {
  const varuint = n => {
    assert.ok(Number.isSafeInteger(n) && n >= 0 && n <= 2147483647);
    const out = [];
    while (n >= 128) { out.push((n % 128) + 128); n = Math.floor(n / 128); }
    out.push(n);
    return out;
  };
  const frame = (id, version, payload) =>
    [90, 79, 82, 68, 1, ...varuint(id), ...varuint(version), ...varuint(payload.length), ...payload];
  assert.deepEqual(frame(7, 1, [0, 255]), [90,79,82,68,1,7,1,2,0,255]);
  assert.deepEqual(frame(42, 2, [1]), [90,79,82,68,1,42,2,1,1]);
  assert.deepEqual(varuint(2147483647), [255,255,255,255,7]);
  assert.deepEqual(varuint(2147483646), [254,255,255,255,7]);
});
