// Node 22+, Chromium, built bot, and disposable TEST_DATABASE_URL only.
// Synthetic users; no .env, external Telegram calls, or production access.
// Additional unavailable/error/rollover states below are explicitly simulated.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHmac, randomInt } from 'node:crypto';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

export async function checkDaily({ send, evaluate, until, origin, signedInitData, userId, artifacts }) {
  const request = async (route, body = {}) => {
    const response = await fetch(new URL(`api/v1/${route}`, origin), {
      method: 'POST', headers: { Authorization: `tma ${signedInitData(userId)}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    assert.equal(response.status, 200, await response.clone().text());
    return response.json();
  };
  const injection = await send('Page.addScriptToEvaluateOnNewDocument', { source: `
    window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(userId))}, colorScheme: 'dark',
      onEvent(name, callback) { if (name === 'themeChanged') window.__dailyTheme = callback; },
      ready() {}, expand() {}, setHeaderColor() {}, setBackgroundColor() {},
      enableClosingConfirmation() {}, disableClosingConfirmation() {},
      BackButton: { show() {}, hide() {}, onClick() {} }, showConfirm(text, callback) { callback(true); } } };
    window.__dailyCalls = [];
    const original = window.fetch;
    window.fetch = async (...args) => {
      const route = new URL(String(args[0])).pathname.split('/').pop();
      if (['daily', 'sessions', 'stop'].includes(route)) window.__dailyCalls.push({ route, body: JSON.parse(args[1].body) });
      if (route === 'daily') {
        const override = window.__dailyOverride, fail = window.__dailyFail;
        if (window.__dailyHold) await new Promise(resolve => { window.__releaseDaily = resolve; });
        if (fail) return new Response('{}', { status: 503 });
        if (override) return new Response(JSON.stringify(override), { status: 200 });
      }
      return original(...args);
    };
  ` });
  const shot = async name => {
    assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `overflow: ${name}`);
    const capture = await send('Page.captureScreenshot', { format: 'png' });
    await writeFile(path.join(artifacts, name), Buffer.from(capture.data, 'base64'));
  };
  const home = () => until(() => evaluate('!document.getElementById("home").hidden && document.getElementById("daily-task").getAttribute("aria-busy") === "false"'), 'daily home loaded');
  try {
    const before = await request('bootstrap');
    assert.equal(before.session, null, 'daily scenario requires a fresh synthetic user');
    await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 900, deviceScaleFactor: 1, mobile: true });
    await send('Page.navigate', { url: origin });
    await home();
    assert.equal((await request('bootstrap')).session, null, 'viewing daily created a session');
    assert.equal(await evaluate('window.__dailyCalls.some(call => call.route === "sessions")'), false);
    assert.equal(await evaluate('document.getElementById("start-daily").textContent'), 'Начать задание');
    const metadata = await request('daily', { timezone: 'Europe/Moscow' });
    assert.equal(metadata.status, 'ready');
    const browserDay = await evaluate('document.getElementById("daily-timezone").textContent');
    assert(browserDay.includes(metadata.timezone), 'server timezone missing in details');

    for (const theme of ['dark', 'light']) {
      await evaluate(`{
        const colors = ${JSON.stringify({
          dark: { bg: '#17212b', surface: '#232e3c', text: '#f5f5f5', link: '#73a9ff' },
          light: { bg: '#ffffff', surface: '#f0f2f5', text: '#1f2937', link: '#245ea8' },
        })}['${theme}'];
        for (const [name, value] of Object.entries({ 'bg-color': colors.bg,
          'secondary-bg-color': colors.surface, 'text-color': colors.text, 'link-color': colors.link }))
          document.documentElement.style.setProperty('--tg-theme-' + name, value);
        Telegram.WebApp.colorScheme = '${theme}'; window.__dailyTheme();
      }`);
      for (const width of [390, 768, 1440]) {
        await send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 600 });
        const layout = await evaluate(`(() => {
          const card = document.getElementById('daily-task').getBoundingClientRect();
          const practice = document.querySelector('.next-action').getBoundingClientRect();
          const action = document.getElementById('start-daily').getBoundingClientRect();
          return { primaryBottom: document.getElementById('quick-start').getBoundingClientRect().bottom,
            optionalAfterPractice: card.top >= practice.bottom, height: action.height,
            map: Boolean(document.getElementById('skill-map-chart').children.length) };
        })()`);
        assert(layout.primaryBottom < 560 && layout.optionalAfterPractice && layout.height >= 44 && layout.map);
        await evaluate('document.getElementById("daily-details").open = true; window.scrollTo(0, 0)');
        const contrast = await evaluate(`(() => {
          // Computed color-mix values serialize as color(srgb 0..1), not rgb(0..255).
          // Let the browser convert either CSS color syntax to the same pixel space.
          const context = document.createElement('canvas').getContext('2d');
          const pixels = color => {
            context.clearRect(0, 0, 1, 1);
            context.fillStyle = color;
            context.fillRect(0, 0, 1, 1);
            return Array.from(context.getImageData(0, 0, 1, 1).data);
          };
          const luminance = rgba => rgba.slice(0, 3).map(c => c / 255)
            .map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4)
            .reduce((sum, c, i) => sum + c * [.2126, .7152, .0722][i], 0);
          const background = getComputedStyle(document.getElementById('daily-task')).backgroundColor;
          const groundRgba = pixels(background), ground = luminance(groundRgba);
          return ['daily-status', 'daily-timezone'].map(id => {
            const color = getComputedStyle(document.getElementById(id)).color;
            const inkRgba = pixels(color), ink = luminance(inkRgba);
            return { id, color, background, inkRgba, groundRgba,
              ratio: (Math.max(ink, ground) + .05) / (Math.min(ink, ground) + .05) };
          });
        })()`);
        await shot(`daily-ready-${theme}-${width}.png`);
        assert(contrast.every(value => value.ratio >= 4.5 && value.inkRgba[3] === 255 && value.groundRgba[3] === 255),
          `daily text contrast: ${theme}/${width}: ${JSON.stringify(contrast)}`);
      }
    }
    await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 900, deviceScaleFactor: 1, mobile: true });
    await evaluate('document.getElementById("start-daily").focus()');
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
    await until(() => evaluate('!document.getElementById("training").hidden'), 'daily starts by keyboard');
    const started = (await request('bootstrap')).session;
    assert.equal(started.daily_day, metadata.local_day);
    const startBody = await evaluate('window.__dailyCalls.find(call => call.route === "sessions").body');
    assert.equal(startBody.daily, true);
    assert.equal(startBody.daily_day, metadata.local_day);
    assert.equal(startBody.timezone, metadata.timezone);
    assert.equal(await evaluate('document.getElementById("finish-session").textContent'), 'Отложить задание');
    await evaluate('document.getElementById("finish-session").click()');
    await home();
    assert.equal(await evaluate('window.__dailyCalls.some(call => call.route === "stop")'), false, 'daily postpone stopped session');
    assert.equal(await evaluate('document.getElementById("start-daily").textContent'), 'Продолжить задание');
    await evaluate('document.getElementById("start-daily").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'daily resumes');
    assert.equal((await request('bootstrap')).session.session_id, started.session_id, 'resume created another daily');
    await evaluate('document.getElementById("stop-session").click()');
    await home();

    // SIMULATION: backend metadata failure must not lock the ordinary practice path.
    await evaluate('window.__dailyFail = true; document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'quick start still works');
    await evaluate('document.getElementById("stop-session").click()');
    await home();
    assert.equal(await evaluate('document.getElementById("retry-daily").hidden'), false);
    assert.equal(await evaluate('document.getElementById("quick-start").disabled'), false);
    await shot('daily-error-390.png');
    await evaluate('window.__dailyFail = false; document.getElementById("retry-daily").click()');
    await home();

    // SIMULATION: metadata loading does not use the global API button lock.
    await evaluate('window.__dailyFail = true; document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'enter before loading fixture');
    await evaluate('document.getElementById("stop-session").click()');
    await home();
    await evaluate('window.__dailyFail = false; window.__dailyHold = true; document.getElementById("retry-daily").click()');
    await until(() => evaluate('Boolean(window.__releaseDaily)'), 'held daily metadata');
    assert.equal(await evaluate('document.getElementById("quick-start").disabled'), false);
    await shot('daily-loading-390.png');
    await evaluate('window.__dailyHold = false; window.__releaseDaily()');
    await home();

    // SIMULATION: an older metadata response must not overwrite a newer bootstrap's daily state.
    await evaluate('window.__dailyFail = true; document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'enter before stale fixture');
    await evaluate('document.getElementById("stop-session").click()');
    await home();
    await evaluate(`window.__dailyFail = false; window.__dailyHold = true; window.__releaseDaily = null;
      window.__dailyOverride = ${JSON.stringify({ ...metadata, local_day: '1999-01-01', status: 'unavailable' })};
      document.getElementById('retry-daily').click()`);
    await until(() => evaluate('Boolean(window.__releaseDaily)'), 'old metadata held');
    await evaluate('window.__dailyHold = false; window.__dailyOverride = null; document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'ordinary action during stale metadata');
    await evaluate('document.getElementById("stop-session").click()');
    await home();
    const fresh = await evaluate('document.getElementById("daily-timezone").textContent');
    await evaluate('window.__releaseDaily()');
    await evaluate('new Promise(resolve => setTimeout(resolve, 100))');
    assert.equal(await evaluate('document.getElementById("daily-timezone").textContent'), fresh);
    assert(!fresh.includes('1999-01-01'), 'stale metadata replaced the new bootstrap');

    // SIMULATION: status copy separates an accepted attempt from completion.
    for (const [status, practiced, expected] of [
      ['in_progress', true, 'Практика начата'], ['completed', true, 'Задание дня завершено'],
      ['closed', true, 'Задание дня закрыто'], ['unavailable', false, 'Задание дня сейчас недоступно'],
    ]) {
      await evaluate(`window.__dailyOverride = ${JSON.stringify({ ...metadata, status, practiced, completed: status === 'completed', session_id: started.session_id })};
        window.__dailyFail = true; document.getElementById('quick-start').click()`);
      await until(() => evaluate('!document.getElementById("training").hidden'), 'enter for status fixture');
      await evaluate('document.getElementById("stop-session").click()');
      await home();
      await evaluate('window.__dailyFail = false; document.getElementById("retry-daily").click()');
      await home();
      assert((await evaluate('document.getElementById("daily-status").textContent')).includes(expected));
      assert.equal(await evaluate('document.getElementById("start-daily").hidden'), status !== 'in_progress');
      await shot(`daily-${status}-390.png`);
    }
    // SIMULATION: rollover metadata must not change a restored daily session's postpone action.
    await evaluate(`window.__dailyOverride = ${JSON.stringify({ ...metadata, local_day: '2099-01-02', session_id: null })};
      document.getElementById('quick-start').click()`);
    await until(() => evaluate('!document.getElementById("training").hidden'), 'restored daily opens after simulated rollover');
    assert.equal(await evaluate('document.getElementById("finish-session").textContent'), 'Отложить задание');
    await evaluate('document.getElementById("finish-session").click()');
    await home();
    assert.equal(await evaluate('window.__dailyCalls.some(call => call.route === "stop")'), false);

    // Exact session identity: choosing ordinary practice keeps its original finish behavior.
    const ordinary = await request('sessions', { recommended: true });
    assert.equal(ordinary.daily_day, '');
    await send('Page.navigate', { url: origin });
    await home();
    await evaluate('document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'ordinary session opened');
    assert.equal(await evaluate('document.getElementById("finish-session").textContent'), 'Завершить досрочно');
    await evaluate('document.getElementById("finish-session").click()');
    await home();
    assert.equal(await evaluate('window.__dailyCalls.filter(call => call.route === "stop").length'), 1);
  } finally {
    await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: injection.identifier });
  }
}

async function main() {
  assert(process.env.TEST_DATABASE_URL, 'Set TEST_DATABASE_URL to a disposable PostgreSQL database.');
  const artifacts = await mkdtemp(path.join(tmpdir(), 'cpp-recall-daily-ui-'));
  const freePort = async () => {
    const server = createServer();
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const port = server.address().port;
    await new Promise(resolve => server.close(resolve));
    return port;
  };
  const token = '000000000:local-daily-ui-test-token';
  const userId = randomInt(800000000, 900000000);
  const signedInitData = id => {
    const fields = { auth_date: String(Math.floor(Date.now() / 1000)), user: JSON.stringify({ id, first_name: 'Проверка' }) };
    const check = Object.entries(fields).sort(([a], [b]) => a.localeCompare(b)).map(([key, value]) => `${key}=${value}`).join('\n');
    const secret = createHmac('sha256', 'WebAppData').update(token).digest();
    return new URLSearchParams({ ...fields, hash: createHmac('sha256', secret).update(check).digest('hex') }).toString();
  };
  const until = async (check, description) => {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
      try { if (await check()) return; } catch { /* startup or rendering */ }
      await new Promise(resolve => setTimeout(resolve, 80));
    }
    throw new Error(`Timed out: ${description}`);
  };
  const appPort = await freePort(), debugPort = await freePort();
  const origin = `http://127.0.0.1:${appPort}/`;
  let log = '', browser, ws;
  const app = spawn('./build/cc_recall_bot', [], { env: { PATH: process.env.PATH,
    TELEGRAM_BOT_TOKEN: token, DATABASE_URL: process.env.TEST_DATABASE_URL,
    MINI_APP_URL: 'https://localhost/', MINI_APP_LISTEN_ADDRESS: '127.0.0.1', MINI_APP_PORT: String(appPort),
    MINI_APP_STATIC_DIR: 'docs/miniapp', HTTPS_PROXY: 'http://127.0.0.1:9', HTTP_PROXY: 'http://127.0.0.1:9',
    ALL_PROXY: 'http://127.0.0.1:9', NO_PROXY: '127.0.0.1,localhost' }, stdio: ['ignore', 'pipe', 'pipe'] });
  app.stdout.on('data', data => { log = (log + data).slice(-4000); });
  app.stderr.on('data', data => { log = (log + data).slice(-4000); });
  app.on('error', error => { log += error.message; });
  try {
    await until(async () => (await fetch(origin)).ok, 'local app');
    browser = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--no-sandbox', '--disable-dev-shm-usage',
      '--disable-background-networking', '--no-first-run', '--no-default-browser-check',
      '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost',
      `--remote-debugging-port=${debugPort}`, `--user-data-dir=${path.join(artifacts, 'chromium')}`, 'about:blank'], { stdio: 'ignore' });
    const debuggerOrigin = `http://127.0.0.1:${debugPort}`;
    await until(async () => (await fetch(`${debuggerOrigin}/json/version`)).ok, 'Chromium');
    const target = await (await fetch(`${debuggerOrigin}/json/new?about:blank`, { method: 'PUT' })).json();
    ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { ws.addEventListener('open', resolve, { once: true }); ws.addEventListener('error', reject, { once: true }); });
    let sequence = 0;
    const pending = new Map(), errors = [];
    ws.addEventListener('message', ({ data }) => {
      const message = JSON.parse(data);
      if (message.id) {
        const call = pending.get(message.id);
        if (!call) return;
        pending.delete(message.id); clearTimeout(call.timer);
        if (message.error) call.reject(new Error(JSON.stringify(message.error))); else call.resolve(message.result);
      } else if (message.method === 'Runtime.exceptionThrown') errors.push(message.params.exceptionDetails);
    });
    const send = (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++sequence;
      const timer = setTimeout(() => { pending.delete(id); reject(new Error(`CDP timeout: ${method}`)); }, 15000);
      pending.set(id, { resolve, reject, timer }); ws.send(JSON.stringify({ id, method, params }));
    });
    const evaluate = async expression => {
      const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
      return result.result.value;
    };
    await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable');
    await send('Network.setBlockedURLs', { urls: ['https://telegram.org/*'] });
    await checkDaily({ send, evaluate, until, origin, signedInitData, userId, artifacts });
    assert.deepEqual(errors, [], 'browser runtime errors');
    console.log(`PASS daily browser checks; screenshots: ${artifacts}`);
  } catch (error) {
    console.error(`Daily browser checks failed. Artifacts: ${artifacts}\n${log}`);
    throw error;
  } finally { ws?.close(); browser?.kill('SIGTERM'); app.kill('SIGTERM'); }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) await main();
