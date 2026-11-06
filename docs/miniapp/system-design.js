'use strict';

// Generic presentation only. Protected lessons, hints and reviews come from the owner-only API.
window.CppRecallSystemDesign = {
  create({ root, api, userId, requestId }) {
    let metadata = null, session = null, draft = '', saved = '', pending = null;
    let view = 'home', timer = null, chain = Promise.resolve(), conflict = false, localRaw = null;
    let errorText = '', storageError = false, busy = false, startRequestId = null;
    const h = (tag, text = '', className = '') => {
      const node = document.createElement(tag); node.textContent = text; node.className = className; return node;
    };
    const invoke = (action, body = {}) => api(`system-design/${action}`, body);
    const field = () => root.querySelector('#sd-draft');
    const draftKey = () => session?.stage
      ? `cpp-recall-sd-draft-v1:${userId}:${session.session_id}:${session.content_revision}:${session.stage.id}` : null;
    const bytes = text => new TextEncoder().encode(text).length;
    const button = (label, action, style = 'secondary') => {
      const node = h('button', label, style); node.type = 'button';
      node.addEventListener('click', () => void run(action)); return node;
    };
    const panel = title => {
      const node = h('section', '', 'panel sd-panel'); node.append(h('h2', title)); return node;
    };
    const notice = () => {
      const node = h('p', errorText, 'form-error'); node.id = 'sd-error'; node.setAttribute('role', 'status'); return node;
    };
    function updateStatus() {
      const node = root.querySelector('#sd-storage');
      if (node) node.textContent = conflict
        ? 'Занятие изменилось в другом окне. Текст из этого окна не перезаписан. Разреши конфликт ниже.'
        : storageError ? 'Локальная копия недоступна. До выхода дождись сохранения на сервере или скопируй текст.'
        : draft !== saved ? 'Есть несохранённый текст.' : 'Черновик сохранён на сервере.';
      const error = root.querySelector('#sd-error'); if (error) error.textContent = errorText;
      root.querySelectorAll('[data-sd-action]').forEach(node => { node.disabled = busy || conflict || Boolean(pending); });
      root.querySelectorAll('select').forEach(node => { node.disabled = busy || Boolean(pending); });
    }
    function persistLocal() {
      const key = draftKey(); if (!key || bytes(draft) > 16000) return;
      try {
        const raw = localStorage.getItem(key);
        // Never delete or overwrite a recovery copy changed by another tab,
        // including when this tab has no unsaved changes of its own.
        if (raw !== localRaw) {
          if (draft !== saved) conflict = true;
          updateStatus(); return;
        }
        if (draft === saved) { localStorage.removeItem(key); localRaw = null; }
        else {
          localRaw = JSON.stringify({ version: session.version, text: draft });
          localStorage.setItem(key, localRaw);
        }
        storageError = false;
      } catch { storageError = true; }
      updateStatus();
    }
    function restoreLocal() {
      localRaw = null; conflict = false;
      try {
        localRaw = localStorage.getItem(draftKey());
        if (localRaw) {
          const local = JSON.parse(localRaw);
          if (typeof local.text !== 'string' || bytes(local.text) > 16000 || !Number.isSafeInteger(local.version))
            throw new Error('invalid local draft');
          if (local.text !== saved) {
            draft = local.text;
            conflict = local.version !== session.version;
          } else { localStorage.removeItem(draftKey()); localRaw = null; }
        }
      } catch { storageError = true; }
    }
    function accept(next, preserve = false) {
      const sameStage = session?.stage?.id && session.stage.id === next.stage?.id;
      const currentDraft = draft;
      session = next; saved = next.stage?.draft || '';
      draft = preserve && sameStage ? currentDraft : saved;
      if (!preserve && next.stage) restoreLocal();
      else if (next.stage) persistLocal();
    }
    async function sendPending() {
      if (!pending) return;
      const command = pending;
      const next = await invoke(command.action, command.body);
      pending = null;
      const typedSince = draft !== command.text;
      if (typedSince && session?.stage?.id !== next.stage?.id) {
        conflict = true; persistLocal();
        throw new Error('Этап изменился, пока ты редактировал текст. Скопируй текущий черновик перед загрузкой занятия.');
      }
      accept(next, typedSince || command.action === 'save' || command.action === 'hint');
    }
    async function mutate(action, extra = {}) {
      if (conflict) throw new Error('Сначала разреши конфликт черновиков.');
      await sendPending();
      pending = { action, text: draft, body: { session_id: session.session_id, version: session.version,
        request_id: requestId(), ...(session.stage ? { stage_id: session.stage.id } : {}), ...extra } };
      await sendPending();
    }
    async function save() {
      window.clearTimeout(timer);
      if (conflict) throw new Error('Сначала разреши конфликт черновиков. Текущий текст остаётся в этом окне.');
      if (session?.status !== 'active') return;
      if (pending && pending.action !== 'save') throw new Error('Сначала повтори незавершённый запрос кнопкой ниже.');
      await sendPending();
      if (draft !== saved) {
        if (bytes(draft) > 16000) throw new Error('Сократи ответ до 16 000 байт. Текст пока только в этом окне.');
        await mutate('save', { text: draft });
      }
      updateStatus();
    }
    function run(action) {
      const result = chain.then(async () => {
        busy = true; errorText = ''; updateStatus();
        try { return await action(); }
        catch (error) {
          errorText = error.message;
          if (error.status >= 400 && error.status < 500) pending = null;
          if (error.status === 409 && session?.stage) {
            conflict = true;
          }
          updateStatus();
          if (view === 'case') render();
          throw error;
        } finally { busy = false; updateStatus(); }
      });
      chain = result.catch(() => {});
      // Event listeners intentionally use fire-and-forget; errors are visible in the screen.
      void result.catch(() => {});
      return result;
    }
    async function load() {
      metadata = await invoke('bootstrap');
      const active = await invoke('current');
      if (active.session) { accept(active.session); view = 'case'; }
      else view = 'home';
      render();
    }
    function shell(title) {
      root.replaceChildren(h('h1', title), notice());
      if (pending && errorText) {
        const retry = button('Повторить незавершённый запрос', async () => { await sendPending(); render(); }, 'text-button');
        retry.id = 'sd-retry-command'; root.append(retry);
      }
    }
    function renderHome() {
      shell('System Design');
      const stats = metadata.statistics;
      root.append(h('p', `Начато кейсов: ${stats.started} · завершено: ${stats.completed} · в работе: ${stats.in_progress}`, 'muted'));
      const intro = panel(metadata.module.title);
      const openModule = button(metadata.module.completed ? 'Перечитать модуль' : 'Открыть модуль', async () => {
        const module = await invoke('module'); view = 'module'; renderModule(module);
      }); openModule.id = 'sd-open-module';
      intro.append(h('p', metadata.module.completed ? 'Вводный модуль прочитан.' : 'Начни со структуры архитектурного собеседования.'), openModule);
      root.append(intro);
      const exercise = panel(metadata.case.title);
      exercise.append(h('p', metadata.case.brief));
      const start = button(session?.status === 'active' ? 'Продолжить кейс' : 'Начать кейс', async () => {
        if (session?.status !== 'active') {
          startRequestId ||= requestId();
          accept(await invoke('start', { case_id: metadata.case.id, request_id: startRequestId }));
          startRequestId = null;
        }
        view = 'case'; render();
      }, 'primary');
      start.id = 'sd-start'; start.disabled = !metadata.case.available;
      exercise.append(start);
      if (!metadata.case.available) exercise.append(h('p', 'Сначала прочитай вводный модуль.', 'muted'));
      root.append(exercise);
      if (metadata.history.length) {
        const history = panel('История кейсов');
        for (const item of metadata.history) {
          const row = h('div', '', 'sd-history-row');
          row.append(h('span', `Попытка №${item.session_id} · ${item.status === 'completed' ? 'завершена' : 'в работе'}`),
            button(item.status === 'completed' ? 'Открыть разбор' : 'Продолжить', async () => {
              accept(await invoke('session', { session_id: item.session_id })); view = 'case'; render();
            }, 'text-button'));
          history.append(row);
        }
        root.append(history);
      }
    }
    function renderModule(module) {
      shell('Вводный модуль');
      root.append(button('К направлению', async () => { await load(); }, 'text-button'));
      const body = panel(module.title);
      for (const section of module.sections) {
        const article = h('section', '', 'sd-lesson-section'); article.append(h('h3', section.title), h('p', section.text)); body.append(article);
      }
      const acknowledge = button('Прочитал — перейти к практике', async () => {
        await invoke('complete-module', { acknowledged: true }); await load();
      }, 'primary'); acknowledge.id = 'sd-complete-module'; body.append(acknowledge); root.append(body);
    }
    function renderCase() {
      shell('System Design');
      root.append(button('К направлению', async () => { await save(); await load(); view = 'home'; render(); }, 'text-button'));
      const stage = session.stage;
      root.append(h('p', `${session.title} · этап ${session.current_stage + 1} из ${session.stages_total}`, 'muted'));
      const body = panel(stage.title); body.append(h('p', stage.prompt));
      const label = h('label', 'Твой подход'); label.htmlFor = 'sd-draft';
      const textarea = h('textarea', '', 'sd-editor'); textarea.id = 'sd-draft'; textarea.value = draft;
      textarea.rows = 9; textarea.maxLength = 16000; textarea.spellcheck = false;
      textarea.setAttribute('aria-describedby', 'sd-storage');
      textarea.addEventListener('input', () => {
        draft = textarea.value; persistLocal(); window.clearTimeout(timer);
        timer = window.setTimeout(() => { if (!conflict && !pending) void run(save); }, 900);
      });
      const status = h('p', '', 'muted sd-storage'); status.id = 'sd-storage'; status.setAttribute('role', 'status');
      body.append(label, textarea, status);
      const actions = h('div', '', 'sd-actions');
      const persist = button('Сохранить черновик', save); persist.id = 'sd-save'; persist.dataset.sdAction = '';
      const advance = button(session.current_stage + 1 === session.stages_total ? 'Завершить и открыть разбор' : 'Сохранить ответ и дальше', async () => {
        textarea.disabled = true;
        try { await save(); await mutate('advance', { text: draft }); render(); }
        finally { textarea.disabled = false; }
      }, 'primary'); advance.id = 'sd-advance'; advance.dataset.sdAction = '';
      actions.append(persist, advance); body.append(actions);
      if (conflict) {
        body.append(h('p', 'Текущий текст остаётся в поле. Загрузка серверной версии заменит его только по твоему выбору.', 'muted'),
          button('Загрузить сохранённое занятие', async () => {
            const next = await invoke('session', { session_id: session.session_id });
            try { localStorage.removeItem(draftKey()); } catch { /* Optional local copy. */ }
            pending = null; accept(next); conflict = false; render();
          }, 'text-button'));
      }
      root.append(body);
      const help = h('section', '', 'sd-hints'); help.append(h('h2', 'Подсказки по запросу'));
      for (const hint of stage.hints) help.append(h('p', hint));
      if (stage.more_hints) {
        const reveal = button(stage.hints.length ? 'Следующая подсказка' : 'Открыть подсказку', async () => {
          await save(); await mutate('hint'); render();
        }, 'text-button'); reveal.id = 'sd-hint'; reveal.dataset.sdAction = ''; help.append(reveal);
      } else help.append(h('p', 'Все подсказки этого этапа открыты.', 'muted'));
      root.append(help);
      if (session.accepted_answers.length) {
        const previous = h('details'); previous.append(h('summary', 'Мои предыдущие ответы'));
        for (const answer of session.accepted_answers) previous.append(h('h3', answer.title), h('p', answer.text, 'sd-own-answer'));
        root.append(previous);
      }
      updateStatus();
    }
    function renderReview() {
      shell('Разбор кейса'); root.append(h('p', session.review.notice));
      root.append(button('К направлению', load, 'text-button'));
      const counts = { covered: 0, uncertain: 0, missing: 0, unreviewed: 0 };
      for (const stage of session.review.stages) for (const criterion of stage.criteria) counts[criterion.status]++;
      root.append(h('p', `Самооценка: обосновано ${counts.covered}, сомневаюсь ${counts.uncertain}, упущено ${counts.missing}, ещё не отмечено ${counts.unreviewed}.`, 'muted'));
      session.review.stages.forEach((stage, stageIndex) => {
        const body = panel(stage.title); body.append(h('h3', 'Мой ответ'), h('p', stage.answer, 'sd-own-answer'),
          h('p', `Подсказок использовано: ${stage.hints_used}`, 'muted'), h('h3', 'Что проверить'), h('p', stage.analysis));
        stage.criteria.forEach((criterion, criterionIndex) => {
          const label = h('label', criterion.text, 'sd-rubric-label');
          const select = h('select'); select.setAttribute('aria-label', criterion.text);
          for (const [value, title] of Object.entries({ unreviewed: 'Не отмечено', covered: 'Обосновал в ответе', uncertain: 'Сомневаюсь', missing: 'Упустил' })) {
            const option = h('option', title); option.value = value; option.selected = criterion.status === value; select.append(option);
          }
          select.addEventListener('change', () => void run(async () => {
            await mutate('self-review', { stage: stageIndex, criterion: criterionIndex, status: select.value }); render();
          })); label.append(select); body.append(label);
        });
        body.append(h('h3', 'Альтернативы и цена выбора'));
        const alternatives = h('ul'); stage.alternatives.forEach(text => alternatives.append(h('li', text))); body.append(alternatives);
        for (const source of stage.sources) {
          const link = h('a', source.title); link.href = source.url; link.target = '_blank'; link.rel = 'noopener noreferrer';
          body.append(link);
        }
        root.append(body);
      });
    }
    function render() {
      if (!metadata) { shell('System Design'); root.append(button('Повторить загрузку', load)); return; }
      if (view === 'case' && session) session.status === 'completed' ? renderReview() : renderCase();
      else renderHome();
      updateStatus();
    }
    window.addEventListener('pagehide', persistLocal);
    return {
      async enter() { if (!metadata) { render(); await run(load); } },
      async flush() { await run(save); if (conflict) throw new Error('Черновик требует внимания; текст сохранён в этом окне.'); },
      persistLocal,
    };
  },
};
