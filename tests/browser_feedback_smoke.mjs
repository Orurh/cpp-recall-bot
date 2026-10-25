// Optional end-to-end check: Node 22+, Chromium and a disposable TEST_DATABASE_URL.
// It uses a test bot token, blocks outbound requests, and never reads .env.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHmac, randomInt } from 'node:crypto';
import { mkdtemp, writeFile } from 'node:fs/promises';
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
    window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(ownerId))},
      ready() {}, expand() {}, setHeaderColor() {}, setBackgroundColor() {}, enableClosingConfirmation() {},
      disableClosingConfirmation() {}, BackButton: { show() {}, hide() {}, onClick() {} },
      showConfirm(message, callback) { callback(true); }, HapticFeedback: { notificationOccurred() {} } } };
  ` });
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
  await send('Page.navigate', { url: origin });
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home rendering');
  assert.equal(await evaluate('document.getElementById("editorial-tab").hidden'), false);
  assert.equal(await evaluate('document.getElementById("stat-accuracy").textContent'), '—', 'empty accuracy was shown as a measured result');
  assert.equal(await evaluate('document.getElementById("stat-first-answer-label").textContent'), 'с первого ответа');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'training rendering');
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
  await evaluate('document.getElementById("editorial-tab").click()');
  await until(() => evaluate('document.querySelectorAll(".review-card").length > 0'), 'editorial queue');
  const reports = await evaluate(`Array.from(document.querySelectorAll('.review-comment'), el => el.textContent).filter(text => text === ${JSON.stringify(comment)}).length`);
  assert.equal(reports, 1, 'retry created another report');
  assert.equal(await evaluate('Boolean(window.xssExecuted)'), false, 'a report comment was interpreted as HTML');
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
  await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: preload.identifier });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(ownerId + 1))}, ready() {}, expand() {} } };` });
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'regular user home');
  assert.equal(await evaluate('document.getElementById("editorial-tab").hidden'), true);
  assert.equal(await evaluate('document.getElementById("statistics-tab").hidden'), true);
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
