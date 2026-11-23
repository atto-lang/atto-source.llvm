import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const clock=readFileSync(new URL('../stdlib/perf.atto',import.meta.url),'utf8');
const alloc=readFileSync(new URL('../stdlib/alloc/perf_trace_buffer.atto',import.meta.url),'utf8');
const ids=readFileSync(new URL('../stdlib/perf_ids.atto',import.meta.url),'utf8');
test('native AOT clock candidate is guarded before monotonic read',()=>{
 const begin=clock.split('pub fnc begin_ns')[1].split('pub fnc duration_ns')[0];
 const duration=clock.split('pub fnc duration_ns')[1].split('pub fnc should_sample')[0];
 assert.ok(begin.indexOf('if !enabled')<begin.indexOf('process.monotonic_ns()'));
 assert.ok(duration.indexOf('if started_ns <= 0')<duration.indexOf('process.monotonic_ns()'));
 assert.doesNotMatch(clock,/stdio|spawn|jit_compile|System\.|Java\./);
});
test('alloc tier is bounded, owner-local, no clock or I/O, disabled path does not allocate',()=>{
 assert.match(alloc,/let mut Array<Event> events/);
 assert.match(alloc,/capacity < 1 \|\| capacity > 65536/);
 assert.match(alloc,/self.events.size >= self.capacity/);
 const record=alloc.split('pub record(mut self)')[1].split('pub count()')[0];
 assert.ok(record.indexOf('if !enabled')<record.indexOf('new Event('));
 assert.doesNotMatch(alloc,/process\.|stdio\.|println|thread|spawn|interop\./);
});
test('static probe IDs preserve Java/common-source registry identity',()=>{
 const records=[...ids.matchAll(/pub fnc ([a-z0-9_]+)\(\) -> int \{ return (\d+); \}/g)];
 assert.deepEqual(records.map(m=>Number(m[2])),[23,24,25,26,27,28,29]);
 assert.equal(new Set(records.map(m=>m[1])).size,records.length);
});
