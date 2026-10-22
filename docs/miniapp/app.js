'use strict';

(() => {
  const tg = window.Telegram?.WebApp;
  const byId = (id) => document.getElementById(id);
  const apiBase = new URL('./api/v1/', window.location.href);
  const state = { bootstrap: null, session: null, busy: false };
  const screens = ['loading', 'auth-error', 'home', 'training', 'feedback', 'completed'];

  tg?.ready();
  tg?.expand();
  tg?.setHeaderColor?.('secondary_bg_color');
  tg?.setBackgroundColor?.('bg_color');

  function show(name) {
    for (const id of screens) byId(id).hidden = id !== name;
    window.scrollTo({ top: 0, behavior: 'auto' });
    if (name === 'training' || name === 'feedback') {
      tg?.BackButton?.show();
      tg?.enableClosingConfirmation?.();
    } else {
      tg?.BackButton?.hide();
      tg?.disableClosingConfirmation?.();
    }
  }

  function setBusy(value) {
    state.busy = value;
    document.body.classList.toggle('busy', value);
    for (const button of document.querySelectorAll('button')) {
      if (value) {
        button.dataset.disabledBeforeBusy = button.disabled ? 'true' : 'false';
        button.disabled = true;
      } else if (button.dataset.disabledBeforeBusy !== undefined) {
        button.disabled = button.dataset.disabledBeforeBusy === 'true';
        delete button.dataset.disabledBeforeBusy;
      }
    }
  }

  async function api(path, body = {}) {
    if (!tg?.initData) throw new Error('Нет данных авторизации Telegram.');
    if (state.busy) throw new Error('Предыдущий запрос ещё выполняется.');
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 15000);
    setBusy(true);
    try {
      const response = await fetch(new URL(path, apiBase), {
        method: 'POST',
        headers: {
          'Authorization': `tma ${tg.initData}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(body),
        cache: 'no-store',
        signal: controller.signal,
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(payload.message || `HTTP ${response.status}`);
      return payload;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('Сервер не ответил вовремя. Попробуй ещё раз.');
      throw error;
    } finally {
      window.clearTimeout(timeout);
      setBusy(false);
    }
  }

  function haptic(kind) {
    tg?.HapticFeedback?.notificationOccurred?.(kind);
  }

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function renderSummary(summary) {
    byId('stat-level').textContent = String(summary.adaptive_level);
    byId('stat-accuracy').textContent = summary.completed ? `${summary.accuracy}%` : '—';
    byId('stat-due').textContent = String(summary.due_review);
    const container = byId('topic-progress');
    container.replaceChildren();
    if (!summary.topics.length) return;
    container.append(element('p', 'eyebrow', 'ПРОГРЕСС ПО ТЕМАМ'));
    for (const topic of summary.topics.slice(0, 6)) {
      const row = element('article', 'topic-row');
      const name = element('div');
      name.append(element('strong', '', topic.title), element('small', '', `${topic.completed} завершено`));
      const level = element('span', 'level-pill', `L${topic.level}`);
      row.append(name, level);
      container.append(row);
    }
  }

  function chip(item, group, checked) {
    const label = element('label', 'chip');
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.name = group;
    input.value = item.id;
    input.checked = checked;
    label.append(input, element('span', '', item.title));
    return label;
  }

  function renderLevels() {
    const summary = state.bootstrap.summary;
    const levels = state.bootstrap.levels;
    for (const item of levels) {
      item.unlocked = item.ready && item.level <= summary.unlocked_level;
      item.current = item.level === summary.adaptive_level;
    }

    const selectable = levels.filter((item) => item.unlocked && item.question_count > 0);
    const defaultLevel = selectable.some((item) => item.current)
      ? summary.adaptive_level
      : selectable.at(-1)?.level;
    const cards = levels.map((item) => {
      const label = element('label', `level-card${item.unlocked ? '' : ' locked'}`);
      const input = document.createElement('input');
      input.type = 'radio';
      input.name = 'level';
      input.value = String(item.level);
      input.disabled = !item.unlocked || item.question_count === 0;
      input.checked = item.level === defaultLevel;
      const badge = element('span', 'level-number', item.unlocked ? `L${item.level}` : item.ready ? '🔒' : '…');
      const detail = element('span', 'level-detail', !item.ready
        ? `L${item.level} · пополняется`
        : item.unlocked ? `${item.question_count} ${questionWord(item.question_count)}` : `L${item.level} · закрыт`);
      label.append(input, badge, detail);
      return label;
    });
    byId('level-list').replaceChildren(...cards);
    const availableLevel = selectable.at(-1)?.level;
    byId('level-unlock-copy').textContent = availableLevel ? `доступно до L${availableLevel}` : 'нет готовых уровней';
  }

  function questionWord(count) {
    const lastTwo = count % 100;
    const last = count % 10;
    if (lastTwo >= 11 && lastTwo <= 14) return 'вопросов';
    if (last === 1) return 'вопрос';
    if (last >= 2 && last <= 4) return 'вопроса';
    return 'вопросов';
  }

  function renderHome() {
    const data = state.bootstrap;
    const name = data.user.first_name ? `, ${data.user.first_name}` : '';
    byId('greeting').textContent = `Продолжим C++${name}?`;
    renderSummary(data.summary);
    renderLevels();

    const modeList = byId('mode-list');
    modeList.replaceChildren(...data.modes.map((mode) => chip(mode, 'mode', mode.id !== 'matching')));
    modeList.onchange = (event) => {
      if (!event.target.checked) return;
      const matching = event.target.value === 'matching';
      for (const input of modeList.querySelectorAll('input')) {
        if (input !== event.target && (matching || input.value === 'matching')) input.checked = false;
      }
    };

    byId('topic-list').replaceChildren(...data.topics.map((topic) => chip(topic, 'topic', false)));
    show('home');
  }

  function selectedValues(name) {
    return [...document.querySelectorAll(`input[name="${name}"]:checked`)].map((item) => item.value);
  }

  async function startSession() {
    const modes = selectedValues('mode');
    const level = Number(document.querySelector('input[name="level"]:checked')?.value);
    if (!level) {
      byId('setup-error').textContent = 'Выбери доступный уровень.';
      return;
    }
    if (!modes.length) {
      byId('setup-error').textContent = 'Выбери хотя бы один формат.';
      return;
    }
    byId('setup-error').textContent = '';
    try {
      state.session = await api('sessions', { modes, topics: selectedValues('topic'), level });
      renderSession();
    } catch (error) {
      byId('setup-error').textContent = error.message;
    }
  }

  function renderSession() {
    if (!state.session) return renderHome();
    if (state.session.status === 'completed') return renderCompleted(state.session.summary);
    if (state.session.phase === 'feedback') return renderFeedback(state.session.feedback);
    renderQuestion(state.session.question, state.session.progress);
  }

  function optionControl(option, type) {
    const label = element('label', 'answer-option');
    const input = document.createElement('input');
    input.type = type;
    input.name = 'answer';
    input.value = option.id;
    label.append(input, element('span', 'answer-marker'), element('span', 'answer-text', option.text));
    return label;
  }

  function matchingButton(item, side, board, questionId) {
    const button = element('button', 'match-option');
    button.type = 'button';
    if (item.matched) button.classList.add('matched');
    if (item.selected) button.classList.add('selected');
    button.disabled = item.matched || state.busy;
    const marker = element('span', 'match-marker', item.matched ? '✓' : side === 'left'
      ? String.fromCharCode(65 + item.position) : String(item.position + 1));
    button.append(marker, element('span', '', item.text));
    button.addEventListener('click', () => selectMatch(questionId, side, item.position, board.revision));
    return button;
  }

  function renderMatching(question) {
    const board = question.task.board;
    const root = element('div', 'matching');
    const summary = element('div', 'matching-summary');
    summary.append(
      element('strong', '', `${board.matched} / ${board.total} пар`),
      element('span', board.last_mismatch ? 'error-text' : '', `${board.mistakes} ошибок`),
    );
    root.append(summary);
    const columns = element('div', 'matching-columns');
    for (const [side, title] of [['left', 'Понятия'], ['right', 'Описания']]) {
      const column = element('section', 'match-column');
      column.append(element('h2', '', title));
      for (const item of board[side]) column.append(matchingButton(item, side, board, question.id));
      columns.append(column);
    }
    root.append(columns);
    return root;
  }

  function renderQuestion(question, progress) {
    byId('progress-label').textContent = `Вопрос ${progress.question_number} из ${progress.session_goal}`;
    byId('attempt-label').textContent = `Попытка ${progress.attempt}`;
    byId('progress-fill').style.width = `${Math.min(100, progress.completed / progress.session_goal * 100)}%`;
    byId('question-level').textContent = `L${question.level}`;
    byId('question-topic').textContent = question.topic_title;
    byId('question-standard').textContent = question.standard;
    byId('question-text').textContent = question.text;
    const code = byId('question-code');
    code.hidden = !question.code;
    code.querySelector('code').textContent = question.code || '';
    byId('answer-error').textContent = '';

    const area = byId('answer-area');
    area.replaceChildren();
    const task = question.task;
    byId('submit-answer').hidden = task.type === 'matching';
    if (task.type === 'single_choice' || task.type === 'multiple_choice') {
      byId('instructions').textContent = task.type === 'single_choice'
        ? 'Выбери один вариант.' : 'Можно выбрать несколько вариантов.';
      const list = element('div', 'answer-list');
      for (const option of task.options) list.append(optionControl(option, task.type === 'single_choice' ? 'radio' : 'checkbox'));
      area.append(list);
    } else if (task.type === 'short_answer') {
      byId('instructions').textContent = 'Напиши короткий ответ. Регистр важен только там, где это существенно для C++.';
      const input = document.createElement('textarea');
      input.id = 'short-answer';
      input.rows = 3;
      input.maxLength = 4096;
      input.placeholder = 'Твой ответ…';
      area.append(input);
    } else if (task.type === 'matching') {
      byId('instructions').textContent = 'Выбери элемент в любой колонке, затем найди ему пару.';
      area.append(renderMatching(question));
    }
    show('training');
  }

  async function submitAnswer() {
    const question = state.session.question;
    let answer;
    if (question.task.type === 'single_choice') {
      answer = document.querySelector('input[name="answer"]:checked')?.value;
    } else if (question.task.type === 'multiple_choice') {
      answer = selectedValues('answer');
    } else {
      answer = byId('short-answer')?.value.trim();
    }
    if (!answer || (Array.isArray(answer) && !answer.length)) {
      byId('answer-error').textContent = 'Сначала дай ответ.';
      return;
    }
    try {
      const response = await api('session/answer', { question_id: question.id, answer });
      state.session = response.session;
      haptic(state.session.feedback.correct ? 'success' : 'error');
      renderSession();
    } catch (error) {
      byId('answer-error').textContent = error.message;
    }
  }

  async function selectMatch(questionId, side, position, revision) {
    try {
      const response = await api('session/match', { question_id: questionId, side, position, revision });
      state.session = response.session;
      if (response.outcome === 'matched' || response.outcome === 'finished') haptic('success');
      if (response.outcome === 'mismatch') haptic('error');
      renderSession();
    } catch (error) {
      byId('answer-error').textContent = error.message;
    }
  }

  function renderFeedback(feedback) {
    const correct = feedback.correct;
    const completed = feedback.completed;
    byId('result-icon').textContent = correct ? '✓' : completed ? '×' : '↻';
    byId('result-icon').className = `result-icon ${correct ? 'correct' : completed ? 'wrong' : 'retry'}`;
    byId('result-title').textContent = correct
      ? feedback.attempts === 1 ? 'Верно с первой попытки' : `Верно с попытки ${feedback.attempts}`
      : completed ? 'Разберём и запомним' : 'Вернём этот вопрос позже';
    byId('result-answer').textContent = `Твой ответ: ${feedback.answer}`;

    const changes = [];
    if (feedback.previous_topic_level !== feedback.topic_level) changes.push(`Уровень темы: ${feedback.previous_topic_level} → ${feedback.topic_level}`);
    if (feedback.previous_adaptive_level !== feedback.adaptive_level) changes.push(`Общий уровень: ${feedback.previous_adaptive_level} → ${feedback.adaptive_level}`);
    byId('level-change').hidden = !changes.length;
    byId('level-change').textContent = changes.join(' · ');

    const explanation = byId('explanation');
    explanation.hidden = !feedback.explanation;
    byId('explanation-text').textContent = feedback.explanation || '';
    const source = byId('explanation-source');
    source.hidden = !feedback.source;
    source.href = feedback.source || '#';
    show('feedback');
  }

  async function nextQuestion() {
    try {
      state.session = await api('session/next');
      renderSession();
    } catch (error) {
      byId('result-title').textContent = error.message;
    }
  }

  function renderCompleted(summary) {
    state.session = null;
    state.bootstrap.summary = summary;
    byId('completed-copy').textContent = `Точность ${summary.accuracy}% · текущий L${summary.adaptive_level} · достигнут L${summary.unlocked_level}`;
    haptic('success');
    show('completed');
  }

  function confirmStop() {
    return new Promise((resolve) => {
      if (tg?.showConfirm) tg.showConfirm('Закончить текущую тренировку?', resolve);
      else resolve(window.confirm('Закончить текущую тренировку?'));
    });
  }

  async function stopSession() {
    if (!(await confirmStop())) return;
    try {
      const result = await api('session/stop');
      state.session = null;
      state.bootstrap.summary = result.summary;
      renderHome();
    } catch (error) {
      if (tg?.showAlert) tg.showAlert(error.message);
      else window.alert(error.message);
    }
  }

  async function boot() {
    if (!tg?.initData) {
      byId('connection').textContent = 'Нет авторизации';
      show('auth-error');
      return;
    }
    try {
      state.bootstrap = await api('bootstrap');
      state.session = state.bootstrap.session;
      byId('connection').textContent = 'Защищённый вход';
      if (state.session) renderSession(); else renderHome();
    } catch (error) {
      byId('auth-error-text').textContent = error.message;
      byId('connection').textContent = 'Ошибка входа';
      show('auth-error');
    }
  }

  byId('start-session').addEventListener('click', startSession);
  byId('submit-answer').addEventListener('click', submitAnswer);
  byId('next-question').addEventListener('click', nextQuestion);
  byId('stop-session').addEventListener('click', stopSession);
  byId('back-home').addEventListener('click', renderHome);
  byId('clear-topics').addEventListener('click', () => {
    for (const input of document.querySelectorAll('input[name="topic"]')) input.checked = false;
  });
  tg?.BackButton?.onClick?.(stopSession);
  boot();
})();
