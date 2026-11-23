// Real app.js and event wiring in a minimal DOM; browser inspection is separate.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const html = readFileSync('docs/miniapp/index.html', 'utf8');
const source = readFileSync('docs/miniapp/app.js', 'utf8');
assert(html.includes('id="delayed-practice"'), 'the optional delayed-practice surface is missing');
assert(html.indexOf('id="saved-trainings"') < html.indexOf('id="delayed-practice"'),
  'saved training must retain visual priority');
const nodes = new Map();
function node(id = '') {
  return {id, hidden: true, disabled: false, dataset: {}, textContent: '', events: {},
    style: {removeProperty() {}, setProperty() {}},
    classList: {add() {}, remove() {}, toggle() {}},
    addEventListener(name, handler) {this.events[name] = handler;},
    setAttribute() {}, removeAttribute() {}, append() {}, replaceChildren() {},
    getContext() {return null;}, focus() {}};
}
for (const [, id] of html.matchAll(/\bid="([^"]+)"/g)) nodes.set(id, node(id));
const get = id => {assert(nodes.has(id), `missing DOM node ${id}`); return nodes.get(id);};
const requests = [];
let response;
const sandbox = {URL, AbortController, console,
  document: {getElementById: get, documentElement: node(), body: node(),
    createElement: () => node(), querySelectorAll: () => [], addEventListener() {}},
  window: {location: {href: 'https://local.invalid/'}, crypto: {randomUUID: () => '23000000-0000-4000-8000-000000000001'},
    Telegram: {WebApp: {initData: 'synthetic', ready() {}, expand() {}}},
    scrollTo() {}, setTimeout, clearTimeout, setInterval() {}, addEventListener() {}},
  fetch: async (url, options) => {
    requests.push({path: url.pathname, body: JSON.parse(options.body)});
    return response();
  }};
vm.runInNewContext(source.replace('  boot();',
  '  globalThis.testUI = {state, renderDelayedPractice, startDelayedPractice};'), sandbox);
const ui = sandbox.testUI;
ui.state.bootstrap = {delayed_practice: null};
ui.renderDelayedPractice();
assert(get('delayed-practice').hidden);
ui.state.bootstrap.delayed_practice = {id: 'raii_state_restore/v1', status: 'ready', familiar: false};
ui.renderDelayedPractice();
assert.equal(get('delayed-practice').hidden, false);
assert.equal(get('delayed-practice-title').textContent, 'Проверка после паузы');
assert.equal(get('start-delayed-practice').textContent, 'Решить задание');
assert.equal(requests.length, 0, 'rendering a recommendation must not deliver its question');
assert.equal(typeof get('start-delayed-practice').events.click, 'function');
ui.state.bootstrap.delayed_practice.familiar = true;
ui.renderDelayedPractice();
assert.equal(get('delayed-practice-title').textContent, 'Повторная проверка');
ui.state.bootstrap.delayed_practice.status = 'in_progress';
ui.renderDelayedPractice();
assert.equal(get('start-delayed-practice').textContent, 'Продолжить проверку');
// A failed launch must preserve the existing session and permit a retry.
ui.state.session = {session_id: 17};
response = async () => ({ok: false, status: 400, json: async () => ({message: 'Предложение изменилось. Обнови экран.'})});
await get('start-delayed-practice').events.click();
assert.equal(requests[0].path, '/api/v1/sessions');
assert.equal(requests[0].body.delayed_practice, 'raii_state_restore/v1');
assert.equal(ui.state.session.session_id, 17);
assert.match(get('delayed-practice-error').textContent, /Предложение изменилось/);
assert.equal(get('start-delayed-practice').disabled, false);
let release;
response = () => new Promise(resolve => {release = resolve;});
const pending = ui.startDelayedPractice();
await Promise.resolve();
await ui.startDelayedPractice();
assert.equal(requests.length, 2, 'double click created a second launch');
release({ok: false, status: 503, json: async () => ({message: 'Повтори позже'})});
await pending;
assert.equal(requests[1].body.request_id, requests[0].body.request_id, 'retry lost its idempotency key');
console.log('delayed practice UI model passed');
