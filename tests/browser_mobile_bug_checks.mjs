// Real local API/DB and browser; only the final malformed/XSS payloads are SIMULATION.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function checkMobileBug({ send, evaluate, until, origin, signedInitData, userId, artifacts }) {
  const request = async (route, body = {}) => {
    const response = await fetch(new URL(`api/v1/${route}`, origin), { method: 'POST',
      headers: { Authorization: `tma ${signedInitData(userId)}`, 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    assert.equal(response.status, 200, await response.clone().text()); return response.json();
  };
  await request('bootstrap');
  execFileSync('psql', [process.env.TEST_DATABASE_URL, '-X', '-v', 'ON_ERROR_STOP=1', '-c',
    `UPDATE users SET adaptive_level=3, unlocked_level=3 WHERE user_id=${userId}`], {stdio:'pipe'});
  let session = await request('sessions', { modes:['review_code'], topics:['containers'], level:3 });
  // This selection contains three real debugging cards. Reach the new one without
  // a production-only selector, special API or dependence on randomized order.
  for (let i = 0; session.question.id !== 'vector_reserve_bug_line' && i < 3; i++) {
    const card = JSON.parse(await readFile(`content/questions/containers/${session.question.id}.json`, 'utf8'));
    await request('session/answer', {session_id:session.session_id, question_id:card.id, answer:card.task.correct_option_id});
    session = await request('session/next', {session_id:session.session_id});
  }
  assert.equal(session.question.id, 'vector_reserve_bug_line');
  assert(!('correct_option_id' in session.question.task));
  const injection = await send('Page.addScriptToEvaluateOnNewDocument', {source:`
    window.Telegram = {WebApp:{initData:${JSON.stringify(signedInitData(userId))},colorScheme:'dark',
      ready(){},expand(){},setHeaderColor(){},setBackgroundColor(){},
      onEvent(name, callback){if(name==='themeChanged') window.__bugTheme=callback;},
      BackButton:{show(){},hide(){},onClick(){}}, enableClosingConfirmation(){},disableClosingConfirmation(){}}};
    window.__bugAnswerCalls = [];
    const original = window.fetch;
    window.fetch = async (...args) => {
      if(String(args[0]).endsWith('/session/answer')) window.__bugAnswerCalls.push(JSON.parse(args[1].body));
      return original(...args);
    };
  `});
  const open = async () => {
    await send('Page.navigate', {url:origin});
    await until(() => evaluate('!document.getElementById("home").hidden'), 'bug home');
    await evaluate('document.getElementById("quick-start").click()');
    await until(() => evaluate('!document.getElementById("training").hidden'), 'bug question');
    assert.equal(await evaluate('document.getElementById("question-public-id").textContent'), 'CR-000214');
    assert.equal(await evaluate('window.__bugAnswerCalls.length'), 0, 'opening sent an answer');
  };
  const shot = async name => {
    assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true, `overflow: ${name}`);
    const image = await send('Page.captureScreenshot', {format:'png', captureBeyondViewport:true});
    await writeFile(path.join(artifacts, name), Buffer.from(image.data, 'base64'));
  };
  try {
    await open();
    assert.equal(await evaluate('document.getElementById("question-code").hidden'), true);
    assert.equal(await evaluate('document.querySelectorAll(".bug-code-choice").length'), 3);
    for (const theme of ['dark','light']) {
      await evaluate(`{
        const colors=${JSON.stringify({dark:['#17212b','#232e3c','#f5f5f5'],light:['#ffffff','#f0f2f5','#1f2937']})}[${JSON.stringify(theme)}];
        ['bg-color','secondary-bg-color','text-color'].forEach((key,i)=>document.documentElement.style.setProperty('--tg-theme-'+key,colors[i]));
        Telegram.WebApp.colorScheme=${JSON.stringify(theme)}; window.__bugTheme();
      }`);
      for (const width of [390,768,1440]) {
        await send('Emulation.setDeviceMetricsOverride', {width,height:1000,deviceScaleFactor:1,mobile:width<600});
        const rows = await evaluate(`Array.from(document.querySelectorAll('.bug-code-choice'),el=>({
          height:el.getBoundingClientRect().height, label:el.querySelector('input').getAttribute('aria-label')}))`);
        assert(rows.every(row=>row.height>=44 && /^Строка [456]:/.test(row.label)));
        await shot(`bug-${theme}-${width}.png`);
      }
    }
    await send('Emulation.setDeviceMetricsOverride', {width:390,height:1000,deviceScaleFactor:1,mobile:true});
    await evaluate('document.querySelector("input[value=line_4]").focus()');
    await send('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
    await send('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
    assert.equal(await evaluate('document.querySelector("input[name=answer]:checked").value'), 'line_5');
    assert.equal(await evaluate('window.__bugAnswerCalls.length'), 0, 'keyboard selection auto-submitted');
    await evaluate('document.getElementById("submit-answer").click()');
    await until(() => evaluate('!document.getElementById("feedback").hidden'), 'wrong line feedback');
    assert.equal(await evaluate('window.__bugAnswerCalls[0].answer'), 'line_5');
    assert.equal(await evaluate('document.getElementById("explanation").hidden'), true);
    // Other unfinished main cards must precede the repair tail.
    await request('session/next', {session_id:session.session_id});
    for (let i=0;i<3;i++) {
      const current = (await request('bootstrap')).session;
      if(current.question.id==='vector_reserve_bug_line') break;
      const card=JSON.parse(await readFile(`content/questions/containers/${current.question.id}.json`,'utf8'));
      await request('session/answer',{session_id:session.session_id,question_id:card.id,answer:card.task.correct_option_id});
      await request('session/next',{session_id:session.session_id});
    }
    await open(); // Restored repair does not send the previous choice again.
    const point = await evaluate(`(()=>{const r=document.querySelector('input[value=line_6]').closest('label').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()`);
    await send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{...point,id:1}]});
    await send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    assert.equal(await evaluate('document.querySelector("input[name=answer]:checked").value'),'line_6');
    assert.equal(await evaluate('window.__bugAnswerCalls.length'),0,'touch selection auto-submitted');
    await shot('bug-selected-390.png');
    await evaluate('document.getElementById("submit-answer").click()');
    await until(()=>evaluate('!document.getElementById("feedback").hidden'),'correct line feedback');
    assert.match(await evaluate('document.getElementById("result-title").textContent'),/Верно после/);
    assert.equal(await evaluate('document.getElementById("result-correct-answer").hidden'),true);
    assert.match(await evaluate('document.getElementById("explanation-text").textContent'),/reserve/);
    await shot('bug-result-390.png');
    // SIMULATION: malformed archived presentation falls back to ordinary choices;
    // code resembling HTML stays literal in the interactive line view.
    for (const variant of ['xss', 'malformed']) {
      const fixture = structuredClone(session);
      if (variant === 'xss') {
        const text = 'v[0] = 42; // <img src=x onerror="window.__bugExecuted=true">';
        fixture.question.code = fixture.question.code.replace('v[0] = 42;', text);
        fixture.question.task.options.find(option=>option.id==='line_6').text = `Строка 6: ${text}`;
      } else fixture.question.task.options[0].id = 'line_99999';
      const simulated = await send('Page.addScriptToEvaluateOnNewDocument', {source:`
        const originalBootstrap = window.fetch;
        window.fetch = async (...args) => {
          const response = await originalBootstrap(...args);
          if(!String(args[0]).endsWith('/bootstrap')) return response;
          const data = await response.json(); data.session = ${JSON.stringify(fixture)};
          return new Response(JSON.stringify(data), {status:200});
        };
      `});
      try {
        await open();
        assert.equal(await evaluate('window.__bugExecuted === true'),false);
        assert.equal(await evaluate('document.querySelectorAll(".bug-hunter img").length'),0);
        assert.equal(await evaluate('document.querySelectorAll(".bug-code-choice").length'),variant==='xss'?3:0);
        if(variant==='malformed') {
          assert.equal(await evaluate('document.getElementById("question-code").hidden'),false);
          assert.equal(await evaluate('document.querySelectorAll(".answer-option input[name=answer]").length'),3);
        }
        await shot(`bug-${variant}-390.png`);
      } finally { await send('Page.removeScriptToEvaluateOnNewDocument',{identifier:simulated.identifier}); }
    }
  } finally { await send('Page.removeScriptToEvaluateOnNewDocument',{identifier:injection.identifier}); }
}
