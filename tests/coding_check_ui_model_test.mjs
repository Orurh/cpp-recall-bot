// Pure report-model checks: synthetic reports are not evidence of C++ execution.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { webcrypto } from 'node:crypto';

if (!globalThis.crypto) Object.defineProperty(globalThis, 'crypto', { value: webcrypto });
await import('../docs/miniapp/coding-check.js');
const check = globalThis.CppRecallCodingCheck;
const manifest = JSON.parse(await readFile(new URL('../docs/miniapp/coding-check-cases.json', import.meta.url), 'utf8'));
const clone = value => structuredClone(value);
const source = '// <img src=x onerror="alert(1)">\nstd::vector<int> TakePrefix(std::vector<int>& p, std::size_t n) { return {}; }';
const marks = { help_opened: false, solution_opened: false };
const submission = await check.createSubmission(source, marks);
assert.equal(submission.source, source);
assert.match(submission.source_sha256, /^[a-f0-9]{64}$/);
assert.notEqual(submission.attempt_id, (await check.createSubmission(source, marks)).attempt_id);
assert.equal(submission.source_sha256, (await check.createSubmission(source, marks)).source_sha256);
assert.notEqual(submission.source_sha256, (await check.createSubmission(source + ' ', marks)).source_sha256);
assert.deepEqual(submission.assistance, marks);
marks.help_opened = true;
assert.equal(submission.assistance.help_opened, false, 'submission owns an immutable-in-practice assistance snapshot');
await assert.rejects(check.createSubmission('я'.repeat(8193), marks), /16384/);
await assert.rejects(check.createSubmission(source, { help_opened: 1, solution_opened: false }), /помощи/);

const pass = {
  schema: 'cpp-recall-local-check/v1', exercise_id: submission.exercise_id,
  exercise_revision: submission.exercise_revision, suite_id: submission.suite_id,
  source_sha256: submission.source_sha256, attempt_id: submission.attempt_id,
  assistance: clone(submission.assistance), trust: 'local_unverified', status: 'tests_passed',
  compile: { status: 'passed', diagnostics: '' },
  compiler: { id: 'synthetic-compiler', version: 'test fixture only', standard: 'c++23', flags: ['-std=c++23'] },
  tests: manifest.cases.map(test => ({ id: test.id, status: 'passed', input_pending: clone(test.pending), actual: clone(test.expected) }))
};
const result = await check.validateReport(JSON.stringify(pass), submission, manifest);
assert.equal(result.status, 'tests_passed'); assert.equal(result.total, 17);
assert.equal(result.trust, 'local_unverified');
assert.equal(result.tests[3].test.id, 'partial_prefix');

let assertions = 0;
async function rejectChange(change, expression) {
  const report = clone(pass); change(report);
  await assert.rejects(check.validateReport(report, submission, manifest), expression);
  ++assertions;
}
await rejectChange(report => { report.tests.pop(); }, /полным/);
await rejectChange(report => { report.tests[1].id = report.tests[0].id; }, /повторяющийся/);
await rejectChange(report => { report.tests[0].id = 'invented'; }, /Неизвестный/);
await rejectChange(report => { report.tests[3].actual.pending = [4, 7, 4, 9]; }, /противоречит/);
await rejectChange(report => { report.tests[3].actual.result = [4]; report.tests[3].status = 'failed'; }, /полным/);
await rejectChange(report => { report.tests[0].actual.result = [2147483648]; }, /значения/);
await rejectChange(report => { report.tests[0].actual.pending = [0.5]; }, /значения/);
await rejectChange(report => { report.tests[0].actual.result = Array(4097).fill(1); }, /значения/);
await rejectChange(report => { report.tests[0].actual.result = ['1']; }, /значения/);
await rejectChange(report => { report.tests[0].actual.result = new Array(2); }, /значения/);
await rejectChange(report => { delete report.tests[0].input_pending; }, /вход/);
await rejectChange(report => { report.tests[0].input_pending = [2147483648]; }, /вход/);
await rejectChange(report => { report.tests[3].input_pending = []; }, /Вход теста/);
await rejectChange(report => { [report.tests[0], report.tests[1]] = [report.tests[1], report.tests[0]]; }, /порядку/);
await rejectChange(report => { report.tests.splice(2, 1); }, /порядку/);
await rejectChange(report => { report.tests[14].input_pending = [9]; }, /Вход теста/);
await rejectChange(report => { report.compile.status = 'failed'; }, /полным/);
await rejectChange(report => { report.compiler.standard = 'c++17'; }, /C\+\+23/);
await rejectChange(report => { report.compiler.flags = 'g++'; }, /C\+\+23/);
await rejectChange(report => { report.trust = 'verified'; }, /Формат/);
await rejectChange(report => { report.schema = 'wrong'; }, /Формат/);
await rejectChange(report => { report.exercise_revision = 2; }, /редакции/);
await rejectChange(report => { report.suite_id = 'another-suite'; }, /набору/);
await rejectChange(report => { report.source_sha256 = '0'.repeat(64); }, /коду/);
await rejectChange(report => { report.attempt_id = crypto.randomUUID(); }, /пакету/);
await rejectChange(report => { report.assistance.solution_opened = true; }, /помощи/);
await rejectChange(report => { report.status = 'certified'; }, /Неизвестный/);
await rejectChange(report => { report.status = 'tests_failed'; }, /нет несовпадения/);
await rejectChange(report => { report.status = 'compile_error'; report.compile.status = 'failed'; }, /компиляции/);
await rejectChange(report => { report.compile.diagnostics = 'я'.repeat(65536); }, /большой/);

const failed = clone(pass); failed.status = 'tests_failed';
failed.tests[3].actual.pending = [4, 7, 4, 9]; failed.tests[3].status = 'failed';
assert.equal((await check.validateReport(failed, submission, manifest)).tests[3].status, 'failed');
const incompleteFailure = clone(failed); incompleteFailure.tests.pop();
await assert.rejects(check.validateReport(incompleteFailure, submission, manifest), /полным набором/);
const compileError = clone(pass); compileError.status = 'compile_error'; compileError.tests = [];
compileError.compile = { status: 'failed', diagnostics: '<script>untrusted compiler diagnostic</script>' };
assert.equal((await check.validateReport(compileError, submission, manifest)).diagnostics, compileError.compile.diagnostics);
const unavailable = clone(pass); unavailable.status = 'sandbox_unavailable'; unavailable.tests = [];
unavailable.compile.status = 'not_run';
assert.equal((await check.validateReport(unavailable, submission, manifest)).status, 'sandbox_unavailable');
const partialRuntime = clone(pass); partialRuntime.status = 'runtime_error'; partialRuntime.tests = partialRuntime.tests.slice(0, 2);
assert.equal((await check.validateReport(partialRuntime, submission, manifest)).tests.length, 2);
for (const status of ['timeout', 'resource_limit', 'output_limit']) {
  const interruptedCompile = clone(pass); interruptedCompile.status = status; interruptedCompile.tests = [];
  interruptedCompile.compile.status = 'not_run';
  assert.equal((await check.validateReport(interruptedCompile, submission, manifest)).status, status);
  interruptedCompile.compile.status = 'failed';
  assert.equal((await check.validateReport(interruptedCompile, submission, manifest)).status, status);
}
const sequenceFailure = clone(failed);
sequenceFailure.tests[13].actual.pending = [9];
sequenceFailure.tests[13].status = 'failed';
sequenceFailure.tests[14].input_pending = [9];
sequenceFailure.tests[14].actual = { result: [9], pending: [] };
sequenceFailure.tests[14].status = 'failed';
sequenceFailure.tests[15].input_pending = [];
assert.deepEqual((await check.validateReport(sequenceFailure, submission, manifest)).tests[14].input_pending, [9]);
const modifiedSubmission = clone(submission); modifiedSubmission.source += ' ';
await assert.rejects(check.validateReport(pass, modifiedSubmission, manifest), /пакет изменён/);
const modifiedManifest = clone(manifest); modifiedManifest.suite_id = 'different';
await assert.rejects(check.validateReport(pass, submission, modifiedManifest), /набору/);
assert.throws(() => check.parseReport('{'), /JSON/);
assert.throws(() => check.parseReport('я'.repeat(check.MAX_REPORT_BYTES / 2 + 1)), /128/);
console.log(`PASS local coding report model: ${assertions} rejected contradictions plus valid pass/fail/compile/partial/error fixtures; no C++ execution claimed.`);
