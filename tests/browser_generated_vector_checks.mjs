// Real local API and synthetic PostgreSQL user; no production credentials or user C++ execution.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function checkGeneratedVector({send, evaluate, until, origin, signedInitData, userId, artifacts}) {
  const api = async (route, body={}) => {
    const response = await fetch(new URL(`api/v1/${route}`,origin),{method:'POST',
      headers:{Authorization:`tma ${signedInitData(userId)}`,'Content-Type':'application/json'},body:JSON.stringify(body)});
    assert.equal(response.status,200,await response.clone().text()); return response.json();
  };
  const sql = query => execFileSync('psql',[process.env.TEST_DATABASE_URL,'-XAt','-v','ON_ERROR_STOP=1','-c',query],{encoding:'utf8'}).trim();
  await api('bootstrap');
  sql(`INSERT INTO skill_progress(user_id,skill_id,question_id) VALUES (${userId},'core.storage_duration','gen01-fixture');
       INSERT INTO user_skill_state(user_id,skill_id,status) VALUES (${userId},'core.storage_duration','demonstrated');`);
  const injection=await send('Page.addScriptToEvaluateOnNewDocument',{source:`
    window.Telegram={WebApp:{initData:${JSON.stringify(signedInitData(userId))},colorScheme:'dark',
      ready(){},expand(){},onEvent(name,cb){if(name==='themeChanged')window.__genTheme=cb;},setHeaderColor(){},setBackgroundColor(){},BackButton:{show(){},hide(){},onClick(){}}}};
  `});
  const home=async()=>{
    await send('Page.navigate',{url:origin});
    await until(()=>evaluate('!document.getElementById("home").hidden'),'generator home');
  };
  const answerFor=question=>{
    assert.match(question.content_revision,/^[a-f0-9]{64}$/);
    return sql(`SELECT content_snapshot #>> '{task,accepted_answers,0}' FROM question_content_revisions
      WHERE question_id='vector_reserve_does_not_change_size' AND content_revision='${question.content_revision}'`);
  };
  const capture=async name=>{
    assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true,'generated UI overflows');
    const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
    await writeFile(path.join(artifacts,name),Buffer.from(shot.data,'base64'));
  };
  try {
    await send('Emulation.setDeviceMetricsOverride',{width:390,height:900,deviceScaleFactor:1,mobile:true});
    await home();
    assert.equal(await evaluate('document.getElementById("start-vector-generator").disabled'),false);
    await evaluate('document.getElementById("vector-generator-entry").scrollIntoView({block:"start"})');
    await capture('generated-entry-390.png');
    await evaluate('document.getElementById("start-vector-generator").click()');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'generated first exercise');
    const first=(await api('bootstrap')).session;
    assert.equal(first.question.task.type,'short_answer');
    assert(!('accepted_answers' in first.question.task) && !('explanation' in first.question));
    assert.equal(await evaluate('document.getElementById("short-answer").inputMode'),'numeric');
    assert.equal(await evaluate('document.getElementById("short-answer").getBoundingClientRect().height>=44'),true);
    for(const theme of ['dark','light']) {
      await evaluate(`{
        const colors=${JSON.stringify({dark:['#17212b','#232e3c','#f5f5f5'],light:['#ffffff','#f0f2f5','#1f2937']})}[${JSON.stringify(theme)}];
        ['bg-color','secondary-bg-color','text-color'].forEach((key,i)=>document.documentElement.style.setProperty('--tg-theme-'+key,colors[i]));
        Telegram.WebApp.colorScheme=${JSON.stringify(theme)}; window.__genTheme();
      }`);
      for(const width of [390,768,1440]) {
        await send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:width<600});
        await capture(`generated-question-${theme}-${width}.png`);
      }
    }
    await send('Emulation.setDeviceMetricsOverride',{width:390,height:900,deviceScaleFactor:1,mobile:true});
    await home();
    await evaluate('document.getElementById("quick-start").click()');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'generated restore');
    assert.equal((await api('bootstrap')).session.question.code,first.question.code);
    await evaluate(`document.getElementById('short-answer').value=${JSON.stringify(answerFor(first.question))}; document.getElementById('submit-answer').click()`);
    await until(()=>evaluate('!document.getElementById("feedback").hidden'),'generated correct answer');
    assert.match(await evaluate('document.getElementById("explanation-text").textContent'),/Итоговый размер/);
    assert.equal(await evaluate('getComputedStyle(document.getElementById("explanation-text")).whiteSpace'),'pre-line');
    await capture('generated-result-390.png');
    await home();
    await evaluate('document.getElementById("start-vector-generator").click()');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'generated second exercise');
    const second=(await api('bootstrap')).session;
    assert.notEqual(second.question.code,first.question.code);
    assert.equal(second.question.question_family_id,first.question.question_family_id);
    await evaluate('document.getElementById("short-answer").value="999";document.getElementById("submit-answer").click()');
    await until(()=>evaluate('!document.getElementById("feedback").hidden'),'generated wrong answer');
    assert.equal(await evaluate('document.getElementById("explanation").hidden'),true);
    await api('session/next',{session_id:second.session_id});
    await home();
    await evaluate('document.getElementById("quick-start").click()');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'generated repair restore');
    assert.equal((await api('bootstrap')).session.question.code,second.question.code);
    await evaluate(`document.getElementById('short-answer').value=${JSON.stringify(answerFor(second.question))}; document.getElementById('submit-answer').click()`);
    await until(()=>evaluate('!document.getElementById("feedback").hidden'),'generated repair correct');
    const feedback=(await api('bootstrap')).session.feedback;
    assert(feedback.correct && feedback.completed);
    assert.equal(feedback.xp_gained,0);
    assert.equal(feedback.skill_mastery.successful_families,1);
    assert.equal(feedback.skill_mastery.current,'applying');
    await capture('generated-repair-390.png');
    console.log('GEN-01 browser PASS: real start button, two variants, numeric answer, reload, wrong/repair, same family/no extra XP');
  } finally {await send('Page.removeScriptToEvaluateOnNewDocument',{identifier:injection.identifier});}
}
