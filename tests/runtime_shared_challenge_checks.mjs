// Local process/HTTP checks against a silent proxy, never a real Telegram API.
import assert from 'node:assert/strict';

export async function checkSharedChallengeHttp({ api, origin, userId, connectionCount }) {
  const json = async (route, uid, body = {}) => {
    const response = await api(route, uid, body);
    assert.equal(response.status, 200, `${route}: ${response.status}`);
    return response.json();
  };
  const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async condition => {
    const deadline = Date.now() + 2000;
    while (!condition() && Date.now() < deadline) await pause(10);
    assert(condition(), 'native request did not reach the local silent proxy');
  };
  const requestFor = session => ({ session_id: session.session_id, question_id: session.question.id,
    content_revision: session.question.content_revision });
  const before = await json('bootstrap', userId);
  const session = await json('sessions', userId, { challenge_id: 'review_212_v1' });
  await json('bootstrap', userId + 1);
  const other = await json('sessions', userId + 1, { challenge_id: 'review_213_v1' });
  const body = requestFor(session);
  const startConnections = connectionCount();
  const plain = await json('session/share', userId, body);
  assert.equal(plain.url, 'https://t.me/cpp_recall_bot?startapp=review_212_v1');
  assert.deepEqual(await json('session/share', userId, { ...body, native: false }), plain);
  for (const invalid of ['true', 1, null, {}]) {
    assert.equal((await api('session/share', userId, { ...body, native: invalid })).status, 400);
  }
  assert.equal((await api('session/share', userId,
    { ...body, native: true, content_revision: '0'.repeat(64) })).status, 409);
  assert.equal((await api('session/share', userId,
    { ...body, native: true, question_id: 'not-the-current-question' })).status, 409);
  assert.equal((await api('session/share', userId + 1, { ...body, native: true })).status, 409);
  assert.equal(connectionCount(), startConnections, 'invalid or plain sharing contacted Telegram');

  const started = Date.now();
  // Even a malicious body cannot change what is prepared for authenticated userId.
  const pending = api('session/share', userId, { ...body, native: true, user_id: userId + 1,
    url: 'https://invalid.example/', text: 'not server content', result: {} }, 0, 7000)
    .then(response => ({ response }), error => ({ error }));
  await until(() => connectionCount() > startConnections);
  const whilePreparing = Date.now();
  const current = await json('session', userId);
  assert.equal(current.session_id, session.session_id, 'native preparation blocked same-user session access');
  assert.deepEqual(await (await fetch(new URL('health/live', origin), {signal: AbortSignal.timeout(1500)})).json(),
    { alive: true });
  const busy = await json('session/share', userId + 1, { ...requestFor(other), native: true });
  assert.equal(busy.prepared_message, undefined, 'a second native preparation should fall back without waiting');
  assert(Date.now() - whilePreparing < 1800, 'native sharing starved ordinary HTTP requests');
  const outcome = await pending;
  assert.ifError(outcome.error);
  assert.equal(outcome.response.status, 200);
  const fallback = await outcome.response.json();
  assert.deepEqual(fallback, plain, 'Telegram blackhole changed safe URL/text or leaked upstream details');
  const elapsed = Date.now() - started;
  assert(elapsed >= 2500 && elapsed < 6500, `native request did not respect its four-second budget: ${elapsed}ms`);
  const after = await json('bootstrap', userId);
  assert.equal(after.session.session_id, session.session_id);
  assert.equal(after.session.phase, 'question');
  assert.deepEqual(after.session.question.task, session.question.task);
  assert(Number.isSafeInteger(before.summary.xp.total), 'XP fixture is missing');
  assert.equal(after.summary.xp.total, before.summary.xp.total);
}
