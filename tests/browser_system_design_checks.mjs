// Real local owner-only API and browser; all users/data are synthetic in disposable PostgreSQL.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function checkSystemDesignBrowser({send,evaluate,until,origin,signedInitData,ownerId,artifacts}) {
  const api=async(route,body={},uid=ownerId)=>{
    const response=await fetch(new URL(`api/v1/${route}`,origin),{method:'POST',headers:{
      Authorization:`tma ${signedInitData(uid)}`,'Content-Type':'application/json'},body:JSON.stringify(body)});
    assert.equal(response.status,200,await response.clone().text());return response.json();
  };
  const sql=query=>execFileSync('psql',[process.env.TEST_DATABASE_URL,'-XAt','-v','ON_ERROR_STOP=1','-c',query],{encoding:'utf8'}).trim();
  const snapshot=()=>['users','sessions','attempts','question_progress','topic_progress','skill_progress','skill_evidence',
    'user_skill_state','xp_ledger','active_training_sessions','saved_training_sessions','daily_practice_assignments','user_daily_settings']
    .map(table=>sql(`SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]')::text FROM ${table} t WHERE user_id=${ownerId}`)).join('\n');
  const injections=[];
  async function identify(uid) {
    const item=await send('Page.addScriptToEvaluateOnNewDocument',{source:`window.Telegram={WebApp:{
      initData:${JSON.stringify(signedInitData(uid))},colorScheme:'dark',ready(){},expand(){},
      onEvent(name,cb){if(name==='themeChanged')window.__sdTheme=cb;},setHeaderColor(){},setBackgroundColor(){},
      BackButton:{show(){},hide(){},onClick(){}}}};`});
    injections.push(item.identifier);
  }
  const idle=()=>until(()=>evaluate('!document.body.classList.contains("busy")'),'SD UI idle');
  const click=async id=>{await idle();await evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);};
  const switchTo=async id=>{
    await idle();await evaluate(`{
      const select=document.getElementById('education-direction');select.value=${JSON.stringify(id)};
      select.dispatchEvent(new Event('change',{bubbles:true}));
    }`);
  };
  const enterText=async text=>{
    await idle();await evaluate(`{
      const input=document.getElementById('sd-draft'); input.value=${JSON.stringify(text)};
      input.dispatchEvent(new Event('input',{bubbles:true}));
    }`);
  };
  const capture=async name=>{
    assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true,`overflow: ${name}`);
    const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
    await writeFile(path.join(artifacts,name),Buffer.from(shot.data,'base64'));
  };
  try {
    await send('Emulation.setDeviceMetricsOverride',{width:390,height:900,deviceScaleFactor:1,mobile:true});
    await identify(ownerId+800);
    await send('Page.navigate',{url:new URL('?direction=system_design',origin).href});
    await until(()=>evaluate('!document.getElementById("home").hidden'),'ordinary user home');
    assert.equal(await evaluate('document.getElementById("direction-picker").hidden'),true);
    assert.equal(await evaluate('document.getElementById("system-design").hidden'),true);
    await api('bootstrap');
    sql(`INSERT INTO user_skill_state(user_id,skill_id,status) VALUES(${ownerId},'core.storage_duration','demonstrated')
      ON CONFLICT(user_id,skill_id) DO UPDATE SET status='demonstrated';
      INSERT INTO skill_progress(user_id,skill_id,question_id) VALUES(${ownerId},'core.storage_duration','sd01-fixture') ON CONFLICT DO NOTHING;`);
    const cpp=await api('sessions',{exercise_generator:'vector_size_v1',request_id:randomUUID()});
    await identify(ownerId);
    await send('Page.navigate',{url:origin});
    await until(()=>evaluate('!document.getElementById("home").hidden'),'owner C++ home');
    await click('quick-start');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'C++ numeric question');
    await evaluate('window.__cppInput=document.getElementById("short-answer");window.__cppInput.value="17"');
    await idle();
    const before=snapshot();
    await switchTo('system_design');
    await until(()=>evaluate('Boolean(document.getElementById("sd-open-module"))'),'SD home');
    assert.match(await evaluate('document.getElementById("direction-footer").textContent'),/самопроверки/);
    await capture('sd-home-dark-390.png');
    await click('sd-open-module');
    await until(()=>evaluate('Boolean(document.getElementById("sd-complete-module"))'),'SD introductory module');
    await capture('sd-module-dark-390.png');
    await click('sd-complete-module');
    await until(()=>evaluate('Boolean(document.getElementById("sd-start"))'),'SD case entry');
    await click('sd-start');
    await until(()=>evaluate('Boolean(document.getElementById("sd-draft"))'),'SD first stage');
    assert.equal(await evaluate('document.getElementById("sd-draft").value'),'');
    assert.equal(await evaluate('document.getElementById("sd-advance").getBoundingClientRect().height>=44'),true);
    await click('sd-hint');
    await until(()=>evaluate('document.querySelectorAll("#system-design .sd-hints p").length===1'),'first hint');
    const own='Мои вопросы: кто создаёт ссылку?\nСвой подход <script>window.sdXss=true</script>';
    await enterText(own);await click('sd-save');
    await until(async()=>((await api('system-design/current')).session?.stage.draft)===own,'server draft saved');

    // A clean tab must not delete another tab's unsaved recovery copy on pagehide.
    const active=(await api('system-design/current')).session;
    const key=`cpp-recall-sd-draft-v1:${ownerId}:${active.session_id}:${active.content_revision}:${active.stage.id}`;
    const externalRaw=JSON.stringify({version:active.version,text:'Черновик другого окна — не удалять'});
    await evaluate(`localStorage.setItem(${JSON.stringify(key)},${JSON.stringify(externalRaw)});window.dispatchEvent(new Event('pagehide'));`);
    assert.equal(await evaluate(`localStorage.getItem(${JSON.stringify(key)})`),externalRaw,'clean tab deleted another tab draft');
    await evaluate(`localStorage.removeItem(${JSON.stringify(key)})`);

    await switchTo('cpp');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'same C++ screen');
    assert.equal(await evaluate('document.getElementById("short-answer")===window.__cppInput'),true,'C++ DOM recreated');
    assert.equal(await evaluate('document.getElementById("short-answer").value'),'17');
    assert.match(await evaluate('document.getElementById("direction-footer").textContent'),/^Сервер проверяет ответы/);
    assert.equal((await api('session')).session_id,cpp.session_id);
    await switchTo('system_design');
    await until(()=>evaluate('!document.getElementById("system-design").hidden'),'SD return');
    assert.equal(await evaluate('document.getElementById("sd-draft").value'),own);
    await send('Page.reload');
    await until(()=>evaluate('Boolean(document.getElementById("sd-draft"))&&!document.getElementById("system-design").hidden'),'SD preference/draft restoration');
    assert.equal(await evaluate('document.getElementById("sd-draft").value'),own);
    assert.equal(await evaluate('document.querySelectorAll("#system-design .sd-hints p").length'),1);
    assert.equal(await evaluate('Boolean(window.sdXss)'),false);
    await switchTo('cpp');
    await until(()=>evaluate('!document.getElementById("home").hidden'),'C++ restored home after reload');
    await click('quick-start');
    await until(()=>evaluate('!document.getElementById("training").hidden'),'C++ question restored after reload');
    assert.equal(await evaluate('document.getElementById("short-answer").value'),'17','unsubmitted C++ draft lost on reload');
    await switchTo('system_design');await idle();

    for(const theme of ['dark','light']) {
      await evaluate(`{
        const colors=${JSON.stringify({dark:['#17212b','#232e3c','#f5f5f5'],light:['#ffffff','#f0f2f5','#1f2937']})}[${JSON.stringify(theme)}];
        ['bg-color','secondary-bg-color','text-color'].forEach((name,i)=>document.documentElement.style.setProperty('--tg-theme-'+name,colors[i]));
        Telegram.WebApp.colorScheme=${JSON.stringify(theme)};window.__sdTheme();
      }`);
      for(const width of [390,768,1440]) {
        await send('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:width<600});
        await capture(`sd-stage-${theme}-${width}.png`);
      }
    }
    await send('Emulation.setDeviceMetricsOverride',{width:390,height:1000,deviceScaleFactor:1,mobile:true});
    await evaluate(`window.__sdActivity=0;window.__sdFetch=window.fetch.bind(window);window.fetch=async(url,options)=>{
      if(String(url).includes('/activity')&&!document.getElementById('system-design').hidden)window.__sdActivity++;
      const response=await window.__sdFetch(url,options);
      if(String(url).includes('/system-design/save')&&!window.__sdDropped){window.__sdDropped=true;throw new TypeError('simulated lost save response');}
      return response;
    };`);
    const old=(await api('system-design/current')).session;
    await enterText('Уточняю собственные требования: жизненный цикл и доступ.');
    await click('sd-save');
    await until(()=>evaluate('Boolean(document.getElementById("sd-retry-command"))'),'lost response recovery');
    await capture('sd-retry-390.png');
    await click('sd-retry-command');
    await until(()=>evaluate('!document.getElementById("sd-retry-command")'),'save retry accepted');
    const recovered=(await api('system-design/current')).session;
    assert.equal(recovered.version,old.version+1,'save retry applied twice');

    // Explicit stale-save conflict: neither unsent text nor the other writer is silently overwritten.
    await enterText('Мой несохранённый текст этого окна');
    await api('system-design/save',{session_id:recovered.session_id,version:recovered.version,
      stage_id:recovered.stage.id,request_id:randomUUID(),text:'Сохранённая версия другого окна'});
    await click('sd-save');
    await until(()=>evaluate('document.getElementById("sd-storage").textContent.includes("другом окне")'),'stale version notice');
    assert.equal(await evaluate('document.getElementById("sd-draft").value'),'Мой несохранённый текст этого окна');
    // Another tab also changes its recovery copy. Navigation must not replace
    // this tab's distinct text with that foreign local copy.
    await evaluate(`localStorage.setItem(${JSON.stringify(key)},JSON.stringify({version:${recovered.version+1},text:'Чужой локальный черновик'}))`);
    await enterText('Мой несохранённый текст этого окна');
    await evaluate(`Array.from(document.querySelectorAll('#system-design button')).find(button=>button.textContent==='К направлению').click()`);
    await idle();
    assert.equal(await evaluate('document.getElementById("sd-draft")?.value'),'Мой несохранённый текст этого окна','conflict navigation lost draft');
    await capture('sd-conflict-390.png');
    await evaluate(`Array.from(document.querySelectorAll('#system-design button')).find(button=>button.textContent==='Загрузить сохранённое занятие').click()`);
    await until(()=>evaluate('document.getElementById("sd-draft")?.value==="Сохранённая версия другого окна"'),'explicit server recovery');
    for(let i=0;i<12;i++) {
      await enterText(`Свой план этапа ${i}: требования, допущения, аргументы и компромиссы.`);
      await click('sd-advance');
      await until(()=>evaluate(i===11
        ? 'document.querySelector("#system-design h1")?.textContent==="Разбор кейса"'
        : `document.querySelector('#system-design .muted')?.textContent.includes('этап ${i+2} из')`),'next SD stage');
    }
    assert.match(await evaluate('document.getElementById("system-design").textContent'),/не автоматическая оценка/);
    await capture('sd-review-390.png');
    await idle();await evaluate(`{
      const select=document.querySelector('#system-design .sd-rubric-label select');select.value='covered';select.dispatchEvent(new Event('change',{bubbles:true}));
    }`);
    await until(()=>evaluate('document.querySelector("#system-design .sd-rubric-label select")?.value==="covered"&&!document.body.classList.contains("busy")'),'self-review mark');
    await send('Page.reload');
    await until(()=>evaluate('Boolean(document.querySelector("#system-design .sd-history-row"))'),'completed history restoration');
    await evaluate(`Array.from(document.querySelectorAll('#system-design button')).find(button=>button.textContent==='Открыть разбор').click()`);
    await until(()=>evaluate('document.querySelector("#system-design .sd-rubric-label select")?.value==="covered"'),'saved self review');
    await evaluate(`window.__sdActivity=0;window.__sdActivityFetch=window.fetch.bind(window);window.fetch=(url,options)=>{
      if(String(url).includes('/activity')&&!document.getElementById('system-design').hidden)window.__sdActivity++;
      return window.__sdActivityFetch(url,options);
    };`);
    await new Promise(resolve=>setTimeout(resolve,5500));
    assert.equal(await evaluate('window.__sdActivity||0'),0,'SD emitted C++ activity pulses');
    assert.equal(snapshot(),before,'SD browser workflow changed C++ scored progress/session data');
    assert.equal((await api('session')).session_id,cpp.session_id);
    console.log('SD-01 browser PASS: owner gate, independent C++ DOM/draft, SD reload/hints, idempotent retry/conflict, twelve stages and durable self-review');
  } finally {
    for(const identifier of injections)await send('Page.removeScriptToEvaluateOnNewDocument',{identifier});
  }
}
