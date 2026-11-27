// Dependency-free rendering/transport test with a minimal DOM, not a browser audit.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const html = readFileSync('docs/miniapp/index.html', 'utf8');
const source = readFileSync('docs/miniapp/app.js', 'utf8');
const nodes = new Map();
let focused;
function node(id = '') {
  return { id, hidden: true, disabled: false, dataset: {}, textContent: '', events: {},
    style: { removeProperty() {}, setProperty() {} },
    classList: { add() {}, remove() {}, toggle() {} },
    addEventListener(type, listener) { (this.events[type] ||= []).push(listener); },
    setAttribute(name, value) { (this.attributes ||= {})[name] = value; }, removeAttribute() {},
    append(...children) { (this.children ||= []).push(...children); },
    replaceChildren(...children) { this.children = children; },
    focus() { focused = id; }, getContext() { return null; }, querySelector() { return node(); },
    open: false, showModal() { this.open = true; }, close() { this.open = false; this.events.close?.forEach(fn => fn()); },
    set innerHTML(value) { throw new Error(`Unsafe HTML rendering: ${value}`); } };
}
for (const [, id] of html.matchAll(/\bid="([^"]+)"/g)) nodes.set(id, node(id));
const byId = id => {
  assert(nodes.has(id), `UI refers to missing HTML element ${id}`);
  return nodes.get(id);
};
const requests = [];
let reply;
const storage = new Map();
let nativeSettingsClick, nativeBackClick, notesPlayed = 0, resumeAudio, audio;
let haptics = 0;
class FakeAudioContext {
  constructor() { audio = this; this.state = 'suspended'; this.currentTime = 0; this.destination = {}; }
  resume() { return new Promise(resolve => { resumeAudio = () => { this.state = 'running'; resolve(); }; }); }
  createGain() { return {gain: {value: 1, setValueAtTime() {}, exponentialRampToValueAtTime() {}}, connect() {}, disconnect() {}}; }
  createOscillator() { return {frequency: {}, connect() {}, disconnect() {}, start() { notesPlayed++; }, stop() {}}; }
}
const sandbox = {
  URL, AbortController, console,
  localStorage: { getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
  document: { getElementById: byId, documentElement: node(), body: node(),
    createElement: () => node(), querySelectorAll: () => [], addEventListener() {} },
  window: { location: { href: 'https://local.invalid/' }, crypto: { randomUUID: () => 'local-test' },
    AudioContext: FakeAudioContext,
    Telegram: { WebApp: { initData: 'synthetic-init-data', ready() {}, expand() {}, isVersionAtLeast: () => true,
      SettingsButton: {isVisible: false, show() { this.isVisible = true; }, hide() { this.isVisible = false; },
        onClick(fn) { nativeSettingsClick = fn; }},
      BackButton: {isVisible: false, show() { this.isVisible = true; }, hide() { this.isVisible = false; },
        onClick(fn) { nativeBackClick = fn; }},
      HapticFeedback: {notificationOccurred() { haptics++; }} } },
    scrollY: 0, scrollTo({top}) { this.scrollY = top; }, setTimeout, clearTimeout, setInterval() {}, addEventListener() {} },
  fetch: async (url, options) => {
    assert.equal(options.headers.Authorization, 'tma synthetic-init-data');
    requests.push({ path: url.pathname, body: JSON.parse(options.body) });
    return reply();
  },
};
// Exercise the complete production script and event wiring, replacing only boot.
assert.equal(source.split('  boot();').length, 2);
vm.runInNewContext(source.replace('  boot();',
  '  globalThis.testUI = {state, renderFeedback, renderTheory, closeTheory, revealAnswer, bugHunterLines, renderBugHunter, renderSession, selectMatch, celebrateAnswer, celebrateCompletion, haptic, profileXpRank, renderXpRank};'), sandbox);
const ui = sandbox.testUI;
const bugCard = JSON.parse(readFileSync('content/questions/containers/vector/vector_reserve_bug_line.json', 'utf8'));
// This fixture models the public payload: no key, explanation or source is sent.
const bug = { kind: bugCard.kind, tags: bugCard.tags, code: bugCard.code,
  task: {type: 'single_choice', options: [...bugCard.task.options].reverse()} };
const mapped = ui.bugHunterLines(bug);
assert(mapped);
const group = ui.renderBugHunter(mapped);
assert.equal(group.children.length, bug.code.split('\n').length + 1); // legend + every code line
const radios = group.children.flatMap(row => row.children || []).filter(n => n.type === 'radio');
assert.deepEqual(radios.map(r => r.value), ['line_4', 'line_5', 'line_6']);
assert(radios.every(r => r.name === 'answer' && r.attributes['aria-label'].includes('Строка')));
assert.equal(requests.length, 0, 'line rendering must not submit an answer');
for (const id of ['line_0', 'line_04', 'line_-1', 'line_+4', 'line_4x', 'line_8', 'line_2', 'line_99999999999999999999']) {
  const broken = structuredClone(bug); broken.task.options[0].id = id;
  assert.equal(ui.bugHunterLines(broken), null, `invalid ID accepted: ${id}`);
}
for (const change of [q => { q.kind = 'code_reading'; }, q => { q.tags = []; },
  q => { q.code = ''; }, q => { q.task.type = 'multiple_choice'; },
  q => { q.task.options.pop(); q.task.options.pop(); },
  q => { q.task.options[0].text = 'wrong line description'; },
  q => { q.task.options[0] = q.task.options[1]; }]) {
  const broken = structuredClone(bug); change(broken);
  assert.equal(ui.bugHunterLines(broken), null);
}
const literal = structuredClone(bug);
literal.code = literal.code.replace('v[0] = 42;', '<img src=x onerror=alert(1)>');
literal.task.options.find(option => option.id === 'line_6').text = 'Строка 6: <img src=x onerror=alert(1)>';
const literalRows = ui.renderBugHunter(ui.bugHunterLines(literal));
assert.equal(literalRows.children[6].children[1].textContent, '  <img src=x onerror=alert(1)>');

const wrong = { question_id: 'card', content_revision: 'a'.repeat(64), correct: false, completed: false, answer: '<img src=x onerror=alert(1)>',
  attempts: 1, answer_available: true, explanation_available: true, theory_available: false,
  previous_topic_level: 1, topic_level: 1, previous_adaptive_level: 1, adaptive_level: 1 };
function render(feedback, shared = false) {
  ui.state.session = { session_id: 42, phase: 'feedback', feedback, shared_challenge_id: shared ? 'shared' : '' };
  ui.renderFeedback(feedback);
}
render(wrong);
assert.match(byId('result-title').textContent, /Неверно/);
assert.equal(byId('result-answer').textContent, `Твой ответ: ${wrong.answer}`);
assert.equal(byId('result-correct-answer').hidden, true);
assert.equal(byId('explanation').hidden, true);
assert.equal(byId('open-theory').hidden, true);
assert.equal(byId('reveal-answer').hidden, false, 'no skill theory must not block card explanation');
assert.equal(byId('reveal-answer').textContent, 'Показать ответ и объяснение');
assert.match(byId('answer-help-note').textContent || html, /XP/);
assert.match(html, /id="reveal-answer"[^>]*aria-describedby="answer-help-note"/);
assert.match(html, /id="explanation"[^>]*tabindex="-1"/);

// Failed delivery must leave a retryable action and must not invent disclosed content.
reply = () => { throw new Error('lost response'); };
await byId('reveal-answer').events.click[0]();
assert.equal(byId('feedback-error').textContent, 'lost response');
assert.equal(byId('result-correct-answer').hidden, true);
assert.equal(byId('reveal-answer').hidden, false);
assert.deepEqual(requests[0], { path: '/api/v1/session/theory',
  body: { session_id: 42, question_id: 'card', content_revision: wrong.content_revision, kind: 'answer' } });

const revealed = { ...wrong, answer_revealed: true, theory_viewed: true,
  correct_answer: 'std::vector<T> & <script>key</script>', explanation: '<script>rationale()</script>\n& text',
  source: 'javascript:alert(1)' };
let finish;
reply = () => new Promise(resolve => { finish = () => resolve({ ok: true,
  json: async () => ({ session: { ...ui.state.session, feedback: revealed } }) }); });
const opening = ui.revealAnswer();
const requestCount = requests.length;
await ui.revealAnswer();
assert.equal(requests.length, requestCount, 'double click issued a second opening');
finish();
await opening;
assert.match(byId('result-title').textContent, /Неверно/);
assert.equal(byId('result-correct-answer').textContent, `Правильный ответ: ${revealed.correct_answer}`);
assert.equal(byId('explanation-text').textContent, revealed.explanation);
assert.equal(byId('explanation').hidden, false);
assert.equal(byId('explanation-source').hidden, true, 'active source URL was rendered');
assert.equal(byId('reveal-answer').hidden, true);
assert.equal(byId('theory-note').hidden, false);
assert.equal(focused, 'explanation');
assert.equal(byId('next-question').textContent, 'Продолжить');
render(structuredClone(revealed)); // Reloaded feedback remains disclosed, still wrong.
assert.equal(byId('explanation').hidden, false);
const beforeResume = requests.length;
byId('feedback').hidden = true;
byId('home').hidden = false;
byId('quick-start').events.click[0](); // Existing Mini App continuation entry.
assert.equal(byId('feedback').hidden, false);
assert.equal(byId('explanation-text').textContent, revealed.explanation);
assert.equal(requests.length, beforeResume, 'resuming saved feedback resubmitted an answer or opening');

for (const explanation of [undefined, '', ' \n\t']) {
  render({ ...revealed, explanation });
  assert.equal(byId('explanation-text').textContent, 'Объяснение для этой редакции карточки пока недоступно.');
}
render({ ...wrong, explanation_available: false });
assert.equal(byId('reveal-answer').textContent, 'Показать правильный ответ');
render({ ...wrong, answer_available: false });
assert.equal(byId('reveal-answer').hidden, true);
render({ ...wrong, theory_available: true });
assert.equal(byId('open-theory').hidden, false);
assert.equal(byId('open-theory').textContent, 'Разобрать принцип');
render(wrong, true);
assert.match(byId('result-title').textContent, /Неверно/);
assert.equal(byId('next-question').textContent, 'Попробовать ещё раз');

render({ ...revealed, correct: true, assisted: true, source: 'https://example.invalid/rule' });
assert.equal(byId('result-title').textContent, 'Верно с помощью');
assert.equal(byId('result-answer').textContent, `Ответ: ${revealed.answer}`);
assert.equal(byId('result-correct-answer').hidden, true, 'correct result repeats the accepted answer');
assert.equal(byId('result-correct-answer').textContent, '', 'previous revealed answer was retained');
assert.equal(byId('reveal-answer').hidden, true);
assert.equal(byId('open-theory').hidden, true);
assert.equal(byId('explanation-source').hidden, false);
assert.match(byId('level-change').textContent, /Без XP/);
render({ ...revealed, correct: true, assisted: false });
assert.equal(byId('result-title').textContent, 'Верно самостоятельно');
assert.equal(byId('explanation').hidden, false);

// Daily is one durable card. Its completion uses the existing guarded Next
// endpoint; a wrong answer still needs repair, not a fictitious next card.
for (const feedback of [wrong, {...revealed, correct: true, assisted: true},
  {...revealed, correct: true, attempts: 2}, {...revealed, correct: true, attempts: 1}]) {
  render(feedback);
  ui.state.session.daily_day = '2026-11-26';
  ui.renderFeedback(feedback);
  assert.equal(byId('next-question').textContent, feedback.correct ? 'Завершить задание' : 'Попробовать ещё раз');
  if (!feedback.correct) assert.equal(byId('result-title').textContent, 'Неверно. Попробуем ещё раз');
  ui.state.session = structuredClone(ui.state.session);
  ui.renderFeedback(ui.state.session.feedback);
  assert.equal(byId('next-question').textContent, feedback.correct ? 'Завершить задание' : 'Попробовать ещё раз');
}
render({...revealed, correct: true});
assert.equal(byId('next-question').textContent, 'Следующий вопрос', 'ordinary training navigation was changed');

// Server verdict, not string equality, determines success. An accepted alternative
// spelling must not look wrong merely because the stored example differs.
for (const answer of ['& it -> second . value', 'a, c', 'A → 2; B → 1', '110']) {
  render({ ...wrong, correct: true, answer, correct_answer: 'another accepted spelling',
    attempts: 2, explanation: 'Разбор правила.' });
  assert.equal(byId('result-title').textContent, 'Верно после повторной попытки');
  assert.equal(byId('result-answer').textContent, `Ответ: ${answer}`);
  assert.equal(byId('result-correct-answer').hidden, true);
  assert.equal(byId('result-correct-answer').textContent, '');
  assert.equal(byId('explanation').hidden, false);
}

// Returning to an unrevealed mistake must clear the previous key and explanation,
// even if an old response happens to carry those fields. Help stays opt-in.
render({ ...revealed, answer_revealed: false, theory_viewed: false });
assert.equal(byId('result-answer').textContent, `Твой ответ: ${revealed.answer}`);
assert.equal(byId('result-correct-answer').hidden, true);
assert.equal(byId('result-correct-answer').textContent, '');
assert.equal(byId('explanation').hidden, true);
assert.equal(byId('explanation-text').textContent, '');
assert.equal(byId('explanation-source').hidden, true);
assert.equal(byId('explanation-source').href, '#');
assert.equal(byId('reveal-answer').hidden, false);

ui.renderTheory({ title: '<b>rule</b>', principle: '<script>rule()</script>', mechanism: '& mechanism',
  example: 'std::vector<T>', common_mistake: 'literal < >',
  sources: ['javascript:alert(1)', 'https://example.invalid/rule'] });
assert.equal(byId('theory-principle').textContent, '<script>rule()</script>');
assert.equal(byId('theory-sources').children.length, 1);
assert.equal(byId('theory-sources').children[0].href, 'https://example.invalid/rule');
assert.equal(byId('theory-dialog').hidden, false);
ui.closeTheory();
assert.equal(byId('theory-dialog').hidden, true);
// Native confirmation is asynchronous: double taps and a changed session
// must never stop a different training.
let confirmCallback;
let confirmCount = 0;
sandbox.window.Telegram.WebApp.showConfirm = (_message, callback) => {
  confirmCount++;
  confirmCallback = callback;
};
render(wrong);
const beforeStop = requests.length;
const stop = byId('finish-session').events.click[0]();
await byId('finish-session').events.click[0]();
assert.equal(confirmCount, 1, 'duplicate stop confirmation opened');
ui.state.session = { ...ui.state.session, session_id: 99 };
confirmCallback(true);
await stop;
assert.equal(requests.length, beforeStop, 'stale confirmation stopped a different session');
assert.equal(ui.state.session.session_id, 99);
const canceled = byId('finish-session').events.click[0]();
assert.equal(confirmCount, 2, 'stale confirmation left the action locked');
confirmCallback(false);
await canceled;
assert.equal(requests.length, beforeStop, 'canceling confirmation sent a stop request');
// Simulate server-driven rerender of one matching card, including scrolling
// during a pending request and the real transition into answer feedback.
const matchQuestion = { id: 'match', kind: 'theory', task: { type: 'matching', board: {
  revision: 1, total: 2, matched: 0, mistakes: 0,
  left: [{position: 0, text: 'L1'}, {position: 1, text: 'L2'}],
  right: [{position: 0, text: 'R1'}, {position: 1, text: 'R2'}],
}}};
ui.state.session = { session_id: 101, phase: 'question', question: matchQuestion,
  progress: {question_number: 1, session_goal: 2, completed: 0, attempt: 1} };
ui.renderSession();
assert.equal(sandbox.window.scrollY, 0);
for (const [side, outcome] of [['left', 'selected'], ['right', 'matched'], ['left', 'mismatch']]) {
  sandbox.window.scrollY = 400;
  let respond;
  reply = () => new Promise(resolve => { respond = resolve; });
  const pending = ui.selectMatch('match', side, 0, matchQuestion.task.board.revision);
  sandbox.window.scrollY = 520;
  matchQuestion.task.board.revision++;
  respond({ok: true, json: async () => ({outcome, session: structuredClone(ui.state.session)})});
  await pending;
  assert.equal(sandbox.window.scrollY, 520, 'matching update reset the current scroll position');
}
sandbox.window.scrollY = 520;
reply = async () => ({ok: true, json: async () => ({
  outcome: 'finished', session: {session_id: 101, phase: 'feedback', feedback: {...wrong, correct: true}},
})});
await ui.selectMatch('match', 'right', 0, matchQuestion.task.board.revision);
assert.equal(sandbox.window.scrollY, 0, 'new feedback screen must start at the top');
// Celebration is presentation-only and is driven by an accepted server cursor.
ui.state.bootstrap = {user: {id: 123}};
const clean = {...wrong, correct: true, completed: true, assisted: false, attempts: 1};
const prior = {session_id: 1234, phase: 'question', question: {id: clean.question_id},
  progress: {total_attempts: 4}};
const milestone = {session_id: 1234, phase: 'feedback', feedback: clean,
  progress: {total_attempts: 5, clean_streak: 5}};
ui.state.session = milestone;
ui.renderFeedback(clean);
assert.equal(byId('practice-streak').hidden, true, 'restoring feedback replayed celebration');
ui.celebrateAnswer(prior, milestone);
assert.equal(byId('practice-streak').hidden, false);
ui.renderFeedback(clean);
ui.celebrateAnswer(prior, milestone);
assert.equal(byId('practice-streak').hidden, true, 'duplicate response replayed celebration');
for (const feedback of [{...clean, assisted: true}, {...clean, attempts: 2}, wrong]) {
  const before = {...prior, session_id: prior.session_id + 1};
  const after = {...milestone, session_id: before.session_id, feedback};
  ui.celebrateAnswer(before, after);
  assert.equal(byId('practice-streak').hidden, true, 'non-clean answer celebrated as clean');
  prior.session_id++;
}
const perfect = {status: 'completed', session_id: 2000, learning: {
  completed: 10, first_try_correct: 10, corrected_after_error: 0, assisted_first_try: 0}};
byId('practice-perfect').hidden = true;
ui.celebrateCompletion({...perfect, learning: {...perfect.learning, first_try_correct: 9}});
assert.equal(byId('practice-perfect').hidden, true);
ui.celebrateCompletion(perfect);
assert.equal(byId('practice-perfect').hidden, false);
byId('practice-perfect').hidden = true;
ui.celebrateCompletion(perfect);
assert.equal(byId('practice-perfect').hidden, true);
assert.equal(byId('feedback-sound').checked, false);
const sessionBeforeSettings = ui.state.session;
const requestsBeforeSettings = requests.length;
ui.renderFeedback(clean);
assert.equal(sandbox.window.Telegram.WebApp.SettingsButton.isVisible, true);
nativeSettingsClick();
assert.equal(byId('user-settings-dialog').open, true);
assert.equal(byId('test-feedback-sound').disabled, true);
byId('feedback-sound').checked = true;
const preview = byId('feedback-sound').events.change[0]();
assert.equal(storage.get('cpp-recall-feedback-sound-v1'), 'on');
assert.equal(notesPlayed, 0, 'sound must wait for resume');
resumeAudio();
await preview;
assert.equal(notesPlayed, 2, 'enabling sound must play the preview after asynchronous resume');
assert.match(byId('feedback-sound-status').textContent, /Тестовый сигнал/);
audio.state = 'interrupted';
const interruptedPreview = byId('test-feedback-sound').events.click[0]();
resumeAudio();
await interruptedPreview;
assert.equal(notesPlayed, 4, 'interrupted audio did not recover');
audio.state = 'suspended';
const pendingPreview = byId('test-feedback-sound').events.click[0]();
byId('feedback-sound').checked = false;
byId('feedback-sound').events.change[0]();
resumeAudio();
await pendingPreview;
assert.equal(notesPlayed, 4, 'muting must cancel a pending preview');
assert.equal(storage.get('cpp-recall-feedback-sound-v1'), 'off');
assert.equal(byId('test-feedback-sound').disabled, true);
byId('feedback-vibration').checked = false;
byId('feedback-vibration').events.change[0]();
const hapticsBeforeMute = haptics;
ui.haptic('success');
assert.equal(haptics, hapticsBeforeMute);
assert.equal(storage.get('cpp-recall-feedback-vibration-v1'), 'off');
byId('feedback-animation').checked = false;
byId('feedback-animation').events.change[0]();
assert.equal(storage.get('cpp-recall-feedback-animation-v1'), 'off');
nativeBackClick();
assert.equal(byId('user-settings-dialog').open, false);
assert.equal(sandbox.window.Telegram.WebApp.BackButton.isVisible, true);
assert.equal(ui.state.session, sessionBeforeSettings, 'settings changed session');
assert.equal(requests.length, requestsBeforeSettings, 'settings unexpectedly made API requests');
byId('open-user-settings').events.click[0]();
assert.equal(byId('user-settings-dialog').open, true);
byId('close-user-settings').events.click[0]();
assert.equal(byId('user-settings-dialog').open, false);
// Cosmetic levels depend only on persisted server XP; no reward or difficulty gate.
const floors = [0, 150, 400, 800, 1500, 3000, 5000];
for (let i = 0; i < floors.length; ++i) {
  assert.equal(ui.profileXpRank(floors[i]).level, i + 1);
  if (i > 0) assert.equal(ui.profileXpRank(floors[i] - 1).level, i);
}
assert.equal(ui.profileXpRank(-1), null);
assert.equal(ui.profileXpRank(undefined), null);
assert.equal(ui.profileXpRank(99999).next, null);
ui.state.bootstrap.summary = {xp: {total: 250}, adaptive_level: 10};
ui.renderXpRank('home');
assert.equal(byId('profile-xp-overview').hidden, false);
assert.equal(byId('profile-rank-title').textContent, 'Уровень профиля 2');
assert.equal(byId('profile-rank-progress').max, 250);
assert.equal(byId('profile-rank-progress').value, 100);
assert.equal(byId('profile-rank-next').textContent, 'До уровня 3: 150 XP');
ui.renderXpRank('training');
assert.equal(byId('profile-xp-overview').hidden, true);
ui.state.bootstrap.summary.xp.total = 5000;
ui.renderXpRank('completed');
assert.equal(byId('profile-rank-title').textContent, 'Уровень профиля 7');
assert.equal(byId('profile-rank-progress').value, 1);
assert.equal(byId('profile-rank-next').textContent, 'Последний доступный уровень профиля');
console.log('wrong_answer_feedback_ui passed (minimal DOM; no browser, network or DB)');
