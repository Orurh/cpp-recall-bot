// UI SIMULATION only. Actual C++ execution is verified separately in the local sandbox.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function checkCodingReports({ send, evaluate, until, input, artifacts }) {
  const manifest = JSON.parse(await readFile('docs/miniapp/coding-check-cases.json', 'utf8'));
  const code = `std::vector<int> TakePrefix(std::vector<int>& pending, std::size_t limit) {
  const auto count = std::min(limit, pending.size());
  std::vector<int> result(pending.begin(), pending.begin() + count);
  pending.erase(pending.begin(), pending.begin() + count);
  return result;
}`;
  await until(() => evaluate('Boolean(document.getElementById("coding-check-prepare"))'), 'local check module');
  const prepare = async () => {
    await evaluate('document.getElementById("coding-check-prepare").click()');
    await until(() => evaluate('Boolean(document.getElementById("coding-check-package").value)'), 'local package');
    return JSON.parse(await evaluate('document.getElementById("coding-check-package").value'));
  };
  const importReport = async report => {
    await evaluate(`document.getElementById('coding-check-report').value=${JSON.stringify(JSON.stringify(report))};
      document.getElementById('coding-check-import').click()`);
  };
  const reportFor = submission => ({ ...submission, source: undefined,
    schema: 'cpp-recall-local-check/v1', trust: 'local_unverified', status: 'tests_passed',
    compiler: { id: 'gcc', version: '14 (UI simulation)', standard: 'c++23', flags: ['-std=c++23'] },
    compile: { status: 'passed', diagnostics: '' },
    tests: manifest.cases.map(test => ({ id: test.id, status: 'passed', input_pending: test.pending, actual: test.expected })),
  });
  const heading = () => evaluate('document.querySelector("#coding-check-result h3")?.textContent || ""');
  const capture = async name => {
    assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'), true, name);
    await evaluate(`document.getElementById(${JSON.stringify(name.includes('idle') ? 'coding-check' : 'coding-check-result')}).scrollIntoView()`);
    const shot = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
    await writeFile(path.join(artifacts, name), Buffer.from(shot.data, 'base64'));
  };
  await input(code);
  const submission = await prepare();
  assert.equal(submission.source, code);
  assert.equal(await heading(), '');
  const wrong = reportFor(submission);
  wrong.status = 'tests_failed';
  wrong.tests[3] = { ...wrong.tests[3], status: 'failed', actual: { result: [4, 7], pending: [4, 7, 4, 9] } };
  await importReport(wrong);
  await until(async () => (await heading()).includes('несовпадение'), 'failed-case diagnostics');
  const failedText = await evaluate('document.getElementById("coding-check-result").textContent');
  assert.equal(await evaluate('document.getElementById("direction-footer").textContent.includes("Сервер проверяет")'), false);
  assert.match(failedText, /Ожидалось.*pending.*\[4,9\]/s);
  assert.match(failedText, /Получено.*pending.*\[4,7,4,9\]/s);
  assert(failedText.indexOf('Ожидалось:') < failedText.indexOf('Импортирован локальный отчёт.'), 'metadata hides the mismatch');
  assert(!failedText.includes('забыл erase'));
  for (const theme of ['dark', 'light']) {
    await evaluate(`{
      const colors=${JSON.stringify({ dark: ['#17212b', '#232e3c', '#f5f5f5'], light: ['#ffffff', '#f0f2f5', '#1f2937'] })}[${JSON.stringify(theme)}];
      ['bg-color','secondary-bg-color','text-color'].forEach((name,i)=>document.documentElement.style.setProperty('--tg-theme-'+name,colors[i]));
      Telegram.WebApp.colorScheme=${JSON.stringify(theme)}; window.__codingTheme();
    }`);
    for (const width of [390, 768, 1440]) {
      await send('Emulation.setDeviceMetricsOverride', { width, height: 1000, deviceScaleFactor: 1, mobile: width < 600 });
      await capture(`coding-check-failed-${theme}-${width}.png`);
    }
  }
  const compile = { ...reportFor(submission), status: 'compile_error', tests: [],
    compile: { status: 'failed', diagnostics: 'user_function.cpp:2: error: expected ; <script>window.reportXss=true</script>' } };
  await importReport(compile);
  await until(async () => (await heading()).includes('не скомпилировалась'), 'compile diagnostics');
  assert.equal(await evaluate('Boolean(window.reportXss)'), false);
  const compileText = await evaluate('document.getElementById("coding-check-result").textContent');
  assert(compileText.includes('нет результатов выполненных тестов'));
  assert(!compileText.includes('значения совпали'));
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 1000, deviceScaleFactor: 1, mobile: true });
  await capture('coding-check-compile-light-390.png');

  // A delayed file selection must not replace a newer paste for the same attempt.
  await evaluate(`{
    const file=document.getElementById('coding-check-file');
    Object.defineProperty(file,'files',{configurable:true,value:[{size:100,text:()=>new Promise(resolve=>window.__resolveOldReport=resolve)}]});
    file.dispatchEvent(new Event('change',{bubbles:true}));
  }`);
  await importReport(compile);
  await until(async () => (await heading()).includes('не скомпилировалась'), 'newer import');
  await evaluate(`window.__resolveOldReport(${JSON.stringify(JSON.stringify(reportFor(submission)))})`);
  await new Promise(resolve => setTimeout(resolve, 100));
  assert.match(await heading(), /не скомпилировалась/, 'late file replaced newer report');

  await importReport(reportFor(submission));
  await until(async () => (await heading()) === 'Открытые тесты пройдены', 'complete open suite');
  await input(code + '\n// исправленный вариант');
  assert.equal(await heading(), '', 'edited code retained old PASS');
  await importReport(reportFor(submission));
  await until(() => evaluate('document.getElementById("coding-check-status").textContent.includes("подготовь")'), 'old package rejected');
  assert.equal(await heading(), '');
  const fresh = await prepare();
  assert.notEqual(fresh.attempt_id, submission.attempt_id);
  await evaluate('document.getElementById("coding-show-help").click()');
  assert.equal(await evaluate('document.getElementById("coding-check-package-panel").hidden'), true, 'help did not invalidate snapshot');

  // Leaving for SD and returning preserves the source, but never a pending check.
  const before = await evaluate('document.getElementById("coding-draft").value');
  await prepare();
  await evaluate(`{
    const select=document.getElementById('education-direction');select.value='system_design';select.dispatchEvent(new Event('change',{bubbles:true}));
  }`);
  await until(() => evaluate('Boolean(document.getElementById("sd-open-module"))'), 'simulated SD screen');
  await evaluate(`{
    const select=document.getElementById('education-direction');select.value='cpp';select.dispatchEvent(new Event('change',{bubbles:true}));
  }`);
  await until(() => evaluate('!document.getElementById("coding").hidden&&Boolean(document.getElementById("coding-check-prepare"))'), 'coding return');
  assert.equal(await evaluate('document.getElementById("coding-draft").value'), before);
  assert.equal(await evaluate('document.getElementById("coding-check-package-panel").hidden'), true);
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 1000, deviceScaleFactor: 1, mobile: true });
  await capture('coding-check-idle-390.png');
  console.log('CF-02 browser UI SIMULATION PASS: package/hash, failed vectors, compile diagnostics, stale/edit/help/direction, delayed file race, six theme/viewports; no C++ execution claimed.');
}
