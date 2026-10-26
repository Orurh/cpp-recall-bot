// Optional end-to-end check: Node 22+, Chromium and a disposable TEST_DATABASE_URL.
// It uses a test bot token, blocks outbound requests, and never reads .env.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHmac, randomInt } from 'node:crypto';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';

assert(process.env.TEST_DATABASE_URL, 'Set TEST_DATABASE_URL to a disposable PostgreSQL database.');
const artifacts = await mkdtemp(path.join(tmpdir(), 'cpp-recall-feedback-ui-'));
const token = '000000000:local-feedback-ui-test-token';
const ownerId = randomInt(800000000, 900000000);
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function freePort() {
  const server = createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

function signedInitData(id) {
  const fields = { auth_date: String(Math.floor(Date.now() / 1000)), user: JSON.stringify({ id, first_name: 'Проверка' }) };
  const check = Object.entries(fields).sort(([a], [b]) => a.localeCompare(b)).map(([key, value]) => `${key}=${value}`).join('\n');
  const secret = createHmac('sha256', 'WebAppData').update(token).digest();
  const hash = createHmac('sha256', secret).update(check).digest('hex');
  return new URLSearchParams({ ...fields, hash }).toString();
}

async function until(check, description) {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try { if (await check()) return; } catch { /* wait for startup or rendering */ }
    await pause(80);
  }
  throw new Error(`Timed out: ${description}`);
}

const appPort = await freePort();
const debugPort = await freePort();
const origin = `http://127.0.0.1:${appPort}/`;
let appLog = '';
const app = spawn('./build/cc_recall_bot', [], { env: { ...process.env,
  TELEGRAM_BOT_TOKEN: token, DATABASE_URL: process.env.TEST_DATABASE_URL,
  MINI_APP_URL: 'https://localhost/', MINI_APP_LISTEN_ADDRESS: '127.0.0.1', MINI_APP_PORT: String(appPort),
  MINI_APP_FEEDBACK_ADMIN_ID: String(ownerId), MINI_APP_STATIC_DIR: 'docs/miniapp',
  HTTPS_PROXY: 'http://127.0.0.1:9', https_proxy: 'http://127.0.0.1:9',
  HTTP_PROXY: 'http://127.0.0.1:9', http_proxy: 'http://127.0.0.1:9',
  ALL_PROXY: 'http://127.0.0.1:9', NO_PROXY: '127.0.0.1,localhost', no_proxy: '127.0.0.1,localhost',
}, stdio: ['ignore', 'pipe', 'pipe'] });
app.stdout.on('data', (data) => { appLog = (appLog + data).slice(-8000); });
app.stderr.on('data', (data) => { appLog = (appLog + data).slice(-8000); });
app.on('error', (error) => { appLog += error.message; });
let browser;
let ws;
let sequence = 0;
const pending = new Map();
const errors = [];

try {
  await until(async () => (await fetch(origin)).ok, 'local Mini App startup');
  assert.deepEqual(await (await fetch(new URL('health/live', origin))).json(), { alive: true });
  assert.deepEqual(await (await fetch(new URL('health/ready', origin))).json(), { ready: true });
  browser = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--no-sandbox', '--disable-dev-shm-usage',
    '--disable-background-networking', '--no-first-run', '--no-default-browser-check',
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost',
    `--remote-debugging-port=${debugPort}`, `--user-data-dir=${path.join(artifacts, 'chromium')}`, 'about:blank'],
  { stdio: 'ignore' });
  browser.on('error', (error) => errors.push(error.message));
  const debuggerOrigin = `http://127.0.0.1:${debugPort}`;
  await until(async () => (await fetch(`${debuggerOrigin}/json/version`)).ok, 'headless Chromium startup');
  const target = await (await fetch(`${debuggerOrigin}/json/new?about:blank`, { method: 'PUT' })).json();
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.addEventListener('open', resolve, { once: true }); ws.addEventListener('error', reject, { once: true }); });
  ws.addEventListener('message', ({ data }) => {
    const message = JSON.parse(data);
    if (message.id) {
      const call = pending.get(message.id);
      if (!call) return;
      pending.delete(message.id);
      clearTimeout(call.timer);
      if (message.error) call.reject(new Error(JSON.stringify(message.error)));
      else call.resolve(message.result);
    } else if (message.method === 'Runtime.exceptionThrown') {
      errors.push(JSON.stringify(message.params.exceptionDetails));
    }
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`CDP timeout: ${method}`)); }, 15000);
    pending.set(id, { resolve, reject, timer });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async (expression) => {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Network.enable');
  await send('Network.setBlockedURLs', { urls: ['https://telegram.org/*'] });
  const preload = await send('Page.addScriptToEvaluateOnNewDocument', { source: `
    window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(ownerId))}, colorScheme: 'dark',
      onEvent(name, callback) { if (name === 'themeChanged') window.emitTestThemeChanged = callback; },
      ready() {}, expand() {}, setHeaderColor() {}, setBackgroundColor() {}, enableClosingConfirmation() {},
      disableClosingConfirmation() {}, BackButton: { show() {}, hide() {}, onClick() {} },
      showConfirm(message, callback) { callback(true); }, HapticFeedback: { notificationOccurred() {} } } };
  ` });
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
  await send('Page.navigate', { url: origin });
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home rendering');
  // Telegram and OS themes may differ. Native selects and their popup options
  // must keep an opaque, contrasting palette in both cases, including on mobile.
  const luminance = color => {
    const channels = color.match(/[\d.]+/g).slice(0, 3).map(Number).map(value => {
      const channel = value / 255;
      return channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4;
    });
    return channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
  };
  for (const systemTheme of ['light', 'dark']) {
    await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: systemTheme }] });
    for (const theme of ['dark', 'light']) {
      await evaluate(`{
        const root = document.documentElement;
        root.style.setProperty('--tg-theme-bg-color', '${theme === 'dark' ? '#17212b' : '#ffffff'}');
        root.style.setProperty('--tg-theme-secondary-bg-color', '${theme === 'dark' ? '#232e3c' : '#f0f2f5'}');
        root.style.setProperty('--tg-theme-text-color', '${theme === 'dark' ? '#f5f5f5' : '#1f2937'}');
        Telegram.WebApp.colorScheme = '${theme}';
        window.emitTestThemeChanged();
      }`);
      for (const width of [390, 1280]) {
        await send('Emulation.setDeviceMetricsOverride', { width, height: 844, deviceScaleFactor: 1, mobile: width < 600 });
        const controls = await evaluate(`Array.from(document.querySelectorAll('select, select option')).map(el => {
          const style = getComputedStyle(el);
          return { id: el.id || el.textContent, color: style.color, background: style.backgroundColor };
        })`);
        assert(controls.length >= 4, 'select contrast fixtures missing');
        for (const control of controls) {
          assert.match(control.background, /^rgb\(/, `select background must be opaque: ${control.id}`);
          const a = luminance(control.color), b = luminance(control.background);
          const contrast = (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
          assert(contrast >= 4.5, `${theme}/${systemTheme}/${width}: unreadable ${control.id}, contrast ${contrast}`);
        }
        assert.equal(await evaluate('getComputedStyle(document.documentElement).colorScheme'), theme);
      }
      await evaluate('document.getElementById("learning-route").scrollIntoView({block: "center"})');
      const screenshot = await send('Page.captureScreenshot', { format: 'png' });
      await writeFile(path.join(artifacts, `select-${theme}-os-${systemTheme}.png`), Buffer.from(screenshot.data, 'base64'));
    }
  }
  await evaluate(`['--tg-theme-bg-color', '--tg-theme-secondary-bg-color', '--tg-theme-text-color'].forEach(
    name => document.documentElement.style.removeProperty(name));
    Telegram.WebApp.colorScheme = 'dark'; window.emitTestThemeChanged();
    document.getElementById('learning-route-select').focus();`);
  for (const [key, code, expected] of [['ArrowDown', 40, 'map_lookup'], ['ArrowUp', 38, 'containers_iterators']]) {
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key, code: key, windowsVirtualKeyCode: code });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key, code: key, windowsVirtualKeyCode: code });
    assert.equal(await evaluate('document.getElementById("learning-route-select").value'), expected,
      'select keyboard navigation stopped working');
  }
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
  assert.equal(await evaluate('document.getElementById("editorial-tab").hidden'), false);
  assert.equal(await evaluate('document.getElementById("stat-accuracy").textContent'), '—', 'empty accuracy was shown as a measured result');
  assert.equal(await evaluate('document.getElementById("stat-first-answer-label").textContent'), 'с первого ответа');
  assert.doesNotMatch(await evaluate('document.getElementById("today-copy").textContent'), /семейств|по сроку/);
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Тренировать');
  assert.equal(await evaluate('document.getElementById("today-copy").hidden'), true);
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'training rendering');
  assert.equal(await evaluate('document.getElementById("training-plan-copy").hidden'), true);
  assert.doesNotMatch(await evaluate('document.getElementById("training-plan-copy").textContent'), /семейств|по сроку/);
  assert.match(await evaluate('document.getElementById("question-public-id").textContent'), /^CR-\d{6}$/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'training overflows a mobile viewport');
  const trainingScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'training-mobile.png'), Buffer.from(trainingScreenshot.data, 'base64'));
  await evaluate('document.getElementById("report-question").click()');
  assert.equal(await evaluate('document.getElementById("card-report-dialog").open'), true);
  assert.equal(await evaluate('document.activeElement.id'), 'report-comment');
  assert.equal(await evaluate('document.getElementById("card-report-dialog").getBoundingClientRect().right <= window.innerWidth'), true);
  const comment = '<img src=x onerror="window.xssExecuted=true"> Неясная формулировка — браузерная проверка.';
  // Simulate a response lost after a successful server-side insert.
  await evaluate(`
    document.getElementById('report-comment').value = ${JSON.stringify(comment)};
    const originalFetch = window.fetch;
    let loseFirstReceipt = true;
    window.fetch = async (...args) => {
      const response = await originalFetch(...args);
      if (String(args[0]).endsWith('/card-feedback') && loseFirstReceipt) {
        loseFirstReceipt = false;
        throw new Error('Имитируем потерю ответа после сохранения');
      }
      return response;
    };
    document.getElementById('card-report-form').requestSubmit();
  `);
  await until(() => evaluate('Boolean(document.getElementById("report-error").textContent)'), 'simulated retry error');
  assert.equal(await evaluate('document.getElementById("report-comment").value'), comment);
  await evaluate('document.getElementById("card-report-form").requestSubmit()');
  await until(() => evaluate('!document.getElementById("report-success").hidden'), 'feedback receipt');
  const receiptText = await evaluate('document.getElementById("report-success").textContent');
  assert.match(receiptText, /Отзыв №\d+ принят/);
  const screenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'feedback-mobile.png'), Buffer.from(screenshot.data, 'base64'));
  await evaluate('document.getElementById("close-report").click(); document.getElementById("stop-session").click()');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after stop');
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Продолжить тренировку');
  await evaluate('const topic = document.querySelector("input[name=topic]"); topic.checked = true; topic.dispatchEvent(new Event("change", { bubbles: true }))');
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Тренировать', 'changed topic still resumed the previous training');
  await evaluate('document.getElementById("editorial-tab").click()');
  await until(() => evaluate('document.querySelectorAll(".review-card").length > 0'), 'editorial queue');
  const reports = await evaluate(`Array.from(document.querySelectorAll('.review-comment'), el => el.textContent).filter(text => text === ${JSON.stringify(comment)}).length`);
  assert.equal(reports, 1, 'retry created another report');
  assert.equal(await evaluate('Boolean(window.xssExecuted)'), false, 'a report comment was interpreted as HTML');
  assert.match(await evaluate('document.querySelector(".review-reference-history").textContent'), /В старых версиях/,
    'an empty reference history was misrepresented as proof of no help');
  const firstCard = '.review-card';
  await evaluate(`document.querySelector('${firstCard} select').value = 'resolved'; document.querySelector('${firstCard} form').requestSubmit()`);
  await until(() => evaluate('Boolean(document.querySelector(".review-card .form-error").textContent)'), 'review validation');
  assert.match(await evaluate('document.querySelector(".review-card .form-error").textContent'), /решение.*ссылк/);
  await evaluate(`
    document.querySelector('${firstCard} select').value = 'in_review';
    document.querySelector('${firstCard} textarea').value = 'Взято на техническую проверку.';
    document.querySelector('${firstCard} form').requestSubmit();
  `);
  await until(() => evaluate('document.querySelector(".review-card .form-error").textContent === "Решение сохранено."'), 'review save');
  assert.match(await evaluate('document.querySelector(".review-card > .muted").textContent'), /Проверяется/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'editorial queue overflows a mobile viewport');
  const queueScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'editorial-mobile.png'), Buffer.from(queueScreenshot.data, 'base64'));
  await evaluate('document.getElementById("statistics-tab").click()');
  await until(() => evaluate('document.getElementById("statistics-content").textContent.includes("Состояние процесса")'), 'statistics rendering');
  assert.equal(await evaluate('document.getElementById("statistics-error").textContent'), '');
  assert.match(await evaluate('document.getElementById("statistics-content").textContent'), /Отказов из-за перегрузки/);
  const statisticsText = await evaluate('document.getElementById("statistics-content").textContent');
  assert.match(statisticsText, /Первый учебный опыт/);
  assert.match(statisticsText, /Первую тренировку полностью завершили/);
  assert.match(statisticsText, /Открытие приложения на следующий день/);
  assert.match(statisticsText, /Возвраты к занятиям/);
  assert.match(statisticsText, /На 7–9-й день после первого ответа/);
  assert.match(statisticsText, /число приглашённых не учитывается/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'statistics overflows a mobile viewport');
  const statisticsScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'statistics-mobile.png'), Buffer.from(statisticsScreenshot.data, 'base64'));
  await evaluate('document.getElementById("statistics-period").value = "7"; document.getElementById("statistics-period").dispatchEvent(new Event("change"))');
  await until(() => evaluate('!document.getElementById("statistics-period").disabled'), 'learning report period change');
  assert.equal(await evaluate('document.getElementById("statistics-error").textContent'), '');
  const weekly = await evaluate(`Array.from(document.querySelectorAll('#statistics-content dt')).find(el => el.textContent === 'На 7–9-й день после первого ответа').nextElementSibling.textContent`);
  assert.match(weekly, /ещё нет зрелой группы|нет учебных ответов/);
  assert(!weekly.includes('0%'), 'an immature weekly cohort was shown as measured zero retention');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'learning report overflows a mobile viewport');
  const deniedStatistics = await fetch(new URL('api/v1/admin/statistics', origin), {
    method: 'POST', headers: { Authorization: `tma ${signedInitData(ownerId + 1)}`, 'Content-Type': 'application/json' }, body: '{"days":30}',
  });
  assert.equal(deniedStatistics.status, 403, 'regular user accessed private statistics');
  const denied = await fetch(new URL('api/v1/admin/card-feedback', origin), {
    method: 'POST', headers: { Authorization: `tma ${signedInitData(ownerId + 1)}`, 'Content-Type': 'application/json' }, body: '{"status":"new"}',
  });
  assert.equal(denied.status, 403, 'regular user accessed the administrative HTTP endpoint');
  const unauthenticated = await fetch(new URL('api/v1/admin/card-feedback', origin), { method: 'POST' });
  assert.equal(unauthenticated.status, 401);
  const ownerApi = async (route, body = {}) => {
    const response = await fetch(new URL(`api/v1/${route}`, origin), {
      method: 'POST', headers: { Authorization: `tma ${signedInitData(ownerId)}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const value = await response.json();
    assert.equal(response.status, 200, JSON.stringify(value));
    return value;
  };
  // Use the real bank and its local answer fixtures to reach a linked reference.
  // This is a scripted scenario, not evidence from a learner.
  let practice = await ownerApi('sessions', { modes: ['code'], topics: ['containers'], level: 3 });
  for (let i = 0; !practice.question?.references?.length && i < 10; ++i) {
    const card = JSON.parse(await readFile(`content/questions/containers/${practice.question.id}.json`, 'utf8'));
    assert.equal(card.task.type, 'single_choice');
    await ownerApi('session/answer', { question_id: card.id, answer: card.task.correct_option_id, session_id: practice.session_id });
    practice = await ownerApi('session/next', { session_id: practice.session_id });
  }
  assert(practice.question?.references?.length, 'fixture did not reach a reference-enabled card');
  const helpedSession = practice.session_id;
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before help');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'resuming reference card');
  const selected = await evaluate('document.querySelector("#answer-area input").value');
  await evaluate(`{
    const originalFetch = window.fetch;
    let loseReferenceReceipt = true;
    window.fetch = async (...args) => {
      const response = await originalFetch(...args);
      if (String(args[0]).endsWith('/session/reference') && loseReferenceReceipt && response.ok) {
        loseReferenceReceipt = false;
        throw new Error('Имитируем потерю ответа справки после сохранения');
      }
      return response;
    };
  }`);
  await evaluate('document.querySelector("#answer-area input").checked = true; document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('document.getElementById("answer-error").textContent.includes("потерю ответа справки")'),
    'lost reference response');
  assert.equal(await evaluate('document.getElementById("theory-dialog").hidden'), true, 'failed help disclosed article text');
  await evaluate('document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'reference rendering');
  assert.equal(await evaluate('document.getElementById("answer-error").textContent'), '', 'successful help retry retained an old error');
  assert.equal(await evaluate('document.getElementById("help-kind").textContent'), 'СПРАВКА · C++23');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'reference overflows mobile viewport');
  const referenceScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'reference-mobile.png'), Buffer.from(referenceScreenshot.data, 'base64'));
  await evaluate('document.getElementById("close-theory").click()');
  assert.equal(await evaluate('document.querySelector("#answer-area input:checked").value'), selected, 'help erased the chosen answer');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
  const referenceReport = await ownerApi('card-feedback', {
    request_id: '90000000-0000-4000-8000-000000000099', question_id: practice.question.id,
    content_revision: practice.question.content_revision, phase: 'question', category: 'unclear_wording',
    comment: 'Проверка сохранённой справки в очереди отзывов.',
  });
  const referenceQueue = await ownerApi('admin/card-feedback', { status: 'all' });
  const recordedHelp = referenceQueue.items.find(item => item.feedback_id === referenceReport.feedback_id).references;
  assert.equal(recordedHelp.length, 1, 'feedback omitted the opened reference');
  assert.equal(recordedHelp[0].reference_id, practice.question.references[0].id);
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before reference history');
  await evaluate('document.getElementById("editorial-tab").click()');
  await until(() => evaluate('document.querySelector(".review-reference-history summary")?.textContent.endsWith(": 1")'),
    'archived reference in feedback queue');
  await evaluate('document.querySelector(".review-reference-history").open = true');
  assert.equal(await evaluate('document.querySelector(".review-reference-history h3").textContent'), recordedHelp[0].content.title);
  assert.match(await evaluate('document.querySelector(".review-reference-history").textContent'),
    new RegExp(recordedHelp[0].reference_content_revision.slice(0, 12)));
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'archived reference history overflows the mobile viewport');
  const provenanceScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'reference-history-mobile.png'), Buffer.from(provenanceScreenshot.data, 'base64'));
  await ownerApi('sessions', { modes: ['code'], topics: ['containers'], level: 3 });
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("saved-trainings").hidden'), 'saved trainings');
  await evaluate(`document.querySelector('#saved-training-list button[data-session-id="${helpedSession}"]').click()`);
  await until(() => evaluate('!document.getElementById("training").hidden'), 'restoring a saved training');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false, 'switching trainings erased assistance');
  // A newly introduced mechanism must have help before any answer, not only after an error.
  let template = await ownerApi('sessions', { modes: ['code'], topics: ['templates'], level: 3 });
  for (let i = 0; !template.question?.id.startsWith('parameter_pack_') && i < 10; ++i) {
    assert(template.question, 'template session ended before reaching introductory practice');
    const card = JSON.parse(await readFile(`content/questions/templates/${template.question.id}.json`, 'utf8'));
    assert.equal(card.task.type, 'single_choice');
    await ownerApi('session/answer', { question_id: card.id, answer: card.task.correct_option_id, session_id: template.session_id });
    template = await ownerApi('session/next', { session_id: template.session_id });
  }
  assert(template.question?.id.startsWith('parameter_pack_'), 'fixture did not reach a pack introduction');
  assert(template.question.references.some(entry => entry.id === 'parameter_packs'));
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before template reference');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'template practice');
  assert.equal(await evaluate('document.querySelector("#reference-help summary").textContent'), 'Справка до ответа');
  assert.equal(await evaluate('Boolean(document.getElementById("reference-help").compareDocumentPosition(document.getElementById("question-code")) & Node.DOCUMENT_POSITION_FOLLOWING)'), true);
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'pack reference before answering');
  assert.equal(await evaluate('document.getElementById("help-kind").textContent'), 'СПРАВКА · C++23');
  const templateScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'template-reference-mobile.png'), Buffer.from(templateScreenshot.data, 'base64'));
  await evaluate('document.getElementById("close-theory").click()');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
  let lifetime = await ownerApi('sessions', { modes: ['code'], topics: ['lifetime'], level: 2 });
  const lifetimeTargets = new Set(['pointer_after_local_lifetime', 'c_str_from_temporary_dangles', 'unique_ptr_move_transfers_ownership']);
  for (let i = 0; !lifetimeTargets.has(lifetime.question?.id) && i < 5; ++i) {
    assert(lifetime.question, 'lifetime practice ended before a selected reference card');
    const card = JSON.parse(await readFile(`content/questions/lifetime/${lifetime.question.id}.json`, 'utf8'));
    await ownerApi('session/answer', { question_id: card.id, answer: card.task.correct_option_id, session_id: lifetime.session_id });
    lifetime = await ownerApi('session/next', { session_id: lifetime.session_id });
  }
  assert(lifetimeTargets.has(lifetime.question?.id));
  assert(lifetime.question.references.length > 0, 'lifetime reference missing before any answer');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before lifetime reference');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'lifetime practice');
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'lifetime reference before answering');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true);
  const lifetimeScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'lifetime-reference-mobile.png'), Buffer.from(lifetimeScreenshot.data, 'base64'));
  await evaluate('document.getElementById("close-theory").click()');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
  let container = await ownerApi('sessions', { modes: ['code'], topics: ['containers'], level: 3 });
  for (let i = 0; container.question?.id !== 'vector_erase_return_next' && i < 10; ++i) {
    assert(container.question, 'container session ended before reaching erase practice');
    const card = JSON.parse(await readFile(`content/questions/containers/${container.question.id}.json`, 'utf8'));
    assert.equal(card.task.type, 'single_choice');
    await ownerApi('session/answer', { question_id: card.id, answer: card.task.correct_option_id, session_id: container.session_id });
    container = await ownerApi('session/next', { session_id: container.session_id });
  }
  assert.equal(container.question?.id, 'vector_erase_return_next');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before vector reference');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'erase practice');
  await evaluate(`document.getElementById('reference-help').open = true;
    Array.from(document.querySelectorAll('#reference-links button')).find(button => button.textContent.includes('что происходит с итераторами')).click()`);
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'vector iterator reference');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'vector reference overflows mobile viewport');
  const containerScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'vector-reference-mobile.png'), Buffer.from(containerScreenshot.data, 'base64'));
  await evaluate('document.getElementById("close-theory").click()');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
  assert.equal((await fetch(new URL('pilot/control_tasks.json', origin))).status, 404, 'pilot controls are publicly served');
  await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: preload.identifier });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(ownerId + 1))}, ready() {}, expand() {} } };` });
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'regular user home');
  assert.equal(await evaluate('document.getElementById("editorial-tab").hidden'), true);
  assert.equal(await evaluate('document.getElementById("statistics-tab").hidden'), true);
  assert.equal(await evaluate('document.getElementById("learning-route").hidden'), false);
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-steps li").length'), 5);
  await evaluate('document.getElementById("learning-route").scrollIntoView({block: "start"})');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'learning route overflows mobile viewport');
  const routeScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'learning-route-mobile.png'), Buffer.from(routeScreenshot.data, 'base64'));
  await evaluate('document.getElementById("start-learning-route").click(); document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'route starts from home');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000155');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after route reload');
  assert.equal(await evaluate('document.getElementById("start-learning-route").textContent'), 'Продолжить начатый шаг');
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'route resumes after reload');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000155');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before skill composition');
  assert.match(await evaluate('document.getElementById("skill-map-total").textContent'), /из 20/,
    'adding a route silently expanded the mastery denominator');
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-select option").length'), 2);
  assert.equal(await evaluate('document.querySelectorAll(".skill-composition").length'), 6);
  await evaluate(`const library = Array.from(document.querySelectorAll('.skill-legend-row')).find(row => row.querySelector('strong').textContent === 'Библиотека');
    library.querySelector('details').open = true; library.scrollIntoView({block: 'start'});`);
  assert.match(await evaluate(`Array.from(document.querySelectorAll('.skill-composition')).map(el => el.textContent).join(' ')`), /пока вне оценки/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'skill composition overflows mobile viewport');
  const compositionScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'skill-composition-mobile.png'), Buffer.from(compositionScreenshot.data, 'base64'));
  await evaluate(`Array.from(document.querySelectorAll('.skill-composition button')).find(button => button.textContent.includes('Словарь')).click()`);
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'map_lookup');
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-steps li").length'), 4);
  assert.match(await evaluate('document.getElementById("learning-route-methods").textContent'), /try_emplace.*insert_or_assign/);
  await evaluate('document.getElementById("learning-route").scrollIntoView({block: "start"})');
  const mapScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'map-route-mobile.png'), Buffer.from(mapScreenshot.data, 'base64'));
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'map route starts from skill composition');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000178');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after map route reload');
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'map_lookup');
  await evaluate(`document.getElementById('learning-route-select').value = 'containers_iterators';
    document.getElementById('learning-route-select').dispatchEvent(new Event('change'));`);
  assert.equal(await evaluate('document.getElementById("start-learning-route").textContent'), 'Продолжить начатый шаг');
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'first route resumes after second starts');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000155');
  assert.deepEqual(errors, [], 'browser script errors');
  console.log(`Browser feedback check passed. Screenshots: ${artifacts}`);
} catch (error) {
  console.error(appLog);
  throw error;
} finally {
  ws?.close();
  browser?.kill('SIGTERM');
  app.kill('SIGTERM');
}
