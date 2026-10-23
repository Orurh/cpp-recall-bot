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

  const skillDirections = [
    { title: 'Основы', topics: ['language_core'] },
    { title: 'ООП', topics: ['oop'] },
    { title: 'Библиотека', topics: ['containers'] },
    { title: 'Память', topics: ['lifetime', 'references', 'move_semantics', 'special_member_functions'] },
    { title: 'Шаблоны', topics: ['templates'] },
    { title: 'Потоки', topics: ['concurrency'] },
  ];

  const masteryLabels = {
    unassessed: 'Недостаточно данных',
    encountered: 'Встречалось',
    applying: 'Применяется',
    demonstrated: 'Подтверждено',
    retained: 'Закреплено',
    needs_recovery: 'Нужно восстановить',
  };

  function svgElement(tag, attributes = {}) {
    const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, String(value));
    return node;
  }

  function polarPoint(center, radius, index, count) {
    const angle = -Math.PI / 2 + index * 2 * Math.PI / count;
    return { x: center + Math.cos(angle) * radius, y: center + Math.sin(angle) * radius };
  }

  function renderSkillMap(summary) {
    const progressBySkill = new Map(summary.skills.map((skill) => [skill.id, skill]));
    const directions = skillDirections.map((direction) => {
      const skills = state.bootstrap.skills.filter(
        (skill) => skill.assessable && direction.topics.includes(skill.topic),
      );
      const statuses = skills.map((skill) => progressBySkill.get(skill.id)?.mastery_status || 'unassessed');
      const demonstrated = statuses.filter((status) => status === 'demonstrated').length;
      const retained = statuses.filter((status) => status === 'retained').length;
      const recovery = statuses.filter((status) => status === 'needs_recovery').length;
      const learning = statuses.filter((status) => status === 'encountered' || status === 'applying').length;
      const confirmed = demonstrated + retained;
      const inProgress = learning + recovery;
      return {
        ...direction,
        total: skills.length,
        confirmed,
        retained,
        recovery,
        learning,
        inProgress,
        confirmedRatio: skills.length ? confirmed / skills.length : 0,
        inProgressRatio: skills.length ? inProgress / skills.length : 0,
      };
    });

    const total = directions.reduce((sum, direction) => sum + direction.total, 0);
    const confirmed = directions.reduce((sum, direction) => sum + direction.confirmed, 0);
    const inProgress = directions.reduce((sum, direction) => sum + direction.inProgress, 0);
    byId('skill-map-total').textContent = `подтверждено ${confirmed} из ${total} · проверяется ${inProgress}`;

    const size = 360;
    const center = size / 2;
    const radius = 112;
    const labelRadius = 151;
    const svg = svgElement('svg', {
      viewBox: `0 0 ${size} ${size}`,
      role: 'img',
      'aria-label': `Карта навыков: подтверждено ${confirmed} из ${total}, проверяется ${inProgress}`,
    });

    for (const fraction of [.25, .5, .75, 1]) {
      svg.append(svgElement('circle', { cx: center, cy: center, r: radius * fraction, class: 'radar-ring' }));
    }

    directions.forEach((direction, index) => {
      const edge = polarPoint(center, radius, index, directions.length);
      svg.append(svgElement('line', { x1: center, y1: center, x2: edge.x, y2: edge.y, class: 'radar-axis' }));

      const position = polarPoint(center, labelRadius, index, directions.length);
      const label = svgElement('text', {
        x: position.x,
        y: position.y - 2,
        class: 'radar-label',
        'text-anchor': position.x < center - 10 ? 'end' : position.x > center + 10 ? 'start' : 'middle',
      });
      label.textContent = direction.title;
      const value = svgElement('tspan', { x: position.x, dy: 13, class: 'radar-value' });
      value.textContent = `${direction.confirmed}/${direction.total}`;
      label.append(value);
      svg.append(label);
    });

    const inProgressPoints = directions.map((direction, index) => {
      const point = polarPoint(center, radius * direction.inProgressRatio, index, directions.length);
      return `${point.x},${point.y}`;
    });
    if (inProgress) {
      svg.append(svgElement('polygon', { points: inProgressPoints.join(' '), class: 'radar-evidence-area' }));
      directions.forEach((direction, index) => {
        if (!direction.inProgress) return;
        const point = polarPoint(center, radius * direction.inProgressRatio, index, directions.length);
        svg.append(svgElement('circle', { cx: point.x, cy: point.y, r: 3.5, class: 'radar-evidence-point' }));
      });
    }

    const confirmedPoints = directions.map((direction, index) => {
      const point = polarPoint(center, radius * direction.confirmedRatio, index, directions.length);
      return `${point.x},${point.y}`;
    });
    if (confirmed) {
      svg.append(svgElement('polygon', { points: confirmedPoints.join(' '), class: 'radar-area' }));
      directions.forEach((direction, index) => {
        if (!direction.confirmed) return;
        const point = polarPoint(center, radius * direction.confirmedRatio, index, directions.length);
        svg.append(svgElement('circle', { cx: point.x, cy: point.y, r: 4, class: 'radar-point' }));
      });
    }

    if (!confirmed) {
      const empty = svgElement('text', { x: center, y: center + 24, class: 'radar-empty', 'text-anchor': 'middle' });
      empty.textContent = inProgress ? `Проверяется: ${inProgress}` : 'Пока нет данных';
      svg.append(empty);
    }
    byId('skill-map-chart').replaceChildren(svg);

    const legend = directions.map((direction) => {
      const row = element('div', 'skill-legend-row');
      row.append(element('strong', '', direction.title), element('span', '', `${direction.confirmed} / ${direction.total}`));
      const details = [`закреплено: ${direction.retained}`, `изучается: ${direction.learning}`];
      if (direction.recovery) details.push(`восстановить: ${direction.recovery}`);
      row.append(element('span', 'skill-legend-detail', details.join(' · ')));
      const track = element('div', 'skill-legend-track');
      const evidenceFill = element('div', 'skill-legend-evidence-fill');
      evidenceFill.style.width = `${direction.inProgressRatio * 100}%`;
      const confirmedFill = element('div', 'skill-legend-fill');
      confirmedFill.style.width = `${direction.confirmedRatio * 100}%`;
      track.append(evidenceFill, confirmedFill);
      row.append(track);
      return row;
    });
    byId('skill-map-legend').replaceChildren(...legend);
  }

  function renderSummary(summary) {
    const xp = summary.xp || { total: 0, next_reward: null };
    byId('stat-xp').textContent = String(xp.total);
    byId('stat-confirmed').textContent = String(
      summary.skill_mastery.demonstrated + summary.skill_mastery.retained,
    );
    byId('stat-accuracy').textContent = summary.completed ? `${summary.accuracy}%` : '—';
    byId('stat-due').textContent = String(summary.due_review);

    const recommendation = state.bootstrap.recommendation;
    if (recommendation) {
      byId('next-action-title').textContent = recommendation.title;
      byId('today-copy').textContent = `${recommendation.description} ${recommendation.topic_title}, сложность L${recommendation.level}.`;
      byId('quick-start').textContent = 'Продолжить';
    } else {
      byId('next-action-title').textContent = 'Короткая тренировка';
      byId('today-copy').textContent = 'Выбери тему и сложность в свободной практике.';
      byId('quick-start').textContent = 'Начать тренировку';
    }

    const reward = xp.next_reward;
    if (reward) {
      byId('next-reward-title').textContent = reward.title;
      byId('next-reward-copy').textContent = `ещё ${reward.remaining} XP`;
      byId('reward-fill').style.width = `${Math.max(0, Math.min(100, reward.progress_percent))}%`;
    } else {
      byId('next-reward-title').textContent = 'Все запланированные награды достигнуты';
      byId('next-reward-copy').textContent = `${xp.total} XP`;
      byId('reward-fill').style.width = '100%';
    }

    renderSkillMap(summary);
    const container = byId('topic-progress');
    container.replaceChildren();
    if (!summary.topics.length) return;
    container.append(element('p', 'eyebrow', 'ПРОГРЕСС ПО ТЕМАМ'));
    for (const topic of summary.topics.slice(0, 6)) {
      const row = element('article', 'topic-row');
      const name = element('div');
      name.append(element('strong', '', topic.title), element('small', '', `${topic.completed} завершено`));
      const level = element('span', 'level-pill', `сложность L${topic.level}`);
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
    byId('level-unlock-copy').textContent = availableLevel ? `доступно до L${availableLevel}` : 'нет готовых ступеней';
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

  async function startSession(errorTarget) {
    const modes = selectedValues('mode');
    const level = Number(document.querySelector('input[name="level"]:checked')?.value);
    byId('home-error').textContent = '';
    byId('setup-error').textContent = '';
    if (!level) {
      byId(errorTarget).textContent = 'Выбери доступную сложность.';
      return;
    }
    if (!modes.length) {
      byId(errorTarget).textContent = 'Выбери хотя бы один формат.';
      return;
    }
    try {
      state.session = await api('sessions', { modes, topics: selectedValues('topic'), level });
      renderSession();
    } catch (error) {
      byId(errorTarget).textContent = error.message;
    }
  }

  async function startRecommendedSession() {
    byId('home-error').textContent = '';
    try {
      state.session = await api('sessions', { recommended: true });
      renderSession();
    } catch (error) {
      byId('home-error').textContent = error.message;
    }
  }

  function renderSession() {
    if (!state.session) return renderHome();
    if (state.session.status === 'completed') return renderCompleted(state.session.summary, state.session.xp_gained);
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
      const implementation = question.kind === 'implementation';
      byId('instructions').textContent = implementation
        ? 'Допиши только недостающий C++-фрагмент.'
        : 'Напиши короткий ответ. Регистр важен только там, где это существенно для C++.';
      const input = document.createElement('textarea');
      input.id = 'short-answer';
      input.rows = implementation ? 4 : 3;
      input.maxLength = 4096;
      input.placeholder = implementation ? 'Только пропущенный фрагмент…' : 'Твой ответ…';
      input.spellcheck = !implementation;
      if (implementation) {
        input.className = 'code-answer';
        input.autocomplete = 'off';
        input.autocapitalize = 'off';
      }
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
    if (feedback.previous_topic_level !== feedback.topic_level) changes.push(`Сложность темы: L${feedback.previous_topic_level} → L${feedback.topic_level}`);
    if (feedback.previous_adaptive_level !== feedback.adaptive_level) changes.push(`Общая сложность: L${feedback.previous_adaptive_level} → L${feedback.adaptive_level}`);
    if (feedback.skill_mastery?.previous !== feedback.skill_mastery?.current) {
      changes.push(`Навык «${feedback.skill_mastery.skill_title}»: ${masteryLabels[feedback.skill_mastery.current]}`);
    }
    if (feedback.xp_gained) changes.push(`Опыт: +${feedback.xp_gained} XP`);
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

  function renderCompleted(summary, xpGained = 0) {
    state.session = null;
    state.bootstrap.summary = summary;
    const confirmed = summary.skill_mastery.demonstrated + summary.skill_mastery.retained;
    const xpCopy = xpGained ? ` · +${xpGained} XP за тренировку` : '';
    byId('completed-copy').textContent = `Точность ${summary.accuracy}% · подтверждено навыков: ${confirmed} · закреплено: ${summary.skill_mastery.retained}${xpCopy}`;
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
      await api('session/stop');
      state.session = null;
      await refreshHome();
    } catch (error) {
      if (tg?.showAlert) tg.showAlert(error.message);
      else window.alert(error.message);
    }
  }

  async function refreshHome() {
    state.bootstrap = await api('bootstrap');
    state.session = state.bootstrap.session;
    if (state.session) renderSession(); else renderHome();
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

  byId('quick-start').addEventListener('click', startRecommendedSession);
  byId('start-session').addEventListener('click', () => startSession('setup-error'));
  byId('open-settings').addEventListener('click', () => {
    const settings = byId('training-settings');
    settings.open = true;
    settings.scrollIntoView({ behavior: 'smooth', block: 'start' });
  });
  byId('submit-answer').addEventListener('click', submitAnswer);
  byId('next-question').addEventListener('click', nextQuestion);
  byId('stop-session').addEventListener('click', stopSession);
  byId('back-home').addEventListener('click', async () => {
    try {
      await refreshHome();
    } catch (error) {
      byId('completed-copy').textContent = error.message;
    }
  });
  byId('clear-topics').addEventListener('click', () => {
    for (const input of document.querySelectorAll('input[name="topic"]')) input.checked = false;
  });
  tg?.BackButton?.onClick?.(stopSession);
  boot();
})();
