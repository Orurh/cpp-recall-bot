// Real local HTTP/process checks. Requires a disposable TEST_DATABASE_URL; never reads .env.
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { createHmac, randomInt, randomUUID } from 'node:crypto';
import { readdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { createServer, connect } from 'node:net';
import { checkSharedChallengeHttp } from './runtime_shared_challenge_checks.mjs';

if (!process.env.TEST_DATABASE_URL) {
  console.log('runtime_http_smoke skipped: TEST_DATABASE_URL is not set');
  process.exit(77);
}
const database = new URL(process.env.TEST_DATABASE_URL);
const sqlEnv = { ...process.env, PGHOST: database.hostname, PGPORT: database.port || '5432',
  PGUSER: decodeURIComponent(database.username), PGPASSWORD: decodeURIComponent(database.password),
  PGDATABASE: decodeURIComponent(database.pathname.slice(1)) };
const sql = (statement) => {
  try { return execFileSync('psql', ['-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', statement], { env: sqlEnv, stdio: 'pipe' }).toString().trim(); }
  catch { throw new Error('HTTP test SQL fixture failed'); }
};
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(check, description, timeout = 25000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    try { if (await check()) return; } catch { /* startup or intentionally saturated */ }
    await pause(100);
  }
  throw new Error(`Timed out: ${description}`);
}
async function listen(server) {
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return server.address().port;
}
const token = '000000000:local-runtime-test-token';
const owner = randomInt(900000000, 1000000000);
function initData(id, ageSeconds = 0) {
  const fields = { auth_date: String(Math.floor(Date.now() / 1000) - ageSeconds), user: JSON.stringify({ id, first_name: 'Проверка' }) };
  const check = Object.entries(fields).sort(([a], [b]) => a.localeCompare(b)).map(([key, value]) => `${key}=${value}`).join('\n');
  const secret = createHmac('sha256', 'WebAppData').update(token).digest();
  return new URLSearchParams({ ...fields, hash: createHmac('sha256', secret).update(check).digest('hex') }).toString();
}
const sockets = new Set();
let proxyConnections = 0;
let proxyBytes = 0;
// An intentionally silent local proxy: no request can reach Telegram; cancellation is exercised.
const proxy = createServer((socket) => {
  ++proxyConnections;
  sockets.add(socket);
  socket.on('data', (data) => { proxyBytes += data.length; });
  socket.on('error', () => {});
  socket.on('close', () => sockets.delete(socket));
});
const proxyPort = await listen(proxy);
let dropDatabase = false;
let droppedDatabaseChunks = 0;
let loseCommitResponse = false;
const databaseProxy = createServer((client) => {
  const upstream = connect(Number(database.port || '5432'), database.hostname);
  let swallowCommitResponse = false;
  let tail = Buffer.alloc(0);
  for (const value of [client, upstream]) {
    sockets.add(value);
    value.on('error', () => {});
    value.on('close', () => { sockets.delete(value); client.destroy(); upstream.destroy(); });
  }
  client.on('data', (data) => {
    const command = Buffer.concat([tail, data]);
    if (loseCommitResponse && command.includes(Buffer.from('COMMIT\0'))) {
      loseCommitResponse = false;
      swallowCommitResponse = true;
    }
    tail = command.subarray(Math.max(0, command.length - 6));
    if (dropDatabase) ++droppedDatabaseChunks;
    else upstream.write(data);
  });
  upstream.on('data', (data) => {
    if (dropDatabase || swallowCommitResponse) ++droppedDatabaseChunks;
    else client.write(data);
  });
});
const databaseProxyPort = await listen(databaseProxy);
const proxiedDatabase = new URL(database);
proxiedDatabase.hostname = '127.0.0.1';
proxiedDatabase.port = String(databaseProxyPort);
proxiedDatabase.searchParams.set('hostaddr', '127.0.0.1');
// Only the disposable local fixture: plaintext lets the proxy identify COMMIT.
proxiedDatabase.searchParams.set('sslmode', 'disable');
const reservation = createServer();
const port = await listen(reservation);
await new Promise((resolve) => reservation.close(resolve));
const origin = `http://127.0.0.1:${port}/`;
const env = { ...process.env, DATABASE_URL: process.env.TEST_DATABASE_URL, TELEGRAM_BOT_TOKEN: token,
  MINI_APP_URL: 'https://localhost/', MINI_APP_LISTEN_ADDRESS: '127.0.0.1', MINI_APP_PORT: String(port),
  MINI_APP_BOT_USERNAME: 'cpp_recall_bot',
  MINI_APP_STATIC_DIR: 'docs/miniapp', MINI_APP_WORKER_COUNT: '2', MINI_APP_FEEDBACK_ADMIN_ID: String(owner),
  HTTPS_PROXY: `http://127.0.0.1:${proxyPort}`, https_proxy: `http://127.0.0.1:${proxyPort}`,
  HTTP_PROXY: `http://127.0.0.1:${proxyPort}`, http_proxy: `http://127.0.0.1:${proxyPort}`,
  ALL_PROXY: `http://127.0.0.1:${proxyPort}`, NO_PROXY: '127.0.0.1,localhost', no_proxy: '127.0.0.1,localhost' };
const children = [];
function launch(overrides = {}) {
  const child = spawn(process.argv[2] || './build/cc_recall_bot', [], { env: { ...env, ...overrides }, stdio: ['ignore', 'pipe', 'pipe'] });
  const result = { child, log: '' };
  child.stdout.on('data', (data) => { result.log += data; });
  child.stderr.on('data', (data) => { result.log += data; });
  result.done = new Promise((resolve, reject) => { child.once('exit', (code, signal) => resolve({ code, signal })); child.once('error', reject); });
  children.push(result);
  return result;
}
async function api(route, id = owner, body = {}, authAgeSeconds = 0, timeout = 1500) {
  return fetch(new URL(`api/v1/${route}`, origin), { method: 'POST',
    headers: { Authorization: `tma ${initData(id, authAgeSeconds)}`, 'Content-Type': 'application/json' },
    body: JSON.stringify(body), signal: AbortSignal.timeout(timeout) });
}
async function socket() {
  const value = connect(port, '127.0.0.1');
  sockets.add(value);
  value.on('error', () => {});
  value.on('close', () => sockets.delete(value));
  await new Promise((resolve, reject) => { value.once('connect', resolve); value.once('error', reject); });
  return value;
}
let fixture = false;
try {
  if (process.argv[3]) {
    async function curlProbe(timeout, cancelAfter) {
      const beforeConnections = proxyConnections;
      const beforeBytes = proxyBytes;
      const args = ['--http-probe', `http://127.0.0.1:${proxyPort}/`, String(timeout)];
      if (cancelAfter !== undefined) args.push(String(cancelAfter));
      const child = spawn(process.argv[3], args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
      const probe = { child, log: '' };
      let output = '';
      child.stdout.on('data', (data) => { output += data; });
      child.stderr.on('data', () => {}); // Never echo transport diagnostics or environment.
      probe.done = new Promise((resolve, reject) => {
        child.once('exit', (code, signal) => resolve({ code, signal }));
        child.once('error', reject);
      });
      children.push(probe);
      const result = await Promise.race([probe.done, pause(2500).then(() => {
        child.kill('SIGKILL');
        throw new Error('Local Curl probe exceeded its deadline');
      })]);
      assert.equal(result.code, 0, 'local Curl probe process failed');
      assert.equal(proxyConnections - beforeConnections, 1, 'Curl probe must reach the silent loopback server');
      assert(proxyBytes > beforeBytes, 'the server must receive a real POST before withholding its response');
      return JSON.parse(output);
    }
    for (const timeout of [150, 300]) {
      const probe = await curlProbe(timeout);
      assert.equal(probe.outcome, 'failed', 'the silent server must not produce a successful response');
      assert(probe.elapsed_ms >= timeout * 0.8 && probe.elapsed_ms < timeout + 600,
        'the configured request timeout, not the old 35-second default, must end a stalled response');
    }
    const cancelled = await curlProbe(4000, 75);
    assert.equal(cancelled.outcome, 'cancelled', 'real Curl must observe stop while the response is stalled');
    assert(cancelled.elapsed_ms >= 50 && cancelled.elapsed_ms < 1800,
      'cancellation must interrupt the active request before its four-second timeout');
    console.log('runtime_http_smoke Curl probes passed: real 150/300ms response deadlines and active cancellation');
  }
  const beforeBotConnections = proxyConnections;
  let app = launch();
  const ready = () => until(async () => (await fetch(new URL('health/ready', origin), { signal: AbortSignal.timeout(1000) })).ok, 'HTTP startup');
  await ready();
  await until(() => proxyConnections > beforeBotConnections, 'bot startup reached the isolated proxy');
  const initial = await (await api('bootstrap')).json();
  assert.equal(initial.summary.accuracy, null);
  assert.equal(initial.summary.first_answer_accuracy, null);
  await checkSharedChallengeHttp({ api, origin, userId: owner + 100, connectionCount: () => proxyConnections });

  // The same start request, even concurrently, must create only one durable training.
  const start = { request_id: randomUUID(), modes: ['theory'], topics: ['language_core'], level: 1 };
  const starts = await Promise.all([api('sessions', owner, start), api('sessions', owner, start)]);
  for (const result of starts) assert.equal(result.status, 200);
  const current = await starts[0].json();
  assert.deepEqual(await starts[1].json(), current);
  assert.equal(sql(`SELECT count(*) FROM sessions WHERE user_id = ${owner}`), '1');
  assert.equal(current.phase, 'question');

  // Kill without graceful flushing: question, option order and progress must already be durable.
  app.child.kill('SIGKILL');
  assert.equal((await app.done).signal, 'SIGKILL');
  app = launch();
  await ready();
  const restored = await (await api('bootstrap', owner, {}, 10)).json();
  assert.deepEqual(restored.session, current);
  assert.equal(restored.summary.attempt_count, 0);

  // Read the key only from the local fixture bank; the public API must not expose it.
  function findQuestion(directory, id) {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const filename = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        const found = findQuestion(filename, id);
        if (found) return found;
      } else if (entry.name.endsWith('.json')) {
        const question = JSON.parse(readFileSync(filename, 'utf8'));
        if (question.id === id) return question;
      }
    }
  }
  const question = findQuestion('content/questions', current.question.id);
  assert.equal(question.task.type, 'single_choice', 'recovery fixture needs a single-choice theory card');
  assert(!('correct_option_id' in current.question.task));
  const answer = { question_id: question.id, answer: question.task.correct_option_id };
  const body = JSON.stringify(answer);
  const lost = await socket();
  lost.write(`POST /api/v1/session/answer HTTP/1.1\r\nHost: localhost\r\nAuthorization: tma ${initData(owner)}\r\nContent-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`);
  await until(() => sql(`SELECT count(*) FROM attempts WHERE user_id = ${owner}`) === '1', 'durable answer before dropping receipt');
  lost.destroy(); // Never read the receipt: the client cannot know whether the answer committed.
  const firstReceipt = await (await api('session/answer', owner, answer)).json();
  assert.equal(firstReceipt.session.phase, 'feedback');
  assert.equal(firstReceipt.session.feedback.correct, true);
  assert.equal(firstReceipt.session.progress.total_attempts, 1);
  assert(firstReceipt.session.progress.xp_gained > 0);
  const beforeRestart = await (await api('bootstrap')).json();

  app.child.kill('SIGKILL');
  await app.done;
  app = launch();
  await ready();
  const afterRestart = await (await api('bootstrap', owner, {}, 20)).json();
  assert.deepEqual(afterRestart.session, firstReceipt.session);
  assert.deepEqual(afterRestart.summary, beforeRestart.summary);
  assert.deepEqual(afterRestart.rewards, beforeRestart.rewards);
  assert.deepEqual(await (await api('session/answer', owner, answer)).json(), firstReceipt);
  assert.equal(sql(`SELECT count(*) FROM attempts WHERE user_id = ${owner}`), '1');
  const next = await (await api('session/next')).json();
  assert.equal(next.phase, 'question');
  assert.notEqual(next.question.id, question.id);
  await api('session/stop');

  const fragmented = await socket();
  let response = '';
  fragmented.on('data', (chunk) => { response += chunk; });
  const closed = new Promise((resolve) => fragmented.once('close', resolve));
  fragmented.write('GET /health/li');
  await pause(50);
  fragmented.write('ve HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n');
  await closed;
  assert.match(response, /HTTP\/1\.1 200/);
  assert.match(response, /\{"alive":true\}/);

  // A port conflict must stop the whole second app, not leave its Telegram loop running.
  const conflict = launch();
  const failure = await Promise.race([conflict.done, pause(5000).then(() => { throw new Error('HTTP startup failure did not stop the app'); })]);
  assert.equal(failure.code, 1);
  assert.match(conflict.log, /category=server_terminated/);

  const marker = 'private-error-content-must-not-leak';
  sql(`CREATE OR REPLACE FUNCTION cpp_recall_http_test_failure() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
    IF NEW.user_id = ${owner + 20} THEN RAISE EXCEPTION '${marker}'; END IF; RETURN NEW; END $$;
    CREATE TRIGGER cpp_recall_http_test_failure BEFORE INSERT ON users FOR EACH ROW EXECUTE FUNCTION cpp_recall_http_test_failure();`);
  fixture = true;
  const failed = await api('bootstrap', owner + 20);
  const error = await failed.json();
  assert.equal(failed.status, 500);
  assert.equal(error.error_id, failed.headers.get('x-request-id'));
  assert(error.error_id);
  assert(!JSON.stringify(error).includes(marker));
  assert(!app.log.includes(marker) && !app.log.includes(token) && !app.log.includes('tma '));
  await until(() => app.log.includes(`"error_id":"${error.error_id}"`), 'correlated safe error log');
  sql('DROP TRIGGER cpp_recall_http_test_failure ON users; DROP FUNCTION cpp_recall_http_test_failure();');
  fixture = false;

  // Occupy both workers, fill the bounded queue and overflow it. Valid queued requests drain after deadlines.
  await socket(); await socket();
  await pause(150);
  for (let i = 0; i < 28; ++i) {
    const queued = await socket();
    queued.on('data', () => {});
    queued.write('GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n');
  }
  let technical;
  await until(async () => {
    const result = await api('admin/statistics', owner, { days: 1 });
    if (!result.ok) return false;
    technical = (await result.json()).technical;
    return technical.http_read_errors >= 2;
  }, 'idle read deadlines and queue recovery');
  assert(technical.rejected_connections > 0, 'queue rejection disappeared from metrics');
  assert(technical.executor.rejected > 0 && technical.executor.pending_limit === 16);
  assert(technical.executor.active <= 2 && technical.executor.pending <= 16);

  // Graceful stop must also finish with clients still waiting to send a request.
  await socket(); await socket();
  await pause(150);
  app.child.kill('SIGTERM');
  const stopped = await Promise.race([app.done, pause(3000).then(() => { throw new Error('Idle clients blocked shutdown'); })]);
  assert.equal(stopped.code, 0);

  // An established database socket stays open but all traffic disappears.
  app = launch({ DATABASE_URL: proxiedDatabase.toString() });
  await ready();
  dropDatabase = true;
  const stalledAt = Date.now();
  const stalled = await api('bootstrap', owner + 30, {}, 0, 9000);
  assert(droppedDatabaseChunks > 0, 'test did not actually stall database traffic');
  assert.equal(stalled.status, 500);
  assert((await stalled.json()).error_id);
  assert(Date.now() - stalledAt < 8500, 'database I/O deadline was not enforced');
  assert.equal(sql(`SELECT count(*) FROM users WHERE user_id = ${owner + 30}`), '0', 'a dropped request was replayed');
  dropDatabase = false;
  await ready();
  assert.equal((await api('bootstrap', owner + 30)).status, 200, 'database did not recover after the I/O timeout');

  // COMMIT succeeded on PostgreSQL, but its result never reaches the application.
  const commitOwner = owner + 32;
  const commitSession = await (await api('sessions', commitOwner, { ...start, request_id: randomUUID() })).json();
  const commitQuestion = findQuestion('content/questions', commitSession.question.id);
  const commitAnswer = { question_id: commitQuestion.id, answer: commitQuestion.task.correct_option_id };
  loseCommitResponse = true;
  const unknownCommit = await api('session/answer', commitOwner, commitAnswer, 0, 9000);
  assert.equal(unknownCommit.status, 500);
  assert.equal(loseCommitResponse, false, 'test did not intercept COMMIT');
  assert.equal(sql(`SELECT count(*) FROM attempts WHERE user_id = ${commitOwner}`), '1');
  const committedXp = sql(`SELECT sum(delta) FROM xp_ledger WHERE user_id = ${commitOwner}`);
  const committedVersion = sql(`SELECT state_version FROM active_training_sessions WHERE user_id = ${commitOwner}`);
  const recoveredCommit = await (await api('session/answer', commitOwner, commitAnswer)).json();
  assert.equal(recoveredCommit.session.feedback.correct, true);
  assert.equal(recoveredCommit.session.progress.total_attempts, 1);
  assert.equal(sql(`SELECT sum(delta) FROM xp_ledger WHERE user_id = ${commitOwner}`), committedXp, 'retry duplicated XP after unknown COMMIT');
  assert.equal(sql(`SELECT state_version FROM active_training_sessions WHERE user_id = ${commitOwner}`), committedVersion);

  // The completion and exact result must also survive an unknown COMMIT,
  // followed by a process crash. A later training must not be affected.
  const resultOwner = owner + 33;
  let stage = await (await api('sessions', resultOwner, { route_id: 'containers_iterators' })).json();
  for (let index = 0; index < 2; ++index) {
    const card = findQuestion('content/questions', stage.question.id);
    const response = await api('session/answer', resultOwner,
      { session_id: stage.session_id, question_id: card.id, answer: card.task.correct_option_id });
    assert.equal(response.status, 200);
    stage = (await response.json()).session;
    if (index === 0) stage = await (await api('session/next', resultOwner,
      { session_id: stage.session_id, expected_total_attempts: stage.progress.total_attempts })).json();
  }
  const completionRequest = { session_id: stage.session_id, expected_total_attempts: stage.progress.total_attempts };
  const beforeCompletion = sql(`SELECT sum(delta) FROM xp_ledger WHERE user_id = ${resultOwner}`);
  loseCommitResponse = true;
  const unknownCompletion = await api('session/next', resultOwner, completionRequest, 0, 9000);
  assert.equal(unknownCompletion.status, 500);
  assert.equal(loseCommitResponse, false);
  assert.equal(sql(`SELECT count(*) FROM training_completion_receipts WHERE user_id = ${resultOwner}`), '1');
  assert.equal(sql(`SELECT count(*) FROM active_training_sessions WHERE user_id = ${resultOwner}`), '0');
  const completion = await (await api('session/result', resultOwner, { session_id: stage.session_id })).json();
  assert.equal(completion.status, 'completed');
  assert.equal(completion.learning.completed, 2);
  assert.equal(completion.summary.sessions_completed, 1);
  assert.equal((await (await api('bootstrap', resultOwner)).json()).session, null);
  // Replay before restart must clear only the stale in-memory final session.
  assert.deepEqual(await (await api('session/next', resultOwner, completionRequest)).json(), completion);
  assert.equal((await (await api('bootstrap', resultOwner)).json()).session, null);
  app.child.kill('SIGKILL');
  await app.done;
  app = launch({ DATABASE_URL: proxiedDatabase.toString() });
  await ready();
  const later = await (await api('sessions', resultOwner, { route_id: 'map_lookup' })).json();
  assert.deepEqual(await (await api('session/next', resultOwner, completionRequest)).json(), completion);
  assert.deepEqual((await (await api('bootstrap', resultOwner)).json()).session, later);
  assert.equal(sql(`SELECT sum(delta) FROM xp_ledger WHERE user_id = ${resultOwner}`), beforeCompletion);
  assert.equal(sql(`SELECT count(*) FROM learning_events WHERE user_id = ${resultOwner} AND event_name = 'training_finished' AND outcome = 'completed'`), '1');
  assert.deepEqual(await (await api('session/result', owner, { session_id: stage.session_id })).json(), { status: 'unavailable' });

  // SIGTERM must interrupt a database wait, not wait for its full I/O deadline.
  dropDatabase = true;
  droppedDatabaseChunks = 0;
  const pending = api('bootstrap', owner + 31, {}, 0, 9000).catch(() => null);
  await until(() => droppedDatabaseChunks > 0, 'active database wait before shutdown', 2000);
  app.child.kill('SIGTERM');
  const databaseStopped = await Promise.race([app.done, pause(2000).then(() => { throw new Error('Database wait blocked shutdown'); })]);
  assert.equal(databaseStopped.code, 0);
  await pending;
  console.log('runtime_http_smoke passed: concurrent start, lost answer/completion receipts, crash recovery, renewed auth, metrics, partial HTTP, safe 500, fatal startup, overload, deadlines, database blackhole/recovery, unknown answer/completion COMMIT, graceful database stop');
} finally {
  try {
    if (fixture) sql('DROP TRIGGER IF EXISTS cpp_recall_http_test_failure ON users; DROP FUNCTION IF EXISTS cpp_recall_http_test_failure();');
  } finally {
    for (const item of children) if (item.child.exitCode === null) item.child.kill('SIGTERM');
    for (const value of sockets) value.destroy();
    await new Promise((resolve) => proxy.close(resolve));
    await new Promise((resolve) => databaseProxy.close(resolve));
    for (const item of children) {
      await Promise.race([item.done, pause(3000).then(() => { item.child.kill('SIGKILL'); })]);
    }
  }
}
