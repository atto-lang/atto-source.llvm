import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

// Independent HOST_ONLY source-oracle check. It does not compile or execute .atto.
const manifest = JSON.parse(readFileSync(new URL('./contracts/atto_ownership_source_cfg_50.json', import.meta.url), 'utf8'));
const categories = ['restored-direct', 'restored-both-branches', 'restored-sibling', 'unrestored-one-branch', 'unrestored-read'];
assert.equal(manifest.schema_version, 1);
assert.equal(manifest.classification, 'CANDIDATE_REAL_SOURCE_CFG_COMPILER_INPUTS_NOT_EXECUTED');
assert.equal(manifest.cases.length, 50);
const names = new Set();
const sources = new Set();
const counts = new Map(categories.map(name => [name, 0]));
for (const entry of manifest.cases) {
  test(entry.id, () => {
    assert.match(entry.id, /^(restored-direct|restored-both-branches|restored-sibling|unrestored-one-branch|unrestored-read)-\d{2}$/);
    assert.ok(!names.has(entry.id), 'duplicate case identity');
    names.add(entry.id);
    const hash = createHash('sha256').update(entry.source, 'utf8').digest('hex');
    assert.ok(!sources.has(hash), 'duplicate source input');
    sources.add(hash);
    assert.ok(counts.has(entry.category), 'unknown category');
    counts.set(entry.category, counts.get(entry.category) + 1);
    assert.ok(entry.id.startsWith(entry.category + '-'), 'category must agree with ID');
    const index = Number(entry.id.slice(-2)) - 1;
    assert.ok(index >= 0 && index < 10, 'invalid case ordinal');
    const base = 20 + index;
    const code = entry.source;
    assert.ok(code.startsWith(`define class Restore${index} as\n  pub let int left = ${base};\n  pub let int right = 7;\nend\n`), 'class layout or initial state altered');
    assert.ok(code.includes(`let mut Restore${index} p = new Restore${index}();`), 'constructor provenance altered');
    assert.ok(code.includes('const saved: int = rt take p.left;'), 'no real source move');
    assert.equal(code.match(/\brt take p\.left\b/g)?.length, 1, 'unexpected number of moves');
    const branch = entry.category === 'restored-both-branches' || entry.category === 'unrestored-one-branch';
    if (branch) {
      const expected = `if (${base} eq ${base + Number(index >= 5)})`;
      assert.ok(code.includes(expected), 'missing expected true/false predicate');
      assert.equal(code.match(/\bif\s*\(/g)?.length, 1, 'duplicate branch');
    } else {
      assert.ok(!/\bif\s*\(/.test(code), 'hidden branch in non-branch category');
    }
    switch (entry.category) {
      case 'restored-direct':
        assert.ok(code.includes('p.left = saved;\n  return p.left + 1;'));
        assert.equal(entry.expected_guest_exit, base + 1);
        assert.ok(!Object.hasOwn(entry, 'expected_diagnostic'));
        break;
      case 'restored-both-branches':
        assert.ok(code.includes('{ p.left = saved; } else { p.left = saved + 1; } fi\n  return p.left;'));
        assert.equal(entry.expected_guest_exit, base + Number(index >= 5));
        assert.ok(!Object.hasOwn(entry, 'expected_diagnostic'));
        break;
      case 'restored-sibling':
        assert.ok(code.includes('p.left = saved;\n  return p.left + p.right;'));
        assert.equal(entry.expected_guest_exit, base + 7);
        assert.ok(!Object.hasOwn(entry, 'expected_diagnostic'));
        break;
      case 'unrestored-one-branch':
        assert.ok(code.includes('{ p.left = saved; } else { } fi\n  return p.left;'));
        assert.equal(entry.expected_diagnostic, 'conditional CFG predecessor ownership mismatch');
        assert.ok(!Object.hasOwn(entry, 'expected_guest_exit'));
        break;
      case 'unrestored-read':
        assert.ok(code.includes('return p.left + saved;'));
        assert.equal(entry.expected_diagnostic, 'ownership violation: use-after-move class field');
        assert.ok(!Object.hasOwn(entry, 'expected_guest_exit'));
        break;
    }
  });
}
for (const category of categories) assert.equal(manifest.cases.filter(x => x.category === category).length, 10, category);
// SOURCE_ONLY by construction. Never emit the compiled guest pass marker.
