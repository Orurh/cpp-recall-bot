// Optional end-to-end check: Node 22+, Chromium and a disposable TEST_DATABASE_URL.
// It uses a test bot token, blocks outbound requests, and never reads .env.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHmac, randomInt } from 'node:crypto';
import { mkdtemp, readFile, readdir, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { checkSharedChallenges } from './browser_shared_challenge_checks.mjs';

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

function signedInitData(id, startParam) {
  const fields = { auth_date: String(Math.floor(Date.now() / 1000)), user: JSON.stringify({ id, first_name: 'Проверка' }),
    ...(startParam ? { start_param: startParam } : {}) };
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
  MINI_APP_BOT_USERNAME: 'cpp_recall_bot',
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
      for (const width of [320, 390, 768, 1280]) {
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
        assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
          `home overflows ${theme}/${systemTheme}/${width}`);
        const homeLayout = await evaluate(`(() => {
          const primary = document.getElementById('quick-start').getBoundingClientRect();
          const secondary = document.getElementById('open-settings').getBoundingClientRect();
          const route = document.getElementById('learning-route').getBoundingClientRect();
          const practice = document.querySelector('.next-action').getBoundingClientRect();
          return { primaryHeight: primary.height, secondaryHeight: secondary.height,
            primaryBottom: primary.bottom + scrollY, practiceBottom: practice.bottom + scrollY,
            routeTop: route.top + scrollY, headings: document.querySelectorAll('#home h1').length };
        })()`);
        assert(homeLayout.primaryHeight >= 44 && homeLayout.secondaryHeight >= 44, 'home actions are too small');
        assert(homeLayout.primaryBottom < 560, 'primary action is buried below the first mobile screen');
        assert(homeLayout.routeTop >= homeLayout.practiceBottom, 'optional route competes with the primary action');
        assert.equal(homeLayout.headings, 1);
        const homeColors = await evaluate(`(() => {
          const context = document.createElement('canvas').getContext('2d');
          const rgb = color => {
            context.clearRect(0, 0, 1, 1);
            context.fillStyle = color;
            context.fillRect(0, 0, 1, 1);
            return 'rgb(' + Array.from(context.getImageData(0, 0, 1, 1).data).slice(0, 3).join(',') + ')';
          };
          return [['#quick-start', '#quick-start'], ['#open-settings', '.next-action'], ['.stats span', '.stats'],
            ['.learning-route .section-label', '.learning-route']].map(([text, surface]) => ({
              text, color: rgb(getComputedStyle(document.querySelector(text)).color),
              background: rgb(getComputedStyle(document.querySelector(surface)).backgroundColor)
            })).concat(['#263650', '#101827'].map(background => ({
              text: 'profile initials', color: rgb(getComputedStyle(document.getElementById('open-profile')).color), background: rgb(background)
            })));
        })()`);
        for (const pair of homeColors) {
          const a = luminance(pair.color), b = luminance(pair.background);
          assert((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) >= 4.5,
            `unreadable home label: ${pair.text}, ${theme}/${width}`);
        }
        assert(await evaluate(`['.stats span', '.route-step .section-label'].every(selector =>
          parseFloat(getComputedStyle(document.querySelector(selector)).fontSize) >= 13)`),
          'important captions became too small');
        await evaluate('window.scrollTo(0, 0)');
        if (systemTheme === 'dark') {
          const homeScreenshot = await send('Page.captureScreenshot', { format: 'png' });
          await writeFile(path.join(artifacts, `home-${theme}-${width}.png`), Buffer.from(homeScreenshot.data, 'base64'));
        }
      }
      await evaluate('document.getElementById("learning-route").scrollIntoView({block: "center"})');
      const screenshot = await send('Page.captureScreenshot', { format: 'png' });
      await writeFile(path.join(artifacts, `select-${theme}-os-${systemTheme}.png`), Buffer.from(screenshot.data, 'base64'));
    }
  }
  // Representative opaque Telegram palettes. Keep a good supplied pair unchanged;
  // replace only its text when the supplied pair is unreadable.
  for (const theme of ['dark', 'light']) {
    await evaluate(`Telegram.WebApp.colorScheme = '${theme}'`);
    for (const [background, ink, keepInk] of [
      ['#4d88ef', '#ffffff', false], ['#141c2b', '#ffffff', true],
      ['#b9efdf', '#ffffff', false], ['#b9efdf', '#111827', true],
      ['#111827', '#111827', false], ['#777777', '#ffffff', false],
    ]) {
      const pair = await evaluate(`(() => {
        document.documentElement.style.setProperty('--tg-theme-button-color', '${background}');
        document.documentElement.style.setProperty('--tg-theme-button-text-color', '${ink}');
        window.emitTestThemeChanged();
        const style = getComputedStyle(document.getElementById('quick-start'));
        return { background: style.backgroundColor, color: style.color };
      })()`);
      const a = luminance(pair.color), b = luminance(pair.background);
      assert((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) >= 4.5,
        `primary contrast failed for ${theme}/${background}/${ink}`);
      const rgb = hex => 'rgb(' + hex.slice(1).match(/../g).map(x => parseInt(x, 16)).join(', ') + ')';
      assert.equal(pair.background, rgb(background), 'contrast adaptation changed Telegram button background');
      if (keepInk) assert.equal(pair.color, rgb(ink), 'a readable Telegram text color was replaced');
    }
  }
  await evaluate(`['--tg-theme-bg-color', '--tg-theme-secondary-bg-color', '--tg-theme-text-color'].forEach(
    name => document.documentElement.style.removeProperty(name));
    ['--tg-theme-button-color', '--tg-theme-button-text-color'].forEach(
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
  for (const reduced of [false, true]) {
    await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: reduced ? 'reduce' : 'no-preference' }] });
    await evaluate('document.getElementById("training-settings").open = false; window.scrollTo(0, 0)');
    const point = await evaluate(`(() => {
      const r = document.getElementById('open-settings').getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    })()`);
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...point });
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', ...point, button: 'left', clickCount: 1 });
    await pause(160);
    const press = await evaluate(`(() => {
      const button = document.getElementById('open-settings');
      const style = getComputedStyle(button);
      return { active: button.matches(':active'), transform: style.transform,
        duration: style.transitionDuration, property: style.transitionProperty };
    })()`);
    assert(press.active, 'pointer did not reach the real settings button');
    if (reduced) {
      assert.equal(press.transform, 'none', 'reduced motion still moved the button');
      assert.equal(press.duration, '0s');
    } else {
      assert.notEqual(press.transform, 'none', 'pointer press has no state feedback');
      assert.equal(press.duration, '0.12s');
      assert.equal(press.property, 'transform');
    }
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...point, button: 'left', clickCount: 1 });
  }
  await send('Emulation.setEmulatedMedia', { features: [] });
  await evaluate('document.getElementById("training-settings").open = false; document.getElementById("quick-start").focus()');
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 });
  assert.equal(await evaluate('document.activeElement.id'), 'open-settings');
  assert.equal(await evaluate('getComputedStyle(document.activeElement).transitionDuration'), '0s',
    'keyboard navigation gained a motion delay');
  assert.equal(await evaluate('document.getElementById("editorial-tab").hidden'), false);
  assert.equal(await evaluate('document.getElementById("stat-accuracy").textContent'), '—', 'empty accuracy was shown as a measured result');
  assert.equal(await evaluate('document.getElementById("stat-first-answer-label").textContent'), 'с первого ответа');
  assert.doesNotMatch(await evaluate('document.getElementById("today-copy").textContent'), /семейств|по сроку/);
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Тренировать');
  assert.equal(await evaluate('document.getElementById("next-action-title").textContent'), 'Твоя тренировка');
  assert.equal(await evaluate('document.querySelector("#main-tabs [aria-current=page]").dataset.tab'), 'home');
  await evaluate('document.getElementById("open-settings").click()');
  assert.equal(await evaluate('document.getElementById("training-settings").open'), true);
  assert.equal(await evaluate('document.activeElement === document.querySelector("#training-settings > summary")'), true,
    'keyboard focus did not follow the settings action');
  await evaluate('document.getElementById("training-settings").open = false');
  await evaluate('document.querySelector(".map-explainer > summary").click()');
  assert.equal(await evaluate('document.querySelector(".map-explainer").open'), true);
  assert.match(await evaluate('document.querySelector(".skill-map-note").textContent'), /подтверждено.*охват/);
  assert.equal(await evaluate('Boolean(document.querySelector("#skill-map-chart svg"))'), true);
  await evaluate('document.querySelector(".map-explainer > summary").click(); window.scrollTo(0, 0)');
  assert.equal(await evaluate('document.getElementById("today-copy").hidden'), true);
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'training rendering');
  assert.equal(await evaluate('document.getElementById("training-plan-copy").hidden'), true);
  assert.match(await evaluate('document.getElementById("progress-fill").style.transform'), /^scaleX\(/);
  assert.equal(await evaluate('document.getElementById("progress-fill").style.width'), '');
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
  assert.equal(await evaluate('document.getElementById("next-action-title").textContent'), 'Начатая тренировка');
  await evaluate('const topic = document.querySelector("input[name=topic]"); topic.checked = true; topic.dispatchEvent(new Event("change", { bubbles: true }))');
  assert.equal(await evaluate('document.getElementById("quick-start").textContent'), 'Тренировать', 'changed topic still resumed the previous training');
  assert.equal(await evaluate('document.getElementById("next-action-title").textContent'), 'Тренировка по выбору');
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
  const containerFixtures = new Map(await Promise.all(
    (await readdir('content/questions/containers', { recursive: true })).filter(name => name.endsWith('.json'))
      .map(async name => {
        const card = JSON.parse(await readFile(path.join('content/questions/containers', name), 'utf8'));
        return [card.id, card];
      })));
  // Pin this single-choice help scenario to a real route instead of assuming
  // every randomly selected code card has choice inputs. Written calls are
  // exercised explicitly by the review pilot below.
  const practice = await ownerApi('sessions', { route_id: 'containers_iterators' });
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
  assert.equal(await evaluate(`document.getElementById('saved-training-count').textContent ===
    String(document.querySelectorAll('#saved-training-list button').length)`), true);
  assert.equal(await evaluate(`Boolean(document.getElementById('saved-trainings').compareDocumentPosition(
    document.getElementById('learning-route')) & Node.DOCUMENT_POSITION_FOLLOWING)`), true,
    'saved trainings were buried below optional routes');
  assert(await evaluate(`Array.from(document.querySelectorAll('#saved-training-list button')).every(button =>
    button.querySelector('.saved-training-title')?.textContent && button.querySelector('.saved-training-detail')?.textContent)`),
    'a saved training has no readable topic or progress');
  await evaluate('document.getElementById("saved-trainings").scrollIntoView({block: "start"})');
  const savedScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'saved-trainings-mobile.png'), Buffer.from(savedScreenshot.data, 'base64'));
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
  const lifetimeTargets = new Set(['pointer_after_local_lifetime', 'c_str_from_temporary_dangles', 'unique_ptr_move_transfers_ownership',
    'unique_ptr_make_get_observe', 'unique_ptr_reset_destroys_now']);
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
  let container = await ownerApi('sessions', { route_id: 'containers_iterators' });
  for (let i = 0; container.question?.id !== 'vector_erase_return_next' && i < 8; ++i) {
    if (container.status === 'completed') {
      container = await ownerApi('sessions', { route_id: 'containers_iterators' });
      continue;
    }
    assert(container.question, 'container session ended before reaching erase practice');
    const card = containerFixtures.get(container.question.id);
    assert(card, 'the routed container fixture is absent from the current bank');
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
  let design = await ownerApi('sessions', { modes: ['theory'], topics: ['oop'], level: 2 });
  for (let i = 0; design.question?.id !== 'srp_actor_not_method_count' && i < 10; ++i) {
    assert(design.question, 'design session ended before reaching the actor introduction');
    const card = JSON.parse(await readFile(`content/questions/oop/${design.question.id}.json`, 'utf8'));
    assert.equal(card.task.type, 'single_choice', 'unexpected design introduction fixture');
    await ownerApi('session/answer', { question_id: card.id, answer: card.task.correct_option_id, session_id: design.session_id });
    design = await ownerApi('session/next', { session_id: design.session_id });
  }
  assert.equal(design.question?.id, 'srp_actor_not_method_count');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before SRP help');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'SRP practice');
  assert.equal(await evaluate('document.getElementById("question-standard").textContent'), 'Принципы проектирования');
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'SRP reference opens before answering');
  assert.equal(await evaluate('document.getElementById("help-kind").textContent'), 'СПРАВКА · ПРОЕКТИРОВАНИЕ');
  assert.match(await evaluate('document.getElementById("theory-principle").textContent'), /актор/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true, 'design help overflows mobile');
  const designScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'srp-reference-mobile.png'), Buffer.from(designScreenshot.data, 'base64'));
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
  assert.equal(await evaluate('document.querySelectorAll("#learning-section-select option").length'), 4);
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-select option").length'), 2);
  assert.equal(await evaluate('document.querySelector("#learning-route-select option[value=lookup_code_review]")'), null);
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
  await evaluate(`Array.from(document.querySelectorAll('.skill-composition button')).find(
    button => button.textContent.includes('Время жизни и RAII')).click()`);
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'lifetime_raii');
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-steps li").length'), 9);
  assert.match(await evaluate('document.getElementById("learning-route-methods").textContent'), /make_unique.*get.*move.*reset/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'the longer lifetime route overflows the mobile viewport');
  await evaluate('document.getElementById("learning-route").scrollIntoView({block: "start"})');
  const lifetimeRouteScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'lifetime-route-mobile.png'), Buffer.from(lifetimeRouteScreenshot.data, 'base64'));
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'lifetime route starts from the skill map');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000010');
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'lifetime reference opens before answering');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after lifetime route reload');
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'lifetime_raii');
  assert.equal(await evaluate('document.getElementById("start-learning-route").textContent'), 'Продолжить начатый шаг');
  await evaluate(`Array.from(document.querySelectorAll('.skill-composition button')).find(
    button => button.textContent.includes('Шаблоны: пакеты')).click()`);
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'template_packs_folds');
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-steps li").length'), 7);
  assert.match(await evaluate('document.getElementById("learning-route-methods").textContent'), /sizeof.*пустой пакет/);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'the template route overflows the mobile viewport');
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'template route starts from map');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000169');
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'template route reference opens');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after template route reload');
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'template_packs_folds');
  assert.equal(await evaluate('document.getElementById("start-learning-route").textContent'), 'Продолжить начатый шаг');
  await evaluate(`document.getElementById('learning-section-select').value = 'references';
    document.getElementById('learning-section-select').dispatchEvent(new Event('change'));`);
  assert.equal(await evaluate('document.getElementById("learning-unit-field").hidden'), true);
  assert.equal(await evaluate('document.querySelectorAll("#learning-route-steps li").length'), 6);
  assert.match(await evaluate('document.getElementById("learning-route-methods").textContent'), /lvalue.*int&/);
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'references route starts');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000202');
  await evaluate('document.getElementById("reference-help").open = true; document.querySelector("#reference-links button").click()');
  await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'value categories reference opens');
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home after references route reload');
  assert.equal(await evaluate('document.getElementById("learning-route-select").value'), 'values_references');
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'references route resumes');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000202');
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before returning to first route');
  const regularApi = async (route, body = {}) => {
    const response = await fetch(new URL(`api/v1/${route}`, origin), {
      method: 'POST', headers: { Authorization: `tma ${signedInitData(ownerId + 1)}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    assert.equal(response.status, 200);
    return response.json();
  };
  // Legacy routes stay callable for existing sessions, but are no longer a new
  // navigation entry. Create the persisted fixture through the compatible API.
  for (const [index, publicId, answer] of [
    [0, 'CR-000208', 'cache . find ( key )'],
    [1, 'CR-000210', 'prices.try_emplace( key, value )'],
    [2, 'CR-000212', 'a'],
    [3, 'CR-000213', 'c'],
  ]) {
    await regularApi('sessions', { route_id: 'lookup_code_review' });
    await send('Page.reload');
    await until(() => evaluate('!document.getElementById("home").hidden'), 'legacy pilot home');
    assert.equal(await evaluate('document.querySelector("#learning-route-select option[value=lookup_code_review]")'), null);
    await evaluate('document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), `pilot stage ${index} starts`);
    assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), publicId);
    await evaluate(`document.getElementById('reference-help').open = true;
      Array.from(document.querySelectorAll('#reference-links button')).find(
        button => button.textContent.includes('Как читать описание')).click();`);
    await until(() => evaluate('!document.getElementById("theory-dialog").hidden'), 'contract-reading help opens');
    if (index === 0) {
      await send('Page.reload');
      await until(() => evaluate('!document.getElementById("home").hidden'), 'pilot home after reload');
      await evaluate('document.getElementById("quick-start").click()');
      await until(() => evaluate('!document.getElementById("training").hidden'), 'pilot resumes after reload');
      assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), publicId);
      assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);
    } else {
      await evaluate('document.getElementById("close-theory").click()');
    }
    assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
      'pilot content overflows the mobile viewport');
    if (index < 2) {
      await evaluate(`document.getElementById('short-answer').value = ${JSON.stringify(answer)};`);
    } else {
      await evaluate('document.getElementById("reference-help").open = false; window.scrollTo(0, 0)');
      const shot = await send('Page.captureScreenshot', { format: 'png' });
      await writeFile(path.join(artifacts, `code-review-${index}-mobile.png`), Buffer.from(shot.data, 'base64'));
      await evaluate(`document.querySelector('#answer-area input[value="${answer}"]').checked = true;`);
    }
    await evaluate('document.getElementById("submit-answer").click()');
    await until(() => evaluate('!document.getElementById("feedback").hidden'), 'pilot answer accepted');
    await evaluate('document.getElementById("next-question").click()');
    await until(() => evaluate('!document.getElementById("completed").hidden'), 'pilot stage completed');
    await evaluate('document.getElementById("back-home").click()');
    await until(() => evaluate('!document.getElementById("home").hidden && !document.body.classList.contains("busy")'),
      'pilot home after a completed stage');
  }
  assert.equal((await regularApi('bootstrap')).learning_routes.find(route => route.id === 'lookup_code_review').status, 'completed');
  assert.match(await evaluate('document.getElementById("skill-map-total").textContent'), /из 20/);
  await evaluate(`document.getElementById('learning-section-select').value = 'containers';
    document.getElementById('learning-section-select').dispatchEvent(new Event('change'));`);
  assert.equal(await evaluate('document.getElementById("start-learning-route").textContent'), 'Продолжить начатый шаг');
  await evaluate('document.getElementById("start-learning-route").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'first route resumes after second starts');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000155');
  // Network acceptance: lose a Next receipt only after the real server committed.
  // This is a simulation, not evidence of a real Telegram user's experience.
  const beforeRecovery = await regularApi('bootstrap');
  const answerCurrentChoice = async () => {
    const current = (await regularApi('bootstrap')).session;
    const directory = `content/questions/${current.question.topic}`;
    const candidates = await Promise.all((await readdir(directory, { recursive: true })).filter(name => name.endsWith('.json'))
      .map(async name => JSON.parse(await readFile(path.join(directory, name), 'utf8'))));
    const card = candidates.find(item => item.id === current.question.id);
    assert(card, 'recovery card missing from the local bank');
    assert.equal(card.task.type, 'single_choice');
    await evaluate(`document.querySelector('#answer-area input[value="${card.task.correct_option_id}"]').checked = true;
      document.getElementById('submit-answer').click();`);
    await until(() => evaluate('!document.getElementById("feedback").hidden'), 'answer before lost Next receipt');
  };
  await answerCurrentChoice();
  const afterAnswer = await regularApi('bootstrap');
  await evaluate(`window.__nextTestFetch = window.fetch;
    window.__loseNextReceipt = true;
    window.fetch = async (...args) => {
      const route = String(args[0]);
      if (route.endsWith('/session/next')) {
        window.__lastNextRequest = JSON.parse(args[1].body);
        if (window.__loseNextBeforeSend) {
          window.__loseNextBeforeSend = false;
          throw new TypeError('Смоделирован обрыв до отправки');
        }
      }
      if ((route.endsWith('/bootstrap') || route.endsWith('/session/result')) && window.__loseRecoveryRead) {
        window.__loseRecoveryRead = false;
        throw new TypeError('Смоделирован обрыв чтения состояния');
      }
      const response = await window.__nextTestFetch(...args);
      if (route.endsWith('/session/next') && window.__loseNextReceipt && response.ok) {
        window.__loseNextReceipt = false;
        // This reply is deliberately discarded, so consume it directly rather
        // than retaining an unread branch of a cloned response stream.
        const payload = await response.json();
        if (payload.status === 'completed') window.__lastCompletion = payload;
        throw new TypeError('Смоделирована потеря квитанции перехода');
      }
      return response;
    };`);
  await evaluate('document.getElementById("next-question").click()');
  await until(() => evaluate('!document.getElementById("training").hidden && document.getElementById("question-public-id").textContent === "CR-000174"'),
    'recover committed Next without being stuck on the old feedback');
  const afterRecoveredNext = await regularApi('bootstrap');
  assert.equal(afterRecoveredNext.summary.attempt_count, afterAnswer.summary.attempt_count);
  assert.equal(afterRecoveredNext.summary.xp.total, afterAnswer.summary.xp.total);
  assert.equal(afterRecoveredNext.session.session_id, afterAnswer.session.session_id);
  assert.equal(await evaluate('window.__lastNextRequest.expected_total_attempts'), afterAnswer.session.progress.total_attempts);
  await answerCurrentChoice();
  await evaluate('window.__loseNextReceipt = true; document.getElementById("next-question").click()');
  await until(() => evaluate('!document.getElementById("completed").hidden'), 'recover exact final completion receipt');
  const afterCompletion = await regularApi('bootstrap');
  assert.equal(afterCompletion.session, null);
  assert.equal(afterCompletion.summary.attempt_count, beforeRecovery.summary.attempt_count + 2);
  assert.equal(afterCompletion.summary.sessions_completed, beforeRecovery.summary.sessions_completed + 1);
  const completion = await regularApi('session/result', { session_id: afterAnswer.session.session_id });
  assert.deepEqual(completion, await evaluate('window.__lastCompletion'));
  assert.deepEqual(await regularApi('session/next', await evaluate('window.__lastNextRequest')), completion);
  const completedCopy = await evaluate('document.getElementById("completed-copy").textContent');
  assert.match(completedCopy, /Карточек завершено: 2/);
  assert.match(completedCopy, new RegExp(`\\+${completion.xp_gained} XP`));
  assert.equal(await evaluate('sessionStorage.getItem("cpp-recall-pending-next-v1")'), null);
  // A failed home refresh must not overwrite the recovered training result.
  await evaluate(`window.__loseRecoveryRead = true; document.getElementById('back-home').click()`);
  await until(() => evaluate('document.getElementById("completed-error").textContent.length > 0'), 'home read error after completion');
  assert.equal(await evaluate('document.getElementById("completed-copy").textContent'), completedCopy);
  // Reload after both the final reply and its reconciliation read were lost.
  // Persist only a recovery intent; no answers or Telegram credentials.
  await evaluate(`sessionStorage.setItem('cpp-recall-pending-next-v1', JSON.stringify({
    userId: ${ownerId + 1}, sessionId: ${completion.session_id} }));`);
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("completed").hidden'), 'recover completion on tab reload');
  assert.equal(await evaluate('document.getElementById("completed-copy").textContent'), completedCopy);
  assert.deepEqual(await regularApi('session/result', { session_id: completion.session_id }), completion);
  // Neither the write nor the reconciliation read is guaranteed to reach the
  // server. Keep feedback usable and preserve its meaning until a later retry.
  await regularApi('sessions', { route_id: 'containers_iterators' });
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before second recovery stage');
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'next available route stage');
  await answerCurrentChoice();
  const originalTitle = await evaluate('document.getElementById("result-title").textContent');
  const beforeNoSend = await regularApi('bootstrap');
  // Page.reload removed the fetch wrapper: create a fresh wrapper for these cases.
  await evaluate(`window.__nextTestFetch = window.fetch;
    window.fetch = async (...args) => {
      const route = String(args[0]);
      if (route.endsWith('/session/next') && window.__loseNextBeforeSend) {
        window.__loseNextBeforeSend = false;
        throw new TypeError('Смоделирован обрыв до отправки');
      }
      if ((route.endsWith('/bootstrap') || route.endsWith('/session/result')) && window.__loseRecoveryRead) {
        window.__loseRecoveryRead = false;
        throw new TypeError('Смоделирован обрыв чтения состояния');
      }
      const response = await window.__nextTestFetch(...args);
      if (route.endsWith('/session/next') && window.__loseNextReceipt && response.ok) {
        window.__loseNextReceipt = false;
        throw new TypeError('Смоделирована потеря квитанции перехода');
      }
      return response;
    };
    window.__loseNextBeforeSend = true; document.getElementById("next-question").click();`);
  await until(() => evaluate('document.getElementById("feedback-error").textContent.includes("до отправки")'),
    'failed request leaves an actionable feedback screen');
  assert.equal(await evaluate('document.getElementById("result-title").textContent'), originalTitle);
  assert.equal((await regularApi('bootstrap')).session.phase, 'feedback');
  await evaluate('window.__loseNextReceipt = true; window.__loseRecoveryRead = true; document.getElementById("next-question").click()');
  await until(() => evaluate('document.getElementById("feedback-error").textContent.includes("Дождись связи")'),
    'failed reconciliation leaves a retry instruction');
  assert.equal(await evaluate('document.getElementById("result-title").textContent'), originalTitle);
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'recovery instruction overflows the mobile viewport');
  const recoveryErrorScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'recovery-feedback-mobile.png'), Buffer.from(recoveryErrorScreenshot.data, 'base64'));
  assert.notEqual(await evaluate('sessionStorage.getItem("cpp-recall-pending-next-v1")'), null);
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("completed").hidden'), 'tab reload recovers the committed result after both replies were lost');
  assert.match(await evaluate('document.getElementById("completed-copy").textContent'), /Карточек завершено: 1/);
  assert.equal(await evaluate('sessionStorage.getItem("cpp-recall-pending-next-v1")'), null);
  const afterReadRetry = await regularApi('bootstrap');
  assert.equal(afterReadRetry.summary.attempt_count, beforeRecovery.summary.attempt_count + 3);
  assert.equal(afterReadRetry.summary.sessions_completed, beforeRecovery.summary.sessions_completed + 2);
  assert.equal(afterReadRetry.summary.xp.total, beforeNoSend.summary.xp.total);
  assert.deepEqual(afterReadRetry.saved_sessions.map(item => item.id).sort(), beforeRecovery.saved_sessions.map(item => item.id).sort(),
    'network recovery lost an unrelated saved training');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'recovered completion overflows the mobile viewport');
  const recoveredHomeScreenshot = await send('Page.captureScreenshot', { format: 'png' });
  await writeFile(path.join(artifacts, 'recovery-completed-mobile.png'), Buffer.from(recoveredHomeScreenshot.data, 'base64'));
  // Presentation-only fixture: no rewards remain. Do not manufacture XP in the database.
  await evaluate(`{
    window.__friendlyFetch = window.fetch;
    window.fetch = async (...args) => {
      const response = await window.__friendlyFetch(...args);
      if (!String(args[0]).endsWith('/bootstrap') || !response.ok) return response;
      const data = await response.json();
      data.summary.xp.next_reward = null;
      data.user.first_name = 'ПроверкаОченьДлинногоИмениБезПробелов';
      data.saved_sessions.push({ id: 987654321, available: false });
      return new Response(JSON.stringify(data), { status: 200, headers: { 'Content-Type': 'application/json' } });
    };
    document.getElementById('back-home').click();
  }`);
  await until(() => evaluate('document.getElementById("open-rewards").classList.contains("rewards-complete")'), 'all rewards presentation');
  assert.equal(await evaluate('document.getElementById("next-reward-track").hidden'), true,
    'all rewards still showed a fictitious next goal');
  assert.equal(await evaluate('document.getElementById("next-reward-copy").textContent'), 'Достижения →');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'a long name or unavailable saved training overflows the home');
  assert.equal(await evaluate(`document.querySelector('#saved-training-list button[data-session-id="987654321"]').disabled`), true);
  assert.match(await evaluate(`document.querySelector('#saved-training-list button[data-session-id="987654321"]').textContent`),
    /Сохранённая тренировка.*сейчас не открывается.*Недоступна/);
  await evaluate('document.documentElement.style.fontSize = "200%"');
  assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
    'home overflows with doubled root text size and a long name');
  await evaluate('document.documentElement.style.removeProperty("font-size")');
  await evaluate('document.getElementById("open-rewards").click()');
  assert.equal(await evaluate('document.getElementById("achievements").hidden'), false);
  assert.equal(await evaluate('document.querySelector("#main-tabs [aria-current=page]").dataset.tab'), 'achievements');
  await evaluate('window.fetch = window.__friendlyFetch; document.getElementById("back-home").click()');
  await until(() => evaluate('!document.getElementById("home").hidden && !document.body.classList.contains("busy")'), 'real home restored');
  assert.equal((await regularApi('bootstrap')).summary.xp.total, afterReadRetry.summary.xp.total,
    'a presentation fixture changed earned XP');
  // Formats are independent of topics, not new routes per container.
  const activeBeforeEmpty = (await regularApi('bootstrap')).session?.session_id || null;
  await evaluate(`document.getElementById('training-settings').open = true;
    document.querySelectorAll('input[name="mode"]').forEach(input => input.checked = input.value === 'write_code');
    document.querySelectorAll('input[name="topic"]').forEach(input => input.checked = input.value === 'oop');
    document.querySelector('input[name="level"][value="3"]').checked = true;
    document.getElementById('start-session').click();`);
  await until(() => evaluate('document.getElementById("setup-error").textContent.includes("пока нет подходящих заданий")'),
    'empty topic/format combination explains the lack of tasks');
  assert.equal((await regularApi('bootstrap')).session?.session_id || null, activeBeforeEmpty);
  await evaluate(`document.getElementById('training-settings').open = true;
    document.querySelector('input[name="mode"][value="write_code"]').checked = false;
    document.querySelector('input[name="mode"][value="write_code"]').click();`);
  assert.equal(await evaluate('document.querySelector("input[name=mode][value=code]").checked'), false);
  await evaluate('document.querySelector("input[name=mode][value=review_code]").click()');
  assert.equal(await evaluate('document.querySelector("input[name=mode][value=write_code]").checked'), true);
  await evaluate('document.querySelector("input[name=mode][value=code]").click()');
  assert.equal(await evaluate('document.querySelector("input[name=mode][value=write_code]").checked'), false);
  assert.equal(await evaluate('document.querySelector("input[name=mode][value=review_code]").checked'), false);
  const startedFormats = [];
  for (const [mode, kind] of [['write_code', 'implementation'], ['review_code', 'debugging']]) {
    await evaluate(`document.getElementById('training-settings').open = true;
      document.querySelectorAll('input[name="mode"]').forEach(input => input.checked = input.value === '${mode}');
      document.querySelectorAll('input[name="topic"]').forEach(input => input.checked = input.value === 'containers');
      document.querySelector('input[name="level"][value="3"]').checked = true;
      document.getElementById('training-settings').dispatchEvent(new Event('change', {bubbles: true}));`);
    for (const width of [390, 768, 1440]) {
      await send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 600 });
      await evaluate('document.getElementById("training-settings").scrollIntoView({block: "start"})');
      assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true,
        `practice settings overflow ${width}`);
      const image = await send('Page.captureScreenshot', { format: 'png' });
      await writeFile(path.join(artifacts, `practice-${mode}-${width}.png`), Buffer.from(image.data, 'base64'));
    }
    await evaluate('document.getElementById("start-session").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), `${mode} starts from settings`);
    const current = (await regularApi('bootstrap')).session;
    const card = containerFixtures.get(current.question.id);
    assert(card && card.kind === kind && card.topic === 'containers' && card.level <= 3,
      'topic/format/difficulty selection leaked');
    if (mode === 'write_code') assert.equal(card.task.type, 'short_answer');
    startedFormats.push(current.session_id);
    await send('Page.reload');
    await until(() => evaluate('!document.getElementById("home").hidden'), 'home preserves specialized training');
    await evaluate('document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'specialized training resumes');
    assert.equal((await regularApi('bootstrap')).session.question.id, current.question.id);
    await send('Page.reload');
    await until(() => evaluate('!document.getElementById("home").hidden'), 'home after specialized resume');
  }
  assert((await regularApi('bootstrap')).saved_sessions.some(session => session.id === startedFormats[0]),
    'new format removed the previous unfinished training');
  assert.match(await evaluate(`document.querySelector('#saved-training-list button[data-session-id="${startedFormats[0]}"]').textContent`),
    /Написать код/);
  await evaluate(`document.querySelector('#saved-training-list button[data-session-id="${startedFormats[0]}"]').click()`);
  await until(() => evaluate('!document.getElementById("training").hidden'), 'saved write-code training resumes');
  assert.equal((await regularApi('bootstrap')).session.session_id, startedFormats[0]);
  await send('Page.reload');
  await until(() => evaluate('!document.getElementById("home").hidden'), 'home before grouped-navigation captures');
  for (const width of [390, 768, 1440]) {
    await send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 600 });
    await evaluate('document.getElementById("learning-route").scrollIntoView({block: "start"})');
    assert.equal(await evaluate('document.documentElement.scrollWidth <= window.innerWidth'), true);
    const image = await send('Page.captureScreenshot', { format: 'png' });
    await writeFile(path.join(artifacts, `learning-sections-${width}.png`), Buffer.from(image.data, 'base64'));
  }
  await checkSharedChallenges({ send, evaluate, until, origin, signedInitData, userId: ownerId + 10, artifacts });
  assert.deepEqual(errors, [], 'browser script errors');
  console.log(`Browser feedback check passed. Screenshots: ${artifacts}`);
} catch (error) {
  if (ws) {
    try {
      console.error('Browser failure state:', await evaluate(`({
        screens: ['home', 'training', 'feedback', 'completed'].filter(id => !document.getElementById(id).hidden),
        busy: document.body.classList.contains('busy'),
        error: document.getElementById('feedback-error').textContent,
        completionCaptured: Boolean(window.__lastCompletion),
      })`));
      const failedScreenshot = await send('Page.captureScreenshot', { format: 'png' });
      await writeFile(path.join(artifacts, 'failure.png'), Buffer.from(failedScreenshot.data, 'base64'));
      console.error(`Browser failure screenshot: ${artifacts}/failure.png`);
    } catch { /* Keep the original failure when the browser itself is unavailable. */ }
  }
  console.error(appLog);
  throw error;
} finally {
  ws?.close();
  browser?.kill('SIGTERM');
  app.kill('SIGTERM');
}
