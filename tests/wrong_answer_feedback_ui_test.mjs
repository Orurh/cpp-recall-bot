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
    focus() { focused = id; }, getContext() { return null; },
    set innerHTML(value) { throw new Error(`Unsafe HTML rendering: ${value}`); } };
}
for (const [, id] of html.matchAll(/\bid="([^"]+)"/g)) nodes.set(id, node(id));
const byId = id => {
  assert(nodes.has(id), `UI refers to missing HTML element ${id}`);
  return nodes.get(id);
};
const requests = [];
let reply;
const sandbox = {
  URL, AbortController, console,
  document: { getElementById: byId, documentElement: node(), body: node(),
    createElement: () => node(), querySelectorAll: () => [], addEventListener() {} },
  window: { location: { href: 'https://local.invalid/' }, crypto: { randomUUID: () => 'local-test' },
    Telegram: { WebApp: { initData: 'synthetic-init-data', ready() {}, expand() {} } },
    scrollTo() {}, setTimeout, clearTimeout, setInterval() {}, addEventListener() {} },
  fetch: async (url, options) => {
    assert.equal(options.headers.Authorization, 'tma synthetic-init-data');
    requests.push({ path: url.pathname, body: JSON.parse(options.body) });
    return reply();
  },
};
// Exercise the complete production script and event wiring, replacing only boot.
assert.equal(source.split('  boot();').length, 2);
vm.runInNewContext(source.replace('  boot();',
  '  globalThis.testUI = {state, renderFeedback, renderTheory, closeTheory, revealAnswer, bugHunterLines, renderBugHunter};'), sandbox);
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
console.log('wrong_answer_feedback_ui passed (minimal DOM; no browser, network or DB)');
