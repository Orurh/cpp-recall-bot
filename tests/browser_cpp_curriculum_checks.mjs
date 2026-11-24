// UI-only simulation. Real permissions, snapshots and learning accounting are
// exercised by cpp_curriculum_service_checks.cc against a disposable database.
import assert from 'node:assert/strict';
import {writeFile} from 'node:fs/promises';
import path from 'node:path';

let failedOnce = false;
export function curriculumResponse(body, user) {
  if (user === 112 && !failedOnce) { failedOnce = true; return {status: 503, data: {message:'Временная ошибка проверки'}}; }
  if (body.unit_id === 'cpp.containers.map_lookup.l3') return {data: {blocked_session_id:88, message:'Продолжи незавершённую тренировку; справка внутри карточки учтёт помощь.'}};
  if (body.unit_id) {
    const entry={id:'demo', title:'Демонстрационный материал', principle:'Сначала проверь контракт операции.',
      mechanism:'Это синтетические данные проверки интерфейса, не учебная статья.', example:'auto found = values.find(key);\n// ' + 'long_line_'.repeat(40),
      common_mistake:'Не проверять результат поиска.', sources:['https://example.invalid/standard']};
    return {data: {theories:[entry], references:[{...entry,id:'demo.reference'}]}};
  }
  const topic = body.topic || 'containers';
  const unit = skill => ({id:`cpp.containers.${skill}.l3`, title:skill === 'vector' ? 'std::vector' : 'Словари',
    mastery:'applying', prerequisites:[{id:'core.storage_duration',title:'Время хранения'}],
    questions:[{public_id:skill==='vector'?'CR-DEMO-001':'CR-DEMO-002',completed:skill==='vector',due:skill==='vector'}], theory_ids:['demo'], reference_ids:['demo.reference'], can_practice:true});
  return {data:{direction:'cpp',topic,pilot:topic === 'containers',working_level:3,unlocked_level:3,
    passing_rule:'Три разных самостоятельных проверки, включая применение. Повторение учитывается отдельно.',
    topics:[{id:'containers',title:'Контейнеры'},{id:'templates',title:'Шаблоны'}],
    levels:Array.from({length:10},(_,index) => ({level:index+1,title:index < 2 ? 'Основные операции' : 'Применение и контракты',
      unlocked:index<3,count:topic==='containers' && index===2 ? 2 : 0,completed:topic==='containers'&&index===2?1:0,due:topic==='containers'&&index===2?1:0,
      goal:'Проверить конкретный контракт по короткому примеру.',content_ready:index===2 && topic==='containers',
      units:topic==='containers' && index===2 ? [unit('vector'),unit('map_lookup')] : []}))}};
}

export async function checkCppCurriculum({send,evaluate,until,origin,artifacts}) {
  const measurements=[];
  const setTheme = async theme => evaluate(`{
    const colors = ${JSON.stringify({dark:{bg:'#17212b',surface:'#232e3c',text:'#f5f5f5',link:'#73a9ff'},light:{bg:'#ffffff',surface:'#f0f2f5',text:'#1f2937',link:'#245ea8'}})}[${JSON.stringify(theme)}];
    for (const [name,value] of Object.entries({'bg-color':colors.bg,'secondary-bg-color':colors.surface,'text-color':colors.text,'link-color':colors.link})) document.documentElement.style.setProperty('--tg-theme-'+name,value);
    window.Telegram.WebApp.colorScheme=${JSON.stringify(theme)};window.__codingTheme();
  }`);
  const capture = async (name,width,theme) => {
    await setTheme(theme);
    await send('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:width<600});
    await evaluate('window.scrollTo(0,0)');
    await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))');
    const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
    await writeFile(path.join(artifacts,`${name}-${width}-${theme}.png`),Buffer.from(shot.data,'base64'));
    const controls=await evaluate(`Array.from(document.querySelectorAll('#curriculum button,#curriculum select,#curriculum summary,#curriculum a')).filter(el=>el.getClientRects().length).map(el=>({tag:el.tagName,id:el.id,text:el.textContent,height:el.getBoundingClientRect().height,width:el.getBoundingClientRect().width}))`);
    measurements.push({name,width,theme,controls});
    await writeFile(path.join(artifacts,'curriculum-measurements.json'),JSON.stringify(measurements,null,2));
    assert(controls.every(item=>item.height>=44 && item.width>=44),`${name} touch targets ${width}/${theme}: ${JSON.stringify(controls)}`);
    const actions=await evaluate(`Array.from(document.querySelectorAll('#curriculum .curriculum-actions')).filter(el=>el.getClientRects().length).map(el=>Array.from(el.querySelectorAll('button')).map(button=>{const r=button.getBoundingClientRect();return {left:r.left,top:r.top,bottom:r.bottom};}))`);
    for (const [material,practice] of actions) {
      if (Math.abs(material.left-practice.left)<1) assert(Math.abs(practice.top-material.bottom-12)<1,'stacked curriculum actions must keep their12px gap');
      else assert(Math.abs(material.top-practice.top)<1,'curriculum action row must align');
    }
  };
  await send('Page.navigate',{url:origin+'?user=101'});
  await until(() => evaluate('!document.getElementById("home").hidden'),'curriculum home');
  // The learning chart must stay on the home screen after UX/navigation changes.
  assert.equal(await evaluate('Boolean(document.getElementById("skill-map-chart") && document.getElementById("skill-map-legend"))'),true,'skill chart or legend missing');
  assert.equal(await evaluate('document.querySelectorAll("#main-tabs .tab-button:not([hidden])").length'),3,'learner navigation must expose three tabs');
  await evaluate('document.querySelector("#main-tabs [data-tab=curriculum]").click()');
  await until(() => evaluate('!document.getElementById("curriculum").hidden && document.querySelectorAll(".curriculum-level").length===10'),'catalog bottom-tab navigation');
  assert.equal(await evaluate('document.querySelector("#main-tabs [data-tab=curriculum]").getAttribute("aria-current")'),'page','catalog tab is not marked current');
  await evaluate('document.querySelector("#main-tabs [data-tab=home]").click()');
  await until(() => evaluate('!document.getElementById("home").hidden'),'return home from bottom navigation');
  await evaluate('document.getElementById("open-coding").click()');
  await until(() => evaluate('!document.getElementById("coding").hidden'),'coding draft before curriculum');
  const draft = await evaluate('document.getElementById("coding-draft").value');
  await evaluate('document.querySelector("[data-tab=home]").click(); document.getElementById("open-curriculum").click()');
  await until(() => evaluate('document.querySelectorAll(".curriculum-level").length===10'),'curriculum levels');
  assert.equal(await evaluate('document.querySelectorAll(".curriculum-unit").length'),2);
  assert.equal(await evaluate('document.getElementById("curriculum-status").textContent.includes("Рабочая сложность темы: L3")'),true);
  for (const theme of ['dark','light']) {
    await setTheme(theme);
    assert.equal(await evaluate('getComputedStyle(document.body).backgroundColor'),theme==='dark'?'rgb(23, 33, 43)':'rgb(255, 255, 255)','synthetic Telegram theme must actually change colors');
    for (const width of [390,768,1440]) {
      await send('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:width<600});
      await evaluate('window.scrollTo(0,0)');
      const geometry = await evaluate(`(() => ({overflow:document.documentElement.scrollWidth>innerWidth,
        controls:[...document.querySelectorAll('#curriculum button:not([hidden]),#curriculum select')].filter(el=>el.getClientRects().length).map(el=>({id:el.id,text:el.textContent,h:el.getBoundingClientRect().height,w:el.getBoundingClientRect().width}))}))()`);
      await capture('curriculum',width,theme);
      assert.equal(geometry.overflow,false,`curriculum overflow ${width} ${theme}`);
      assert(geometry.controls.every(item=>item.h>=44 && item.w>=44),`curriculum touch targets ${width} ${theme}: ${JSON.stringify(geometry.controls)}; screenshot ${artifacts}`);
    }
  }
  await evaluate('document.querySelector(".curriculum-actions button").click()');
  await until(() => evaluate('document.querySelector(".curriculum-materials").textContent.includes("Демонстрационный материал")'),'curriculum material');
  assert.equal(await evaluate('document.querySelector(".curriculum-materials").querySelectorAll("details").length'),1,'identical theory and reference must render one article despite different server IDs');
  await evaluate('document.querySelector(".curriculum-materials summary").focus()');
  assert.equal(await evaluate('document.activeElement===document.querySelector(".curriculum-materials summary")'),true,'material summary must receive keyboard focus');
  const materialBefore=await evaluate('document.querySelector(".curriculum-materials details").open');
  await send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',windowsVirtualKeyCode:13,text:'\r',unmodifiedText:'\r'});
  await send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
  await capture('curriculum-material-keyboard',1440,'light');
  await until(() => evaluate(`document.querySelector(".curriculum-materials details").open!==${materialBefore}`),'keyboard material disclosure');
  if (materialBefore) await evaluate('document.querySelector(".curriculum-materials details").open=true');
  for (const theme of ['dark','light']) for (const width of [390,768,1440]) {
    await capture('curriculum-material',width,theme);
    assert.equal(await evaluate('document.documentElement.scrollWidth>innerWidth'),false,`material pre must scroll locally ${width}/${theme}`);
    const pre=await evaluate('(() => { const el=document.querySelector(".curriculum-materials pre");return {client:el.clientWidth,scroll:el.scrollWidth,overflow:getComputedStyle(el).overflowX}; })()');
    assert(pre.scroll>pre.client && pre.overflow==='auto','long code must scroll inside its own container');
  }
  await evaluate('document.querySelectorAll(".curriculum-actions")[1].querySelector("button").click()');
  await until(() => evaluate('document.querySelectorAll(".curriculum-materials")[1].textContent.includes("Продолжить тренировку")'),'blocked material recovery action');
  await capture('curriculum-blocked',390,'light');
  assert.equal(await evaluate('document.querySelectorAll(".curriculum-materials")[1].textContent.includes("Демонстрационный материал")'),false);
  await evaluate('document.getElementById("curriculum-topic").value="templates";document.getElementById("curriculum-topic").dispatchEvent(new Event("change"))');
  await until(() => evaluate('document.querySelectorAll(".curriculum-unit").length===0 && !document.getElementById("curriculum-topic").disabled'),'empty topic');
  await capture('curriculum-empty',390,'light');
  assert.equal(await evaluate('document.getElementById("curriculum-levels").textContent.includes("ещё готовятся")'),true);
  await evaluate('document.getElementById("curriculum-back").click()');
  assert.equal(await evaluate('document.activeElement.id'),'open-curriculum','restore keyboard focus');
  await evaluate('document.getElementById("open-coding").click()');
  await until(() => evaluate('!document.getElementById("coding").hidden'),'return coding after curriculum');
  assert.equal(await evaluate('document.getElementById("coding-draft").value'),draft,'curriculum discarded coding draft');
  await send('Page.navigate',{url:origin+'?user=112'});
  await until(() => evaluate('!document.getElementById("home").hidden'),'curriculum failure home');
  await evaluate('document.getElementById("open-curriculum").click()');
  await until(() => evaluate('!document.getElementById("curriculum-retry").hidden'),'curriculum retry');
  await capture('curriculum-error',390,'light');
  await evaluate('document.getElementById("curriculum-retry").focus()');
  await send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',windowsVirtualKeyCode:13,text:'\r',unmodifiedText:'\r'});
  await send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
  await until(() => evaluate('document.querySelectorAll(".curriculum-level").length===10'),'keyboard retry success');
  await send('Page.navigate',{url:origin+'?user=107'});
  await until(() => evaluate('!document.getElementById("home").hidden'),'SD curriculum home');
  await evaluate('document.getElementById("open-curriculum").click()');
  await until(() => evaluate('document.querySelectorAll(".curriculum-level").length===10'),'curriculum before SD switch');
  await evaluate('window.__curriculumDOM=document.getElementById("curriculum-levels").firstElementChild;document.getElementById("education-direction").value="system_design";document.getElementById("education-direction").dispatchEvent(new Event("change"))');
  await until(() => evaluate('!document.getElementById("system-design").hidden && Boolean(document.getElementById("sd-open-module"))'),'SD remains available');
  await capture('curriculum-sd-switch',390,'dark');
  await evaluate('document.getElementById("education-direction").value="cpp";document.getElementById("education-direction").dispatchEvent(new Event("change"))');
  await until(() => evaluate('!document.getElementById("curriculum").hidden'),'return exact curriculum from SD');
  assert.equal(await evaluate('document.getElementById("curriculum-levels").firstElementChild===window.__curriculumDOM'),true,'SD round trip recreated curriculum');
  console.log('PASS curriculum UI SIMULATION: six viewports/themes, material disclosure/local code scrolling, blocked assignment, empty/locked sections, keyboard retry, coding draft and SD round trip preserved');
}
