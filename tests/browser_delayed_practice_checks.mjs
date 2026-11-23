// UI SIMULATION in the existing isolated browser harness; no real learner data.
import assert from 'node:assert/strict';
import {writeFile} from 'node:fs/promises';
import path from 'node:path';

export async function checkDelayedPractice({send, evaluate, until, origin, artifacts}) {
  const injected = await send('Page.addScriptToEvaluateOnNewDocument', {source: `
    window.__delayedAttempts = [];
    const originalDelayedFetch = window.fetch;
    window.fetch = async (...args) => {
      if (new URL(String(args[0])).pathname.endsWith('/sessions')) {
        window.__delayedAttempts.push(JSON.parse(args[1].body));
        return new Response(JSON.stringify({message:'Не удалось начать проверку. Попробуй ещё раз.'}), {status:503});
      }
      return originalDelayedFetch(...args);
    };
  `});
  try {
    await send('Page.navigate', {url: `${origin}?user=108`});
    await until(() => evaluate('!document.getElementById("home").hidden'), 'delayed practice home');
    assert.equal(await evaluate('document.getElementById("delayed-practice").hidden'), false);
    assert.equal(await evaluate('window.__delayedAttempts.length'), 0, 'preview started a control');
    for (const width of [390, 768, 1440]) {
      await send('Emulation.setDeviceMetricsOverride', {width, height:1000, deviceScaleFactor:1, mobile:width===390});
      await evaluate('document.getElementById("delayed-practice").scrollIntoView({block:"center"})');
      assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `overflow at ${width}`);
      assert.equal(await evaluate(`(() => {
        const saved=document.getElementById('saved-trainings').getBoundingClientRect();
        const panel=document.getElementById('delayed-practice').getBoundingClientRect();
        return panel.top-saved.bottom>=12;
      })()`), true, `optional check touches saved trainings at ${width}`);
      assert.equal(await evaluate(`(() => {
        const panel=document.getElementById('delayed-practice').getBoundingClientRect();
        const button=document.getElementById('start-delayed-practice').getBoundingClientRect();
        return button.height>=44 && button.left>=panel.left && button.right<=panel.right;
      })()`), true, `clipped or undersized control at ${width}`);
      const image = await send('Page.captureScreenshot', {format:'png'});
      await writeFile(path.join(artifacts, `delayed-ready-${width}.png`), Buffer.from(image.data, 'base64'));
    }
    await evaluate('document.getElementById("start-delayed-practice").click()');
    await until(() => evaluate('document.getElementById("delayed-practice-error").textContent.length>0'), 'launch error');
    assert.equal(await evaluate('document.getElementById("home").hidden'), false);
    assert.equal(await evaluate('document.getElementById("quick-start").disabled'), false);
    const key = await evaluate('window.__delayedAttempts[0].request_id');
    await evaluate('document.getElementById("start-delayed-practice").click()');
    await until(() => evaluate('window.__delayedAttempts.length===2'), 'retry');
    assert.equal(await evaluate('window.__delayedAttempts[1].request_id'), key, 'retry discarded idempotency');
    await send('Page.navigate', {url:`${origin}?user=109`});
    await until(() => evaluate('!document.getElementById("home").hidden'), 'resume home');
    assert.equal(await evaluate('document.getElementById("start-delayed-practice").textContent'), 'Продолжить проверку');
    assert.equal(await evaluate('document.getElementById("delayed-practice-title").textContent'), 'Повторная проверка');
  } finally {
    await send('Page.removeScriptToEvaluateOnNewDocument', {identifier:injected.identifier});
  }
}
