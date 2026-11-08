(function () {
  'use strict';

  const MAX_REPORT_BYTES = 128 * 1024;
  const MAX_SOURCE_BYTES = 16384;
  const identity = { exercise_id: 'take_prefix', exercise_revision: 1, suite_id: 'take_prefix-public-v1' };
  const statuses = new Set(['tests_passed', 'tests_failed', 'compile_error', 'runtime_error', 'timeout',
    'resource_limit', 'output_limit', 'sandbox_unavailable', 'runner_error']);
  const bytes = value => new TextEncoder().encode(value).length;
  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  function requireValue(condition, message) { if (!condition) throw new Error(message); }
  const same = (left, right) => JSON.stringify(left) === JSON.stringify(right);
  const intArray = value => Array.isArray(value) && value.length <= 4096 && Array.from(value).every(item =>
    Number.isInteger(item) && item >= -2147483648 && item <= 2147483647);
  const boundedString = (value, limit) => typeof value === 'string' && value.length <= limit;

  async function hash(source) {
    requireValue(globalThis.crypto?.subtle, 'SHA-256 недоступен. Открой приложение через HTTPS.');
    const digest = await globalThis.crypto.subtle.digest('SHA-256', new TextEncoder().encode(source));
    return [...new Uint8Array(digest)].map(value => value.toString(16).padStart(2, '0')).join('');
  }

  function assistance(value) {
    requireValue(object(value) && typeof value.help_opened === 'boolean' &&
      typeof value.solution_opened === 'boolean', 'Неверные отметки об использовании помощи.');
    return { help_opened: value.help_opened, solution_opened: value.solution_opened };
  }

  async function createSubmission(code, help) {
    requireValue(typeof code === 'string' && bytes(code) <= MAX_SOURCE_BYTES,
      'Функция должна занимать не больше 16384 байт UTF-8.');
    const marks = assistance(help);
    requireValue(typeof globalThis.crypto?.randomUUID === 'function', 'Создание пакета недоступно в этом окне.');
    return { schema: 'cpp-recall-local-submission/v1', ...identity, source: code,
      source_sha256: await hash(code), attempt_id: globalThis.crypto.randomUUID(), assistance: marks };
  }

  function parseReport(text) {
    requireValue(typeof text === 'string' && bytes(text) <= MAX_REPORT_BYTES,
      'Отчёт должен занимать не больше 128 КиБ UTF-8.');
    try { return JSON.parse(text); } catch { throw new Error('Не удалось прочитать JSON. Вставь целый report.json.'); }
  }

  async function validateReport(value, submission, manifest) {
    const report = typeof value === 'string' ? parseReport(value) : value;
    requireValue(object(report) && bytes(JSON.stringify(report)) <= MAX_REPORT_BYTES, 'Неверный или слишком большой отчёт.');
    requireValue(object(submission) && submission.schema === 'cpp-recall-local-submission/v1' &&
      typeof submission.source === 'string' && bytes(submission.source) <= MAX_SOURCE_BYTES,
    'Подготовь пакет проверки для текущего кода заново.');
    requireValue(submission.source_sha256 === await hash(submission.source), 'Исходный пакет изменён. Подготовь его заново.');
    requireValue(report.schema === 'cpp-recall-local-check/v1' && report.trust === 'local_unverified',
      'Формат отчёта не поддерживается. Нужен локальный отчёт cpp-recall-local-check/v1.');
    requireValue(object(manifest) && bytes(JSON.stringify(manifest)) <= MAX_REPORT_BYTES,
      'Набор открытых тестов недоступен или слишком большой.');
    for (const [key, expected] of Object.entries(identity)) {
      requireValue(report[key] === expected && submission[key] === expected && manifest?.[key] === expected,
        'Отчёт относится к другой задаче, редакции или набору тестов.');
    }
    requireValue(typeof submission.attempt_id === 'string' &&
      /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(submission.attempt_id) &&
      report.attempt_id === submission.attempt_id && report.source_sha256 === submission.source_sha256,
    'Отчёт относится к другому пакету или к другому коду. Подготовь проверку заново.');
    requireValue(same(assistance(report.assistance), assistance(submission.assistance)),
      'Отметки об использовании помощи не совпадают с пакетом.');
    requireValue(statuses.has(report.status), 'Неизвестный результат проверки.');
    requireValue(object(report.compile) && ['passed', 'failed', 'not_run'].includes(report.compile.status) &&
      boundedString(report.compile.diagnostics, 65536), 'Неверная диагностика компиляции.');
    requireValue(object(report.compiler) && boundedString(report.compiler.id, 256) &&
      boundedString(report.compiler.version, 4096) && report.compiler.standard === 'c++23' &&
      Array.isArray(report.compiler.flags) && report.compiler.flags.length <= 128 &&
      report.compiler.flags.every(flag => boundedString(flag, 1024)), 'Не указана среда компиляции C++23.');
    requireValue(Array.isArray(manifest.cases) && manifest.cases.length > 0 && manifest.cases.length <= 128,
      'Набор открытых тестов недоступен.');
    const cases = new Map();
    for (const test of manifest.cases) {
      requireValue(object(test) && typeof test.id === 'string' && !cases.has(test.id) &&
        boundedString(test.title, 512) && intArray(test.pending) &&
        (test.limit === 'max' || (Number.isSafeInteger(test.limit) && test.limit >= 0)) &&
        (test.sequence === undefined || boundedString(test.sequence, 128)) &&
        object(test.expected) && intArray(test.expected.result) && intArray(test.expected.pending),
      'Повреждён набор открытых тестов.');
      cases.set(test.id, test);
    }
    requireValue(Array.isArray(report.tests) && report.tests.length <= cases.size, 'Неверный список результатов тестов.');
    const seen = new Set();
    const sequences = new Map();
    const tests = report.tests.map((row, index) => {
      requireValue(object(row) && cases.has(row.id) && !seen.has(row.id), 'Неизвестный или повторяющийся тест в отчёте.');
      seen.add(row.id);
      requireValue(['passed', 'failed'].includes(row.status) && object(row.actual) &&
        intArray(row.input_pending) && intArray(row.actual.result) && intArray(row.actual.pending),
      'Неверные фактические значения или вход теста.');
      const test = cases.get(row.id);
      requireValue(row.id === manifest.cases[index].id, 'Результаты должны идти по порядку открытых тестов без пропусков.');
      const expectedInput = test.sequence && sequences.has(test.sequence)
        ? sequences.get(test.sequence) : test.pending;
      requireValue(same(row.input_pending, expectedInput), 'Вход теста противоречит исходным данным или фактическому остатку предыдущего вызова.');
      if (test.sequence) sequences.set(test.sequence, row.actual.pending);
      const passed = same(row.actual.result, test.expected.result) && same(row.actual.pending, test.expected.pending);
      requireValue((row.status === 'passed') === passed, 'Отметка теста противоречит его фактическим значениям.');
      return { id: row.id, status: row.status, input_pending: [...row.input_pending],
        actual: { result: [...row.actual.result], pending: [...row.actual.pending] },
        test };
    });
    if (report.status === 'tests_passed') {
      requireValue(report.compile.status === 'passed' && tests.length === cases.size &&
        tests.every(test => test.status === 'passed'), 'Успех не подтверждён полным набором открытых тестов.');
    } else if (report.status === 'tests_failed') {
      requireValue(tests.length === cases.size, 'Результат tests_failed должен подтверждаться полным набором открытых тестов.');
      requireValue(report.compile.status === 'passed' && tests.some(test => test.status === 'failed'),
        'В отчёте о падении тестов нет несовпадения результатов.');
    } else if (report.status === 'compile_error') {
      requireValue(report.compile.status === 'failed' && tests.length === 0, 'Ошибка компиляции противоречит результатам тестов.');
    } else {
      requireValue(report.compile.status === 'passed' || tests.length === 0,
        'Состояние запуска противоречит компиляции.');
      if (report.status === 'runtime_error') {
        requireValue(report.compile.status === 'passed', 'Выполнение не могло начаться до успешной компиляции.');
      }
    }
    return { status: report.status, tests, total: cases.size, diagnostics: report.compile.diagnostics,
      compiler: report.compiler, trust: 'local_unverified' };
  }

  function create({ root, getSnapshot, isActive, manifest }) {
    let operation = 0, importOperation = 0, prepared = null, snapshot = null, status, result, packagePanel, packageText, paste;
    const element = (tag, text, className) => {
      const node = document.createElement(tag);
      if (text !== undefined) node.textContent = text;
      if (className) node.className = className;
      return node;
    };
    function currentSnapshot() {
      const value = getSnapshot();
      requireValue(value && Number.isSafeInteger(value.userId) && value.exerciseId === identity.exercise_id &&
        value.revision === identity.exercise_revision && typeof value.code === 'string' &&
        typeof value.helpOpened === 'boolean' && typeof value.solutionOpened === 'boolean',
      'Текущий черновик недоступен. Открой задачу заново.');
      return { userId: value.userId, exerciseId: value.exerciseId, revision: value.revision,
        code: value.code, helpOpened: value.helpOpened, solutionOpened: value.solutionOpened };
    }
    const active = token => {
      if (token !== operation || !isActive()) return false;
      try { return same(snapshot, currentSnapshot()); } catch { return false; }
    };
    function invalidate() {
      ++operation; ++importOperation;
      const hadAttempt = prepared !== null;
      prepared = null; snapshot = null;
      if (result) result.replaceChildren();
      if (packagePanel) packagePanel.hidden = true;
      if (packageText) packageText.value = '';
      if (paste) paste.value = '';
      if (status) status.textContent = hadAttempt
        ? 'Код, помощь или контекст изменились. Подготовь пакет проверки заново.' : '';
    }
    async function prepare() {
      invalidate();
      const token = operation;
      try {
        snapshot = currentSnapshot();
        status.textContent = 'Готовим пакет текущей функции…';
        const submission = await createSubmission(snapshot.code,
          { help_opened: snapshot.helpOpened, solution_opened: snapshot.solutionOpened });
        if (!active(token)) return;
        prepared = submission;
        packageText.value = JSON.stringify(submission, null, 2);
        packagePanel.hidden = false;
        status.textContent = 'Пакет подготовлен. Код ещё не проверен. Сохрани submission.json и запусти локальную проверку.';
      } catch (error) { if (token === operation && isActive()) status.textContent = error.message; }
    }
    function display(checked) {
      const labels = { tests_passed: 'Открытые тесты пройдены', tests_failed: 'Есть несовпадение с ожидаемым результатом',
        compile_error: 'Функция не скомпилировалась', runtime_error: 'Выполнение прервано', timeout: 'Превышено время проверки',
        resource_limit: 'Превышен лимит ресурсов', output_limit: 'Превышен лимит вывода',
        sandbox_unavailable: 'Изолированная среда проверки недоступна', runner_error: 'Не удалось завершить локальную проверку' };
      result.replaceChildren(element('h3', labels[checked.status]));
      if (checked.diagnostics) {
        const details = element('details'); details.append(element('summary', 'Диагностика компилятора'));
        details.append(element('pre', checked.diagnostics, 'code-block')); result.append(details);
      }
      for (const row of checked.tests.filter(test => test.status === 'failed')) {
        const section = element('div'); section.append(element('h3', row.test.title));
        section.append(element('p', row.test.sequence ? `Последовательный вызов (${row.test.sequence}). Вход для этого вызова:` : 'Вход:'));
        section.append(element('pre', `pending = ${JSON.stringify(row.input_pending)}, limit = ${row.test.limit}`, 'code-block'));
        if (!same(row.input_pending, row.test.pending)) section.append(element('p',
          `Для правильной цепочки ожидался вход pending = ${JSON.stringify(row.test.pending)}. Предыдущий вызов оставил другой вектор.`));
        section.append(element('pre', `Ожидалось: result = ${JSON.stringify(row.test.expected.result)}; pending = ${JSON.stringify(row.test.expected.pending)}\nПолучено: result = ${JSON.stringify(row.actual.result)}; pending = ${JSON.stringify(row.actual.pending)}`, 'code-block'));
        result.append(section);
      }
      const passed = checked.tests.filter(test => test.status === 'passed').length;
      result.append(element('p', checked.tests.length
        ? `В отчёте выполнено ${checked.tests.length} из ${checked.total} открытых тестов; значения совпали в ${passed}.`
        : 'В отчёте нет результатов выполненных тестов.'));
      result.append(element('p', 'Импортирован локальный отчёт. Приложение не подтверждает его подлинность; JSON и SHA-256 не являются аттестацией. Открытые тесты не доказывают корректность всех входов, отсутствие UB или освоение навыка. XP не начисляется.'));
      result.append(element('p', `Компилятор: ${checked.compiler.id} ${checked.compiler.version}; C++23.`));
      result.append(element('p', `Помощь в этом пакете: справка ${prepared.assistance.help_opened ? 'открыта' : 'не открывалась'}, пример решения ${prepared.assistance.solution_opened ? 'открыт' : 'не открывался'}.`));
    }
    async function importText(text, token, importToken) {
      if (importToken !== importOperation || token !== operation || !isActive()) return;
      try {
        requireValue(prepared && snapshot, 'Сначала подготовь пакет проверки в этом окне. После перезагрузки подготовь его заново.');
        requireValue(active(token), 'Контекст изменился. Подготовь пакет проверки заново.');
        result.replaceChildren();
        const checked = await validateReport(text, prepared, manifest);
        if (importToken !== importOperation || !active(token)) return;
        display(checked); status.textContent = 'Отчёт прочитан для текущего пакета. Исправь функцию и подготовь новую проверку при необходимости.';
        result.scrollIntoView({ block: 'start', behavior: 'auto' });
      } catch (error) {
        if (importToken === importOperation && token === operation && isActive()) { result.replaceChildren(); status.textContent = error.message; }
      }
    }
    function render() {
      invalidate(); root.replaceChildren();
      root.append(element('h2', 'Проверить в своей среде'));
      root.append(element('p', 'Подготовь пакет функции, запусти локальный проверяющий скрипт на компьютере и верни report.json. Код и отчёт не отправляются на сервер. На телефоне можно вставить готовую функцию; длинную программу писать не нужно.'));
      const prepareButton = element('button', 'Подготовить локальную проверку', 'primary'); prepareButton.type = 'button';
      prepareButton.id = 'coding-check-prepare'; prepareButton.addEventListener('click', prepare); root.append(prepareButton);
      status = element('p'); status.id = 'coding-check-status'; status.setAttribute('role', 'status'); root.append(status);
      packagePanel = element('div'); packagePanel.hidden = true; packagePanel.id = 'coding-check-package-panel';
      const packageLabel = element('label', 'Пакет submission.json — можно скопировать вручную', 'coding-label');
      packageLabel.htmlFor = 'coding-check-package'; packagePanel.append(packageLabel);
      packageText = element('textarea', undefined, 'coding-editor'); packageText.id = 'coding-check-package';
      packageText.rows = 6; packageText.readOnly = true; packageText.spellcheck = false; packagePanel.append(packageText);
      const actions = element('div', undefined, 'coding-actions');
      for (const [id, title, action] of [
        ['coding-check-download', 'Сохранить submission.json', () => {
          if (!prepared || !active(operation)) return;
          let url;
          try {
            url = URL.createObjectURL(new Blob([packageText.value], { type: 'application/json' }));
            const link = element('a'); link.href = url; link.download = 'submission.json'; document.body.append(link);
            try { link.click(); } finally { link.remove(); }
            status.textContent = 'Если Telegram не начал загрузку, скопируй пакет из поля и сохрани как submission.json. Код ещё не проверен.';
          } catch { status.textContent = 'Не удалось начать загрузку. Скопируй пакет из поля и сохрани как submission.json.'; }
          finally { if (url) setTimeout(() => URL.revokeObjectURL(url), 10000); }
        }],
        ['coding-check-copy', 'Скопировать пакет', async () => {
          const token = operation; if (!prepared || !active(token)) return;
          try { await navigator.clipboard.writeText(packageText.value);
            if (active(token)) status.textContent = 'Пакет скопирован. Сохрани его как submission.json. Код ещё не проверен.';
          } catch { if (active(token)) { status.textContent = 'Выдели пакет в поле и скопируй вручную.'; packageText.focus(); packageText.select(); } }
        }]
      ]) { const button = element('button', title, 'secondary'); button.id = id; button.type = 'button'; button.addEventListener('click', action); actions.append(button); }
      packagePanel.append(actions); root.append(packagePanel);
      root.append(element('p', 'Локальный скрипт проверяет функцию в изолированном контейнере; нужны Python 3, Docker и заранее подготовленный образ компилятора. Команда выполняется в клоне репозитория на компьютере.'));
      const link = element('a', 'Инструкция: tools/coding-checker');
      link.href = 'https://github.com/Orurh/cpp-recall-bot/tree/main/tools/coding-checker'; link.target = '_blank'; link.rel = 'noopener noreferrer'; root.append(link);
      root.append(element('pre', 'python3 tools/coding-checker/check.py submission.json --report report.json --image sha256:...', 'code-block'));
      root.append(element('p', 'Замени sha256:... на идентификатор подготовленного образа из инструкции. Пакет ожидает отчёт только в этом окне. После перезагрузки или выхода из задачи подготовь пакет и запусти проверку заново.'));
      const fileLabel = element('label', 'Открыть локальный report.json (до 128 КиБ)', 'coding-label');
      fileLabel.htmlFor = 'coding-check-file'; root.append(fileLabel);
      const file = element('input'); file.type = 'file'; file.accept = '.json,application/json'; file.id = 'coding-check-file';
      file.addEventListener('change', async () => {
        const token = operation, selected = file.files?.[0]; if (!selected) return;
        const importToken = ++importOperation;
        try {
          requireValue(selected.size <= MAX_REPORT_BYTES, 'Отчёт должен занимать не больше 128 КиБ.');
          const text = await selected.text();
          if (importToken !== importOperation || token !== operation || !isActive()) return;
          await importText(text, token, importToken);
        } catch (error) { if (importToken === importOperation && token === operation && isActive()) { result.replaceChildren(); status.textContent = error.message; } }
        finally { if (importToken === importOperation && token === operation) file.value = ''; }
      }); root.append(file);
      const pasteLabel = element('label', 'Или вставить текст report.json', 'coding-label'); pasteLabel.htmlFor = 'coding-check-report'; root.append(pasteLabel);
      paste = element('textarea', undefined, 'coding-editor'); paste.id = 'coding-check-report'; paste.rows = 5;
      paste.spellcheck = false; paste.setAttribute('aria-describedby', 'coding-check-status'); root.append(paste);
      const importButton = element('button', 'Прочитать отчёт', 'secondary'); importButton.id = 'coding-check-import'; importButton.type = 'button';
      importButton.addEventListener('click', () => {
        const token = operation, importToken = ++importOperation;
        void importText(paste.value, token, importToken);
      }); root.append(importButton);
      result = element('div'); result.id = 'coding-check-result'; result.setAttribute('aria-live', 'polite'); root.append(result);
    }
    return { render, invalidate, refresh: invalidate };
  }

  globalThis.CppRecallCodingCheck = Object.freeze({ MAX_REPORT_BYTES, createSubmission, parseReport, validateReport, create });
}());
