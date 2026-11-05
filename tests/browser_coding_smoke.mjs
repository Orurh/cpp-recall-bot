// Standalone UI SIMULATION: fake API, synthetic identity, no database or external network.
// Node 22+ and Chromium. Run inside an isolated network namespace for strict egress isolation.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';

const artifacts = await mkdtemp(path.join(tmpdir(), 'cpp-recall-coding-ui-'));
const calls = [], errors = [];
const fixture = id => ({ user: { id, first_name: 'Проверка' }, feedback_admin: false,
  summary: { xp: { total: 1000, next_reward: null }, skills: [], topics: [], due_review: 0,
    skill_mastery: { demonstrated: 0, retained: 0 }, unlocked_level: 3, adaptive_level: 3 },
  skills: [], topics: [], modes: [], levels: [], learning_routes: [],
  rewards: { profile: { selected_frame: 'none' }, presets: [], preset_slots: 0, newly_unlocked: [] },
  session: { session_id: 88, phase: 'question' },
  saved_sessions: [{ id: 88, available: true, completed: 1, total: 5, topics: [], modes: [] }],
  coding_practice: { id: 'take_prefix', revision: 1, level: 3,
    available: id !== 103, reason: 'Сначала закрепи основы работы с вектором.' },
});
const server = createServer(async (request, response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname.startsWith('/api/v1/')) {
    const route = url.pathname.slice('/api/v1/'.length);
    const chunks = []; for await (const chunk of request) chunks.push(chunk);
    const body = JSON.parse(Buffer.concat(chunks).toString() || '{}');
    const id = Number(request.headers.authorization?.replace('tma fake-', ''));
    calls.push({ route, id, body });
    const data = route === 'bootstrap' ? fixture(id)
      : route === 'daily' ? { status: 'unavailable', timezone: 'UTC', local_day: '2026-11-05' } : {};
    response.writeHead(200, { 'Content-Type': 'application/json' }); response.end(JSON.stringify(data)); return;
  }
  const files = { '/': ['index.html', 'text/html'], '/app.js': ['app.js', 'text/javascript'],
    '/coding-practice.js': ['coding-practice.js', 'text/javascript'], '/style.css': ['style.css', 'text/css'] };
  const file = files[url.pathname];
  if (!file) { response.writeHead(404); response.end(); return; }
  response.writeHead(200, { 'Content-Type': file[1] }); response.end(await readFile(path.join('docs/miniapp', file[0])));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}/`;
let browser, ws;
const pending = new Map(); let sequence = 0;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const until = async (check, description) => {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) { try { if (await check()) return; } catch {} await pause(40); }
  throw new Error(`Timed out: ${description}`);
};
try {
  browser = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--no-sandbox', '--disable-dev-shm-usage',
    '--disable-background-networking', '--no-first-run', '--no-default-browser-check',
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost',
    '--remote-debugging-port=0', `--user-data-dir=${path.join(artifacts, 'chromium')}`, 'about:blank'], { stdio: 'ignore' });
  browser.on('error', error => errors.push(error.message));
  let debugPort;
  await until(async () => {
    debugPort = Number((await readFile(path.join(artifacts, 'chromium/DevToolsActivePort'), 'utf8')).split('\n')[0]);
    return debugPort > 0;
  }, 'Chromium');
  const target = await (await fetch(`http://127.0.0.1:${debugPort}/json/new?about:blank`, { method: 'PUT' })).json();
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.addEventListener('open', resolve, { once: true }); ws.addEventListener('error', reject, { once: true }); });
  ws.addEventListener('message', ({ data }) => {
    const message = JSON.parse(data);
    if (message.id) {
      const item = pending.get(message.id); if (!item) return;
      pending.delete(message.id); clearTimeout(item.timer);
      if (message.error) item.reject(new Error(JSON.stringify(message.error))); else item.resolve(message.result);
    } else if (message.method === 'Runtime.exceptionThrown') errors.push(message.params.exceptionDetails);
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`CDP timeout: ${method}`)); }, 10000);
    pending.set(id, { resolve, reject, timer }); ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async expression => {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable');
  await send('Network.setBlockedURLs', { urls: ['https://*', 'http://telegram.org/*'] });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `
    const params = new URL(location.href).searchParams;
    const user = Number(params.get('user') || 101);
    window.Telegram = { WebApp: { initData: 'fake-' + user, colorScheme: 'dark',
      ready() {}, expand() {}, setHeaderColor() {}, setBackgroundColor() {},
      onEvent(name, callback) { if (name === 'themeChanged') window.__codingTheme = callback; },
      enableClosingConfirmation() {}, disableClosingConfirmation() {},
      BackButton: { show() {}, hide() {}, onClick(callback) { window.__codingBack = callback; } } } };
    if (params.has('blocked')) {
      Storage.prototype.setItem = function() { throw new DOMException('Blocked', 'SecurityError'); };
    }
    if (params.has('readblocked')) {
      window.__realGetItem = Storage.prototype.getItem;
      Storage.prototype.getItem = function(key) {
        if (key.startsWith('cpp-recall-coding-v1:')) throw new DOMException('Read blocked', 'SecurityError');
        return window.__realGetItem.call(this, key);
      };
    }
    Object.defineProperty(navigator, 'clipboard', { configurable: true, value: {
      writeText: async () => { throw new DOMException('Denied', 'NotAllowedError'); } } });
  ` });
  const home = () => until(() => evaluate('!document.getElementById("home").hidden'), 'home');
  const open = async () => {
    await evaluate('document.getElementById("open-coding").click()');
    await until(() => evaluate('!document.getElementById("coding").hidden'), 'coding workspace');
  };
  const recoveryMeasurements = [];
  const checkRecovery = async state => {
    for (const width of [390, 768, 1440]) {
      await send('Emulation.setDeviceMetricsOverride', { width, height: 1000, deviceScaleFactor: 1, mobile: width < 600 });
      await evaluate('window.scrollTo(0,0)');
      const recovery = await evaluate(`(() => {
        const button = document.getElementById('coding-recover');
        const rect = button.getBoundingClientRect();
        return { visible: !button.hidden, height: rect.height, width: rect.width };
      })()`);
      assert(recovery.visible && recovery.height >= 44, `${state} recovery target ${width}: ${JSON.stringify(recovery)}`);
      assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `${state} overflow ${width}`);
      recoveryMeasurements.push({ state, viewportWidth: width, ...recovery });
      const shot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true,
        clip: { x: 0, y: 0, width, height: await evaluate('document.documentElement.scrollHeight'), scale: 1 } });
      await writeFile(path.join(artifacts, `coding-recovery-${state}-${width}.png`), Buffer.from(shot.data, 'base64'));
    }
  };
  const input = code => evaluate(`{
    const field = document.getElementById('coding-draft'); field.value = ${JSON.stringify(code)};
    field.dispatchEvent(new Event('input', { bubbles: true }));
  }`);
  const key = 'cpp-recall-coding-v1:101:take_prefix:1';
  await send('Page.navigate', { url: origin }); await home();
  assert.equal(await evaluate('document.getElementById("stat-xp").textContent'), '1000');
  await open();
  assert.equal(await evaluate('document.activeElement.id'), 'coding-title');
  assert.equal(await evaluate('document.getElementById("coding-help").hidden'), true);
  assert.equal(await evaluate('document.getElementById("coding-solution").hidden'), true);
  const malicious = '// <img src=x onerror="window.__executed=true">\nstd::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) { return {}; }';
  await input(malicious);
  await evaluate('document.getElementById("coding-show-help").click()');
  assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).helpOpened`), true);
  await send('Page.reload'); await home(); await open();
  assert.equal(await evaluate('document.getElementById("coding-draft").value'), malicious);
  assert.equal(await evaluate('document.getElementById("coding-help").hidden'), false);
  assert.equal(await evaluate('window.__executed === true'), false);
  assert.equal(await evaluate('document.getElementById("coding-solution").hidden'), true);
  await evaluate('document.getElementById("coding-copy").click()');
  await until(() => evaluate('document.getElementById("coding-export-status").textContent.includes("вручную")'), 'manual clipboard fallback');
  const exported = await evaluate('document.getElementById("coding-source").value');
  assert(exported.includes(malicious) && exported.includes('CheckCase('));
  assert.equal(await evaluate('document.getElementById("coding-source").readOnly'), true);
  await evaluate('document.getElementById("coding-show-solution").click()');
  await send('Page.reload'); await home(); await open();
  assert.equal(await evaluate('document.getElementById("coding-solution").hidden'), false);

  // Two independent same-origin pages: a stale buffer must not silently replace newer storage.
  const otherTarget = await (await fetch(`http://127.0.0.1:${debugPort}/json/new?${encodeURIComponent(origin)}`, { method: 'PUT' })).json();
  const other = new WebSocket(otherTarget.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { other.addEventListener('open', resolve, { once: true }); other.addEventListener('error', reject, { once: true }); });
  let otherSequence = 0;
  const otherEvaluate = expression => new Promise((resolve, reject) => {
    const id = ++otherSequence;
    const timer = setTimeout(() => reject(new Error('Second page timeout')), 10000);
    const listener = ({ data }) => {
      const message = JSON.parse(data); if (message.id !== id) return;
      clearTimeout(timer); other.removeEventListener('message', listener);
      if (message.error || message.result?.exceptionDetails) reject(new Error(JSON.stringify(message)));
      else resolve(message.result.result.value);
    };
    other.addEventListener('message', listener);
    other.send(JSON.stringify({ id, method: 'Runtime.evaluate', params: { expression, returnByValue: true } }));
  });
  try {
    await until(() => otherEvaluate('location.origin === ' + JSON.stringify(new URL(origin).origin)), 'second page origin');
    await otherEvaluate(`{
      const draft = JSON.parse(localStorage.getItem('${key}')); draft.code = '// newer other window';
      localStorage.setItem('${key}', JSON.stringify(draft));
    }`);
    await until(() => evaluate('document.getElementById("coding-storage").textContent.includes("другом окне")'), 'storage conflict shown');
    await checkRecovery('conflict');
    await input('// stale local edit');
    assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).code`), '// newer other window');
    await evaluate('window.__codingBack()'); await home();
    assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).code`), '// newer other window');
    await open();
    assert.equal(await evaluate('document.getElementById("coding-draft").value'), '// stale local edit');
    await evaluate('document.getElementById("coding-recover").click()');
    assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).code`), '// stale local edit');
    await input(malicious);
  } finally { other.close(); await fetch(`http://127.0.0.1:${debugPort}/json/close/${otherTarget.id}`); }

  for (const theme of ['dark', 'light']) {
    await evaluate(`{
      const colors = ${JSON.stringify({ dark: { bg: '#17212b', surface: '#232e3c', text: '#f5f5f5', link: '#73a9ff' },
        light: { bg: '#ffffff', surface: '#f0f2f5', text: '#1f2937', link: '#245ea8' } })}['${theme}'];
      for (const [name, value] of Object.entries({ 'bg-color': colors.bg, 'secondary-bg-color': colors.surface,
        'text-color': colors.text, 'link-color': colors.link })) document.documentElement.style.setProperty('--tg-theme-' + name, value);
      Telegram.WebApp.colorScheme = '${theme}'; window.__codingTheme();
    }`);
    for (const width of [390, 768, 1440]) {
      await send('Emulation.setDeviceMetricsOverride', { width, height: 1000, deviceScaleFactor: 1, mobile: width < 600 });
      await evaluate('window.scrollTo(0,0)');
      assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `overflow ${theme}/${width}`);
      assert.equal(await evaluate('document.getElementById("coding-download").getBoundingClientRect().height >= 44'), true);
      const screenshot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true,
        clip: { x: 0, y: 0, width, height: await evaluate('document.documentElement.scrollHeight'), scale: 1 } });
      await writeFile(path.join(artifacts, `coding-${theme}-${width}.png`), Buffer.from(screenshot.data, 'base64'));
    }
  }
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: 'reduce' }] });
  assert.equal(await evaluate('matchMedia("(prefers-reduced-motion: reduce)").matches'), true);
  await input('x'.repeat(16385));
  assert.equal(await evaluate('document.getElementById("coding-download").disabled'), true);
  assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).code`), malicious);
  await input(malicious);
  await evaluate('window.__codingBack()'); await home();
  assert.equal(await evaluate('document.activeElement.id'), 'open-coding');
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Продолжить тренировку');
  assert.equal(await evaluate('document.getElementById("saved-training-count").textContent'), '1');
  assert.equal(await evaluate('document.getElementById("stat-xp").textContent'), '1000');
  await send('Page.navigate', { url: `${origin}?user=102` }); await home(); await open();
  assert.notEqual(await evaluate('document.getElementById("coding-draft").value'), malicious);
  assert.equal(await evaluate('document.getElementById("coding-help").hidden'), true);
  await send('Page.navigate', { url: `${origin}?user=103` }); await home();
  assert.equal(await evaluate('document.getElementById("open-coding").disabled'), true);
  await evaluate(`localStorage.setItem('${key}', '{broken')`);
  await send('Page.navigate', { url: origin }); await home(); await open();
  await input('// replacement');
  assert.equal(await evaluate(`localStorage.getItem('${key}')`), '{broken');
  assert.equal(await evaluate('document.getElementById("coding-recover").hidden'), false);
  await checkRecovery('corrupt');
  await evaluate('document.getElementById("coding-recover").click()');
  assert.equal(await evaluate(`JSON.parse(localStorage.getItem('${key}')).code`), '// replacement');
  const mismatched = { version: 1, userId: 101, exerciseId: 'take_prefix', revision: 2,
    code: '// old revision', helpOpened: false, solutionOpened: false };
  await evaluate(`localStorage.setItem('${key}', ${JSON.stringify(JSON.stringify(mismatched))})`);
  await send('Page.reload'); await home(); await open();
  assert.equal(await evaluate('document.getElementById("coding-recover").hidden'), false);
  assert.notEqual(await evaluate('document.getElementById("coding-draft").value'), '// old revision');
  await send('Page.navigate', { url: `${origin}?blocked=1&user=104` }); await home(); await open();
  await input('// unsaved');
  assert.equal(await evaluate('document.getElementById("coding-storage").textContent.includes("Не удалось сохранить")'), true);
  assert.equal(await evaluate('document.getElementById("coding-draft").value'), '// unsaved');
  const readKey = 'cpp-recall-coding-v1:106:take_prefix:1';
  const protectedDraft = JSON.stringify({ version: 1, userId: 106, exerciseId: 'take_prefix', revision: 1,
    code: '// existing read-protected', helpOpened: false, solutionOpened: false });
  // A navigation restores native storage methods before the read-only failure scenario.
  await send('Page.navigate', { url: `${origin}?user=106` }); await home();
  await evaluate(`localStorage.setItem('${readKey}', ${JSON.stringify(protectedDraft)})`);
  await send('Page.navigate', { url: `${origin}?user=106&readblocked=1` }); await home(); await open();
  await input('// must not replace unread version');
  assert.equal(await evaluate(`window.__realGetItem.call(localStorage, '${readKey}')`), protectedDraft);
  assert.equal(await evaluate('document.getElementById("coding-storage").textContent.includes("Не удалось сохранить")'), true);
  await send('Page.navigate', { url: `${origin}?user=107` }); await home(); await open();
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 1000, deviceScaleFactor: 1, mobile: true });
  assert.equal(await evaluate('document.getElementById("coding-full-contract").textContent === CppRecallCodingPractice.prompt'), true);
  assert.equal(await evaluate('document.getElementById("coding-description").textContent === CppRecallCodingPractice.summary'), true);
  const editorTop = await evaluate('document.getElementById("coding-draft").getBoundingClientRect().top');
  assert(editorTop < 600, `Initial 390 editor begins too low: ${editorTop}`);
  await writeFile(path.join(artifacts, 'coding-layout-measurements.json'), JSON.stringify({ editorTop390: editorTop, recoveryMeasurements }, null, 2));
  const takeShot = async name => {
    await evaluate('window.scrollTo(0,0)');
    const shot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true,
      clip: { x: 0, y: 0, width: 390, height: await evaluate('document.documentElement.scrollHeight'), scale: 1 } });
    await writeFile(path.join(artifacts, name), Buffer.from(shot.data, 'base64'));
  };
  await takeShot('coding-initial-390.png');
  await evaluate('URL.createObjectURL = () => { throw new Error("download blocked"); }; document.getElementById("coding-download").click()');
  assert.equal(await evaluate('document.getElementById("coding-export-status").textContent.includes("Не удалось начать загрузку")'), true);
  assert.equal(await evaluate('document.getElementById("coding-source-panel").hidden'), false);
  await takeShot('coding-export-fallback-390.png');
  assert(calls.some(call => call.route === 'activity' && call.body.context === 'coding'));
  assert.deepEqual(calls.filter(call => !['bootstrap', 'daily', 'activity'].includes(call.route)), [], 'coding emitted mutation API calls');
  assert.deepEqual(errors, [], 'browser runtime errors');
  console.log(`PASS coding UI SIMULATION: draft/reload, identity/revision, assistance, recovery, blocked storage, export fallback, six viewports, reduced motion, no training writes; screenshots: ${artifacts}`);
} finally {
  ws?.close(); browser?.kill('SIGTERM'); await new Promise(resolve => server.close(resolve));
}
