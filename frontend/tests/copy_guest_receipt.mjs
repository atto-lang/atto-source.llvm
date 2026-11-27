// Host-only receipt validation; never substitutes for executing the compiler.
export function validRejection(input, expected, run, artifact) {
  if (!run || run.error || run.signal !== null || run.status !== 1 || artifact !== false) return false;
  if (typeof input !== 'string' || !input || typeof expected !== 'string' ||
      typeof run.stderr !== 'string' || !/^E_[A-Z0-9_]+$/.test(expected)) return false;
  // Match one diagnostic with at most one conventional line terminator.
  const line = run.stderr.replace(/\r?\n$/, '');
  const prefix = input + ':';
  if (/[\r\n]/.test(line) || !line.startsWith(prefix)) return false;
  const d = /^([1-9][0-9]*):([1-9][0-9]*): (E_[A-Z0-9_]+): \S[^\r\n]*$/.exec(line.slice(prefix.length));
  return d !== null && Number.isSafeInteger(Number(d[1])) &&
    Number.isSafeInteger(Number(d[2])) && d[3] === expected;
}
