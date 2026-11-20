// Host source-integrity check only. Native execution requires real Graal/LLVM suites.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const cases = JSON.parse(readFileSync(new URL('../contracts/v1/semantics/cases.json',import.meta.url), 'utf8')).cases;
const expected = [["budget-headroom","100 - (32 + 16)","52"],["near-max-budget","9223372036854775807 - (2 * 64)","9223372036854775679"],["staged-subtractions","17 - 8 - 3","6"],["negative-product","(-3) * (-7)","21"],["min-plus-zero","-9223372036854775808 + 0","-9223372036854775808"]];
test('five checked budget arithmetic guest mains are bound to both native semantic suites', () => {
  for (const [id,expression,value] of expected) {
    const item = cases.find(x => x.id === 'native.oresllm-budget-' + id);
    assert.ok(item, 'unregistered fixture: ' + id);
    assert.equal(item.kind, 'native_exit');
    assert.equal(item.expected_exit, 42);
    assert.equal(item.backends.graal, 'required');
    assert.equal(item.backends.llvm, 'required');
    assert.equal(item.source, 'fixtures/oresllm-budget-' + id + '.ores');
    const actual = readFileSync(new URL('../contracts/v1/semantics/' + item.source,import.meta.url), 'utf8');
    assert.equal(actual, 'pub fnc main(): int { return 42 + ((' + expression + ') - (' + value + ')); }\\n'.replace('\\n','\n'));
  }
});
test('each native budget case is uniquely registered and is not a host-only alias', () => {
  for (const [id] of expected)
    assert.equal(cases.filter(x => x.id === 'native.oresllm-budget-' + id).length, 1, id);
});
