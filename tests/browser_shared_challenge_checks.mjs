// Scripted local browser scenarios; no real Telegram users or messages.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function checkSharedChallenges({ send, evaluate, until, origin, signedInitData, userId, artifacts }) {
  const request = async (uid, route, body = {}) => {
    const response = await fetch(new URL(`api/v1/${route}`, origin), {
      method: 'POST', headers: { Authorization: `tma ${signedInitData(uid)}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    assert.equal(response.status, 200, await response.clone().text());
    return response.json();
  };
  let injection;
  const open = async (uid, start, extraQuery = '') => {
    if (injection) await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: injection.identifier });
    injection = await send('Page.addScriptToEvaluateOnNewDocument', { source: `
      window.Telegram = { WebApp: { initData: ${JSON.stringify(signedInitData(uid, start))},
        initDataUnsafe: {start_param: 'review_213_v1'}, colorScheme: 'dark',
        onEvent(name, callback) { if (name === 'themeChanged') window.__challengeThemeChanged = callback; },
        ready() {}, expand() {}, setHeaderColor() {}, setBackgroundColor() {},
        enableClosingConfirmation() {}, disableClosingConfirmation() {},
        BackButton: {show() {}, hide() {}, onClick() {}},
        openTelegramLink(url) { window.__sharedTelegramUrl = url; },
        showConfirm(message, callback) { callback(true); } } };
    ` });
    await send('Page.navigate', { url: origin + extraQuery });
    await until(() => evaluate('document.getElementById("loading").hidden'), 'shared entry loaded');
  };
  const shot = async name => {
    assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `overflow: ${name}`);
    const image = await send('Page.captureScreenshot', { format: 'png' });
    await writeFile(path.join(artifacts, name), Buffer.from(image.data, 'base64'));
  };
  const theme = async value => {
    await evaluate(`{
      const colors = ${JSON.stringify({
        dark: { bg: '#17212b', secondaryBg: '#232e3c', text: '#f5f5f5', link: '#73a9ff', button: '#356dcc' },
        light: { bg: '#ffffff', secondaryBg: '#f0f2f5', text: '#1f2937', link: '#245ea8', button: '#2869c9' },
      })}[${JSON.stringify(value)}];
      for (const [name, color] of Object.entries({ 'bg-color': colors.bg,
        'secondary-bg-color': colors.secondaryBg, 'text-color': colors.text,
        'link-color': colors.link, 'button-color': colors.button }))
        document.documentElement.style.setProperty('--tg-theme-' + name, color);
      Telegram.WebApp.colorScheme = ${JSON.stringify(value)};
      window.__challengeThemeChanged();
    }`);
    assert.equal(await evaluate('getComputedStyle(document.body).backgroundColor'),
      value === 'light' ? 'rgb(255, 255, 255)' : 'rgb(23, 33, 43)', 'Telegram palette was not applied');
    const contrast = await evaluate(`(() => {
      const ink = getComputedStyle(document.getElementById('share-question')).color;
      const ground = getComputedStyle(document.querySelector('#training .question-card')).backgroundColor;
      const luminance = color => color.match(/[\\d.]+/g).slice(0, 3).map(Number)
        .map(c => c / 255).map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4)
        .reduce((sum, c, i) => sum + c * [.2126, .7152, .0722][i], 0);
      const a = luminance(ink), b = luminance(ground);
      return (Math.max(a, b) + .05) / (Math.min(a, b) + .05);
    })()`);
    assert(contrast >= 4.5, `${value} share label contrast: ${contrast}`);
  };
  await request(userId, 'bootstrap');
  const old = await request(userId, 'sessions', { route_id: 'containers_iterators' });
  const q = old.question;
  await request(userId, 'session/reference', { question_id: q.id, reference_id: q.references[0].id });
  await open(userId, 'review_212_v1', '?tgWebAppStartParam=review_213_v1');
  assert.equal(await evaluate('document.getElementById("shared-challenge").hidden'), false);
  assert.match(await evaluate('document.getElementById("challenge-meta").textContent'), /CR-000212/);
  assert.equal(await evaluate('document.getElementById("challenge-preserved").hidden'), false);
  assert.equal((await request(userId, 'bootstrap')).session.session_id, old.session_id);
  for (const width of [390, 768, 1440]) {
    await send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 600 });
    const box = await evaluate('document.getElementById("start-challenge").getBoundingClientRect().toJSON()');
    assert(box.height >= 44 && box.bottom < 900, 'shared entry action is buried or too small');
    await shot(`challenge-entry-${width}.png`);
  }
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 900, deviceScaleFactor: 1, mobile: true });
  await evaluate('document.getElementById("start-challenge").focus()');
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  await until(() => evaluate('!document.getElementById("training").hidden'), 'challenge started with keyboard');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000212');
  assert.equal(await evaluate('document.getElementById("share-question").hidden'), false);
  await evaluate('document.querySelector("#answer-area input[value=b]").checked = true; document.getElementById("share-question").click()');
  await until(() => evaluate('Boolean(window.__sharedTelegramUrl)'), 'Telegram share chooser');
  const sharedUrl = new URL(await evaluate('window.__sharedTelegramUrl'));
  assert.equal(sharedUrl.origin + sharedUrl.pathname, 'https://t.me/share/url');
  assert.equal(sharedUrl.searchParams.get('url'), 'https://t.me/cpp_recall_bot?startapp=review_212_v1');
  assert.match(sharedUrl.searchParams.get('text'), /int Read/);
  assert(!sharedUrl.searchParams.get('text').includes(String(userId)), 'sharing leaked a sender ID');
  assert(!sharedUrl.searchParams.get('text').includes('correct_option_id'), 'sharing leaked an answer key');
  assert.equal(await evaluate('document.querySelector("#answer-area input:checked").value'), 'b', 'sharing erased the draft answer');
  await evaluate(`{
    const original = window.fetch;
    let fail = true;
    window.fetch = async (...args) => {
      if (String(args[0]).endsWith('/session/share') && fail) {
        fail = false;
        return new Response(JSON.stringify({message: 'Не удалось подготовить ссылку. Попробуй ещё раз.'}),
          {status: 503, headers: {'Content-Type': 'application/json'}});
      }
      return original(...args);
    };
    document.getElementById('share-question').click();
  }`);
  await until(() => evaluate('document.getElementById("share-question-status").classList.contains("error-text")'), 'share retry error');
  await shot('challenge-share-error-390.png');
  assert.equal(await evaluate('document.querySelector("#answer-area input:checked").value'), 'b');
  await theme('light');
  await shot('challenge-share-error-light-390.png');
  await theme('dark');
  await evaluate('window.__sharedTelegramUrl = null; document.getElementById("share-question").click()');
  await until(() => evaluate('Boolean(window.__sharedTelegramUrl)'), 'share retry succeeds');
  for (const width of [390, 768, 1440]) {
    await send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 600 });
    await shot(`challenge-question-${width}.png`);
  }
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 900, deviceScaleFactor: 1, mobile: true });
  await theme('light');
  await shot('challenge-question-light-390.png');
  await theme('dark');
  await evaluate('document.getElementById("submit-answer").click()');
  await until(() => evaluate('!document.getElementById("feedback").hidden'), 'wrong review feedback');
  assert.equal(await evaluate('document.getElementById("next-question").textContent'), 'Попробовать ещё раз');
  await evaluate('document.getElementById("next-question").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'review repair');
  await evaluate('document.querySelector("#answer-area input[value=a]").checked = true; document.getElementById("submit-answer").click()');
  await until(() => evaluate('!document.getElementById("feedback").hidden'), 'correct review feedback');
  assert.equal(await evaluate('document.getElementById("next-question").textContent'), 'Завершить задание');
  await evaluate(`{
    const original = window.fetch;
    let lose = true;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (String(args[0]).endsWith('/session/next') && lose && response.ok) {
        lose = false; throw new Error('Simulated lost challenge completion');
      }
      return response;
    };
    document.getElementById('next-question').click();
  }`);
  await until(() => evaluate('!document.getElementById("completed").hidden'), 'challenge completion recovery');
  assert.equal(await evaluate('document.getElementById("related-practice").hidden'), false);
  await shot('challenge-completed-390.png');
  const completed = await request(userId, 'bootstrap');
  assert(completed.saved_sessions.some(saved => saved.id === old.session_id), 'old training lost after shared completion');
  await evaluate('document.getElementById("related-practice").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'related normal practice');
  const related = (await request(userId, 'bootstrap')).session;
  assert.deepEqual(related.topics, ['containers']);
  assert.equal(related.shared_challenge_id, '');
  assert.notEqual(related.question.public_id, 'CR-000212');
  await request(userId, 'session/resume', { session_id: old.session_id });
  await open(userId, undefined);
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'old training resumed');
  assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), q.public_id);
  assert.equal(await evaluate('document.getElementById("reference-assisted").hidden'), false);

  // Missing native share method has a real user-clickable link, not a fake success.
  await request(userId, 'sessions', { challenge_id: 'review_213_v1' });
  await open(userId, undefined);
  await evaluate('document.getElementById("quick-start").click()');
  await until(() => evaluate('!document.getElementById("training").hidden'), 'fallback share task');
  await evaluate('delete Telegram.WebApp.openTelegramLink; document.getElementById("share-question").click()');
  await until(() => evaluate('Boolean(document.querySelector("#share-question-status a"))'), 'fallback share anchor');
  const fallback = await evaluate('document.querySelector("#share-question-status a").href');
  assert.equal(new URL(fallback).host, 't.me');
  await shot('challenge-share-fallback-390.png');

  await request(userId + 1, 'bootstrap');
  execFileSync('psql', [process.env.TEST_DATABASE_URL, '-X', '-v', 'ON_ERROR_STOP=1', '-c',
    `UPDATE users SET adaptive_level=1, unlocked_level=1 WHERE user_id=${userId + 1}`], { stdio: 'pipe' });
  await open(userId + 1, 'review_212_v1');
  assert.equal(await evaluate('document.getElementById("start-challenge").textContent'), 'Подобрать практику');
  await shot('challenge-locked-390.png');
  assert.equal((await request(userId + 1, 'bootstrap')).session, null, 'locked link started a training');
  await open(userId + 1, 'review_missing_v1');
  assert.equal(await evaluate('document.getElementById("start-challenge").hidden'), true);
  assert.equal(await evaluate('document.getElementById("challenge-title").textContent'), 'Задание недоступно');
  await shot('challenge-unavailable-390.png');
  await open(userId + 1, undefined, '?tgWebAppStartParam=review_212_v1');
  assert.equal(await evaluate('document.getElementById("home").hidden'), false, 'unsigned URL overrode the signed launch');
  await send('Page.removeScriptToEvaluateOnNewDocument', { identifier: injection.identifier });
}
