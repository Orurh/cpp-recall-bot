'use strict';

(() => {
  const tg = window.Telegram?.WebApp;
  const byId = (id) => document.getElementById(id);
  const apiBase = new URL('./api/v1/', window.location.href);
  const state = { bootstrap: null, session: null, busy: false, activeTab: 'home' };
  const screens = ['loading', 'auth-error', 'home', 'achievements', 'editorial', 'statistics', 'training', 'feedback', 'completed'];
  let reportDraft = null;
  let reviewCursor = 0;
  let currentScreen = 'loading';
  let startDraft = null;
  const visitId = newRequestId();
  let activitySequence = 0;
  let activityBusy = false;
  let helpReturnFocus = null;
  let trainingSelectionChanged = false;
  let selectedLearningRouteId = null;

  function updateQuickStart() {
    byId('quick-start').textContent = state.session && !trainingSelectionChanged ? 'Продолжить тренировку' : 'Тренировать';
  }

  async function recordActivity() {
    if (!state.bootstrap || !tg?.initData || activityBusy) return;
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 4000);
    activityBusy = true;
    const context = byId('card-report-dialog').open ? 'report'
      : !byId('theory-dialog').hidden ? 'theory'
      : ['editorial', 'statistics'].includes(currentScreen) ? 'admin' : currentScreen;
    try {
      await fetch(new URL('activity', apiBase), {
        method: 'POST', headers: { Authorization: `tma ${tg.initData}`, 'Content-Type': 'application/json' },
        body: JSON.stringify({ visit_id: visitId, sequence: ++activitySequence,
          visible: document.visibilityState === 'visible', context }),
        signal: controller.signal, cache: 'no-store', keepalive: true,
      });
    } catch { /* Telemetry must never block training or show network errors. */ }
    finally { window.clearTimeout(timeout); activityBusy = false; }
  }

  tg?.ready();
  tg?.expand();
  tg?.setHeaderColor?.('secondary_bg_color');
  tg?.setBackgroundColor?.('bg_color');

  function syncColorScheme() {
    document.documentElement.style.colorScheme = tg?.colorScheme === 'light' ? 'light' : 'dark';
  }
  syncColorScheme();
  tg?.onEvent?.('themeChanged', syncColorScheme);

  function show(name) {
    currentScreen = name;
    for (const id of screens) byId(id).hidden = id !== name;
    const tabsVisible = ['home', 'achievements', 'editorial', 'statistics'].includes(name);
    byId('main-tabs').hidden = !tabsVisible;
    byId('open-profile').disabled = !tabsVisible;
    byId('editorial-tab').hidden = !state.bootstrap?.feedback_admin;
    byId('statistics-tab').hidden = !state.bootstrap?.feedback_admin;
    byId('main-tabs').classList.toggle('with-editorial', Boolean(state.bootstrap?.feedback_admin));
    for (const button of document.querySelectorAll('.tab-button')) {
      button.classList.toggle('active', button.dataset.tab === name);
    }
    window.scrollTo({ top: 0, behavior: 'auto' });
    recordActivity();
    if (name === 'training' || name === 'feedback') {
      tg?.BackButton?.show();
      tg?.enableClosingConfirmation?.();
    } else {
      tg?.BackButton?.hide();
      tg?.disableClosingConfirmation?.();
    }
  }

  function openTab(name) {
    state.activeTab = name;
    if (name === 'editorial' && state.bootstrap?.feedback_admin) {
      show('editorial');
      loadReviews();
    } else if (name === 'statistics' && state.bootstrap?.feedback_admin) {
      show('statistics');
      loadStatistics();
    } else if (name === 'achievements') renderAchievements();
    else show('home');
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
    if (path.startsWith('session/') && path !== 'session/resume' && state.session?.session_id) {
      body = { ...body, session_id: state.session.session_id };
    }
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
      if (!response.ok) {
        const reference = typeof payload.error_id === 'string' ? ` Код ошибки: ${payload.error_id.slice(0, 128)}.` : '';
        throw new Error((payload.message || `HTTP ${response.status}`) + reference);
      }
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
      const catalogSkills = state.bootstrap.skills.filter(skill => direction.topics.includes(skill.topic));
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
      const assessed = confirmed + inProgress;
      return {
        ...direction,
        catalogSkills,
        total: skills.length,
        confirmed,
        retained,
        recovery,
        learning,
        inProgress,
        assessed,
        confirmedRatio: skills.length ? confirmed / skills.length : 0,
        assessedRatio: skills.length ? assessed / skills.length : 0,
      };
    });

    const total = directions.reduce((sum, direction) => sum + direction.total, 0);
    const confirmed = directions.reduce((sum, direction) => sum + direction.confirmed, 0);
    const assessed = directions.reduce((sum, direction) => sum + direction.assessed, 0);
    byId('skill-map-total').textContent = `подтверждено ${confirmed} из ${total} · охвачено ${assessed}`;

    const size = 360;
    const center = size / 2;
    const radius = 112;
    const labelRadius = 151;
    const svg = svgElement('svg', {
      viewBox: `0 0 ${size} ${size}`,
      role: 'img',
      'aria-label': `Карта навыков: подтверждено ${confirmed} из ${total}, охвачено ${assessed}`,
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

    const assessedPoints = directions.map((direction, index) => {
      const point = polarPoint(center, radius * direction.assessedRatio, index, directions.length);
      return `${point.x},${point.y}`;
    });
    if (assessed) {
      svg.append(svgElement('polygon', { points: assessedPoints.join(' '), class: 'radar-evidence-area' }));
      directions.forEach((direction, index) => {
        if (!direction.assessed) return;
        const point = polarPoint(center, radius * direction.assessedRatio, index, directions.length);
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
      empty.textContent = assessed ? `Охвачено: ${assessed}` : 'Пока нет данных';
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
      evidenceFill.style.width = `${direction.assessedRatio * 100}%`;
      const confirmedFill = element('div', 'skill-legend-fill');
      confirmedFill.style.width = `${direction.confirmedRatio * 100}%`;
      track.append(evidenceFill, confirmedFill);
      row.append(track);
      const composition = element('details', 'skill-composition');
      composition.append(element('summary', '', 'Какие навыки считаются'));
      const list = element('ul', 'skill-composition-list');
      for (const skill of direction.catalogSkills) {
        const status = skill.assessable
          ? masteryLabels[progressBySkill.get(skill.id)?.mastery_status || 'unassessed']
          : 'пока вне оценки';
        list.append(element('li', '', `${skill.title} — ${status}`));
      }
      composition.append(element('p', 'muted', `В оценке: ${direction.total}; в каталоге направления: ${direction.catalogSkills.length}. Каталог тоже не охватывает весь C++.`), list);
      for (const route of state.bootstrap.learning_routes || []) {
        if (route.status === 'unavailable' || !route.topics.some(topic => direction.topics.includes(topic))) continue;
        const button = element('button', 'text-button', `Учебный путь: ${route.title}`);
        button.type = 'button';
        button.addEventListener('click', () => {
          selectedLearningRouteId = route.id;
          renderLearningRoute(state.bootstrap);
          byId('learning-route').scrollIntoView({ block: 'start', behavior: 'smooth' });
        });
        composition.append(button);
      }
      row.append(composition);
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
    byId('stat-accuracy').textContent = summary.first_answer_accuracy == null ? '—' : `${summary.first_answer_accuracy}%`;
    byId('stat-accuracy').title = summary.first_answer_count
      ? `${summary.correct_first_answer_count} из ${summary.first_answer_count} первых ответов, включая незавершённые карточки`
      : 'Пока нет оценённых ответов';
    byId('stat-first-answer-label').textContent = summary.first_answer_count
      ? `с первого ответа (${summary.correct_first_answer_count}/${summary.first_answer_count})` : 'с первого ответа';
    byId('stat-due').textContent = String(summary.due_review);

    const recommendation = state.bootstrap.recommendation;
    if (recommendation) {
      byId('next-action-title').textContent = recommendation.title;
      byId('today-copy').textContent = recommendation.plan?.new_shortage ? recommendation.plan.description : '';
      byId('today-copy').hidden = !recommendation.plan?.new_shortage;
      updateQuickStart();
    } else {
      byId('next-action-title').textContent = 'Короткая тренировка';
      byId('today-copy').textContent = 'Выбери тему и сложность в свободной практике.';
      byId('today-copy').hidden = false;
      updateQuickStart();
    }

    const reward = xp.next_reward;
    if (reward) {
      byId('next-reward-title').textContent = reward.title;
      byId('next-reward-copy').textContent = `ещё ${reward.remaining} XP`;
      byId('reward-fill').style.width = `${Math.max(0, Math.min(100, reward.progress_percent))}%`;
    } else {
      byId('next-reward-title').textContent = 'Все доступные награды получены';
      byId('next-reward-copy').textContent = 'новые готовятся';
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

  function renderRewardNotice() {
    const newlyUnlocked = state.bootstrap.rewards?.newly_unlocked || [];
    const notice = byId('reward-notice');
    notice.hidden = !newlyUnlocked.length;
    if (!newlyUnlocked.length) return;
    byId('reward-notice-title').textContent = newlyUnlocked.length === 1
      ? `Получена награда: ${newlyUnlocked[0].title}`
      : `Получено наград: ${newlyUnlocked.length}`;
  }

  function profileInitials(user) {
    const parts = [user.first_name, user.last_name].filter(Boolean);
    if (!parts.length) return 'C++';
    return parts.map((part) => [...part][0]).join('').slice(0, 2).toUpperCase();
  }

  function renderProfileAppearance() {
    const data = state.bootstrap;
    if (!data?.rewards) return;
    const selectedFrame = data.rewards.profile.selected_frame;
    const initials = profileInitials(data.user);
    const topAvatar = byId('open-profile');
    topAvatar.className = `mini-profile frame-${selectedFrame}`;
    byId('top-profile-initials').textContent = initials;

    const profileAvatar = byId('profile-avatar');
    profileAvatar.className = `profile-avatar frame-${selectedFrame}`;
    byId('profile-initials').textContent = initials;
  }

  function renderAchievements(showScreen = true) {
    const data = state.bootstrap;
    const rewards = data.rewards;
    const xp = data.summary.xp.total;
    const selectedFrame = rewards.profile.selected_frame;
    renderProfileAppearance();
    byId('profile-name').textContent = [data.user.first_name, data.user.last_name].filter(Boolean).join(' ') || 'Профиль C++';
    byId('profile-xp').textContent = `${xp} XP`;
    const firstReward = rewards.catalog.find((reward) => reward.id === 'profile_frames');
    byId('profile-badge').hidden = firstReward?.status !== 'unlocked';

    const availableRewards = rewards.catalog.filter((reward) => reward.status !== 'planned');
    const unlockedCount = availableRewards.filter((reward) => reward.status === 'unlocked').length;
    byId('reward-count').textContent = `${unlockedCount} из ${availableRewards.length} доступных`;
    const rewardCards = rewards.catalog.map((reward) => {
      const card = element('article', `reward-card ${reward.status}`);
      const icon = element('span', 'reward-icon', reward.status === 'unlocked' ? '✓' : reward.status === 'planned' ? '⋯' : '◇');
      const copy = element('div', 'reward-card-copy');
      copy.append(element('strong', '', reward.title), element('p', '', reward.description));
      const status = reward.status === 'unlocked'
        ? 'Получено'
        : reward.status === 'planned'
          ? `${reward.threshold.toLocaleString('ru-RU')} XP · в разработке`
          : `${reward.threshold.toLocaleString('ru-RU')} XP · ещё ${reward.remaining}`;
      copy.append(element('small', '', status));
      card.append(icon, copy);
      return card;
    });
    byId('reward-list').replaceChildren(...rewardCards);

    const frameCards = rewards.profile.frames.map((frame) => {
      const label = element('label', `frame-choice frame-${frame.id}${frame.unlocked ? '' : ' locked'}`);
      const input = document.createElement('input');
      input.type = 'radio';
      input.name = 'profile-frame';
      input.value = frame.id;
      input.checked = frame.id === selectedFrame;
      input.disabled = !frame.unlocked;
      label.append(input, element('span', 'frame-preview', profileInitials(data.user)), element('strong', '', frame.title));
      if (!frame.unlocked) label.append(element('small', '', '150 XP'));
      return label;
    });
    byId('frame-list').replaceChildren(...frameCards);

    const completed = rewards.achievements.filter((achievement) => achievement.completed).length;
    byId('achievement-count').textContent = `${completed} из ${rewards.achievements.length}`;
    const achievements = rewards.achievements.map((achievement) => {
      const card = element('article', `achievement-card${achievement.completed ? ' completed' : ''}`);
      card.append(element('span', 'achievement-icon', achievement.completed ? '✓' : '○'));
      const copy = element('div');
      copy.append(element('strong', '', achievement.title), element('p', '', achievement.description));
      const track = element('div', 'achievement-track');
      const fill = element('div', 'achievement-fill');
      fill.style.width = `${Math.min(100, achievement.progress / achievement.target * 100)}%`;
      track.append(fill);
      copy.append(track, element('small', '', `${achievement.progress} / ${achievement.target}`));
      card.append(copy);
      return card;
    });
    byId('achievement-list').replaceChildren(...achievements);
    if (showScreen) show('achievements');
  }

  function presetDescription(preset) {
    const modeTitles = new Map(state.bootstrap.modes.map((mode) => [mode.id, mode.title]));
    const topicTitles = new Map(state.bootstrap.topics.map((topic) => [topic.id, topic.title]));
    const modes = preset.modes.map((mode) => modeTitles.get(mode) || mode).join(', ');
    const topics = preset.topics.length
      ? preset.topics.map((topic) => topicTitles.get(topic) || topic).join(', ')
      : 'все темы';
    return `L${preset.level} · ${modes} · ${topics}`;
  }

  function renderPresets() {
    const rewards = state.bootstrap.rewards;
    const presets = new Map(rewards.presets.map((preset) => [preset.slot, preset]));
    byId('preset-limit-copy').textContent = `${rewards.preset_slots} ${rewards.preset_slots === 1 ? 'набор' : 'набора'}`;
    const cards = [1, 2].map((slot) => {
      const unlocked = slot <= rewards.preset_slots;
      const preset = presets.get(slot);
      const card = element('article', `preset-card${unlocked ? '' : ' locked'}`);
      const copy = element('div');
      let nameInput = null;
      if (unlocked) {
        nameInput = document.createElement('input');
        nameInput.className = 'preset-name';
        nameInput.type = 'text';
        nameInput.maxLength = 32;
        nameInput.value = preset?.name || '';
        nameInput.placeholder = `Название набора ${slot}`;
        nameInput.setAttribute('aria-label', `Название набора ${slot}`);
        copy.append(nameInput);
      } else {
        copy.append(element('strong', '', `Набор ${slot}`));
      }
      copy.append(element('small', '', !unlocked
        ? 'Откроется за 400 XP'
        : preset ? presetDescription(preset) : 'Пока не сохранён'));
      const actions = element('div', 'preset-actions');
      if (preset && unlocked) {
        const apply = element('button', 'text-button', 'Применить');
        apply.type = 'button';
        apply.addEventListener('click', () => applyPreset(preset));
        actions.append(apply);
      }
      const save = element('button', 'text-button', preset ? 'Обновить' : 'Сохранить текущие');
      save.type = 'button';
      save.disabled = !unlocked;
      save.addEventListener('click', () => savePreset(slot, nameInput?.value || ''));
      nameInput?.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') {
          event.preventDefault();
          save.click();
        }
      });
      actions.append(save);
      card.append(copy, actions);
      return card;
    });
    byId('preset-list').replaceChildren(...cards);
  }

  function applyPreset(preset) {
    trainingSelectionChanged = true;
    updateQuickStart();
    for (const input of document.querySelectorAll('input[name="mode"]')) {
      input.checked = preset.modes.includes(input.value);
    }
    for (const input of document.querySelectorAll('input[name="topic"]')) {
      input.checked = preset.topics.includes(input.value);
    }
    for (const input of document.querySelectorAll('input[name="level"]')) {
      input.checked = Number(input.value) === preset.level;
    }
    byId('preset-status').textContent = `Применён «${preset.name}».`;
  }

  async function savePreset(slot, name) {
    const level = Number(document.querySelector('input[name="level"]:checked')?.value);
    const modes = selectedValues('mode');
    const normalizedName = name.trim();
    byId('preset-status').textContent = '';
    byId('setup-error').textContent = '';
    if (!normalizedName) {
      byId('setup-error').textContent = 'Введи название набора.';
      return;
    }
    if (!level || !modes.length) {
      byId('setup-error').textContent = 'Для сохранения выбери сложность и хотя бы один формат.';
      return;
    }
    try {
      const response = await api('training-presets', {
        slot,
        name: normalizedName,
        level,
        modes,
        topics: selectedValues('topic'),
      });
      state.bootstrap.rewards = response.rewards;
      renderPresets();
      byId('preset-status').textContent = `«${normalizedName}» сохранён.`;
    } catch (error) {
      byId('setup-error').textContent = error.message;
    }
  }

  async function selectProfileFrame(frameId) {
    byId('frame-error').textContent = '';
    try {
      const response = await api('rewards/profile-frame', { frame_id: frameId });
      state.bootstrap.rewards = response.rewards;
      renderAchievements(false);
    } catch (error) {
      byId('frame-error').textContent = error.message;
      renderAchievements(false);
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

  function renderLearningRoute(data) {
    const routes = (data.learning_routes || (data.learning_route ? [data.learning_route] : []))
      .filter(route => route.status !== 'unavailable');
    const route = routes.find(item => item.id === selectedLearningRouteId)
      || routes.find(item => item.id === state.session?.learning_route_id) || routes[0];
    byId('learning-route').hidden = !route;
    if (route) {
      selectedLearningRouteId = route.id;
      byId('learning-route-select').replaceChildren(...routes.map(item => {
        const option = element('option', '', item.title);
        option.value = item.id;
        option.selected = item.id === route.id;
        return option;
      }));
      byId('learning-route-title').textContent = route.title;
      byId('learning-route-next').textContent = route.next_action;
      byId('learning-route-description').textContent = route.description;
      byId('learning-route-steps').replaceChildren(...route.steps.map(step =>
        element('li', step.completed ? 'muted' : '', `${step.completed ? '✓ ' : ''}${step.title}`)));
      const unfinishedRoute = state.session?.learning_route_id === route.id ? state.session.session_id
        : data.saved_sessions?.find(session => session.available && session.learning_route_id === route.id)?.id;
      byId('start-learning-route').dataset.sessionId = unfinishedRoute || '';
      byId('start-learning-route').dataset.routeId = route.id;
      byId('start-learning-route').dataset.available = Boolean(unfinishedRoute || route.status === 'ready');
      byId('learning-route-methods').textContent = `Основные операции: ${(route.methods || []).join(' · ')}`;
      byId('start-learning-route').textContent = unfinishedRoute ? 'Продолжить начатый шаг' : 'Тренировать следующий шаг';
      byId('start-learning-route').disabled = !unfinishedRoute && route.status !== 'ready';
      byId('learning-route-error').textContent = '';
    }
  }

  byId('learning-route-select').addEventListener('change', event => {
    selectedLearningRouteId = event.target.value;
    renderLearningRoute(state.bootstrap);
  });

  function renderHome() {
    trainingSelectionChanged = false;
    const data = state.bootstrap;
    state.activeTab = 'home';
    renderProfileAppearance();
    const name = data.user.first_name ? `, ${data.user.first_name}` : '';
    byId('greeting').textContent = `Продолжим C++${name}?`;
    renderSummary(data.summary);
    renderLearningRoute(data);
    renderRewardNotice();
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
    renderPresets();
    const saved = data.saved_sessions || [];
    byId('saved-trainings').hidden = !saved.length;
    byId('saved-training-list').replaceChildren(...saved.map((training) => {
      const button = element('button', 'secondary', training.available
        ? `№${training.id} · ${training.topics?.join(', ') || 'Все темы'} · завершено ${training.completed} из ${training.total}`
        : 'Тренировка сохранена, но сейчас не открывается');
      button.type = 'button';
      button.disabled = !training.available;
      button.dataset.sessionId = String(training.id);
      button.addEventListener('click', async () => {
        try {
          state.session = await api('session/resume', { session_id: training.id });
          renderSession();
        } catch (error) { byId('home-error').textContent = error.message; }
      });
      return button;
    }));
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
      state.session = await requestSession({ modes, topics: selectedValues('topic'), level, balanced: true });
      renderSession();
    } catch (error) {
      byId(errorTarget).textContent = error.message;
    }
  }

  async function startRecommendedSession() {
    byId('home-error').textContent = '';
    try {
      state.session = await requestSession({ recommended: true });
      renderSession();
    } catch (error) {
      byId('home-error').textContent = error.message;
    }
  }

  byId('start-learning-route').addEventListener('click', async () => {
    const button = byId('start-learning-route');
    if (state.busy || button.disabled) return;
    button.disabled = true;
    byId('learning-route-error').textContent = '';
    try {
      const sessionId = Number(button.dataset.sessionId);
      state.session = sessionId
        ? await api('session/resume', { session_id: sessionId })
        : await requestSession({ route_id: button.dataset.routeId });
      renderSession();
    } catch (error) {
      byId('learning-route-error').textContent = error.message;
    } finally {
      button.disabled = button.dataset.available !== 'true';
    }
  });

  async function requestSession(body) {
    const signature = JSON.stringify(body);
    if (!startDraft || startDraft.signature !== signature) startDraft = { signature, requestId: newRequestId() };
    const result = await api('sessions', { ...body, request_id: startDraft.requestId });
    startDraft = null;
    return result;
  }

  function renderSession() {
    if (!state.session) return renderHome();
    if (state.session.status === 'completed') {
      return renderCompleted(state.session.summary, state.session.xp_gained, state.session.learning);
    }
    if (state.session.phase === 'feedback') return renderFeedback(state.session.feedback);
    const planCopy = byId('training-plan-copy');
    if (planCopy) {
      planCopy.hidden = !state.session.training_plan?.new_shortage;
      planCopy.textContent = state.session.training_plan?.description || '';
    }
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
    const referenceHelp = byId('reference-help');
    referenceHelp.hidden = !question.references?.length;
    referenceHelp.open = false;
    byId('reference-assisted').hidden = !question.assisted;
    byId('reference-links').replaceChildren(...(question.references || []).map((entry) => {
      const button = element('button', 'secondary', entry.title);
      button.type = 'button';
      button.addEventListener('click', () => openReference(question.id, entry.id, button));
      return button;
    }));
    byId('question-public-id').textContent = question.public_id || '';
    byId('report-question').hidden = !question.public_id || !question.content_revision;
    byId('progress-label').textContent = progress.repair_phase
      ? `Основная часть завершена · осталось исправить: ${progress.repairs_remaining}`
      : `Вопрос ${progress.question_number} из ${progress.session_goal}`;
    byId('attempt-label').textContent = progress.attempt === 1
      ? 'Первый ответ'
      : `Исправление · попытка ${progress.attempt}`;
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
    byId('feedback-public-id').textContent = feedback.public_id || '';
    byId('report-feedback').hidden = !feedback.public_id || !feedback.content_revision;
    const correct = feedback.correct;
    byId('result-icon').textContent = correct ? '✓' : '↻';
    byId('result-icon').className = `result-icon ${correct ? 'correct' : 'retry'}`;
    byId('result-title').textContent = correct
      ? feedback.assisted ? 'Верно с помощью'
        : feedback.attempts === 1
        ? 'Верно самостоятельно'
        : 'Верно после повторной попытки'
      : 'Карточка вернётся в конце';
    byId('result-answer').textContent = `Твой ответ: ${feedback.answer}`;

    const changes = [];
    if (correct && feedback.assisted) changes.push('Без XP и подтверждения навыка: использована помощь');
    if (feedback.previous_topic_level !== feedback.topic_level) changes.push(`Сложность темы: L${feedback.previous_topic_level} → L${feedback.topic_level}`);
    if (feedback.previous_adaptive_level !== feedback.adaptive_level) changes.push(`Общая сложность: L${feedback.previous_adaptive_level} → L${feedback.adaptive_level}`);
    if (feedback.skill_mastery?.previous !== feedback.skill_mastery?.current) {
      changes.push(`Навык «${feedback.skill_mastery.skill_title}»: ${masteryLabels[feedback.skill_mastery.current]}`);
    }
    if (feedback.xp_gained) changes.push(`Опыт: +${feedback.xp_gained} XP`);
    for (const reward of feedback.rewards_unlocked || []) changes.push(`Новая награда: ${reward.title}`);
    byId('level-change').hidden = !changes.length;
    byId('level-change').textContent = changes.join(' · ');

    const explanation = byId('explanation');
    explanation.hidden = !feedback.explanation;
    byId('explanation-text').textContent = feedback.explanation || '';
    const source = byId('explanation-source');
    source.hidden = !feedback.source;
    source.href = feedback.source || '#';
    const theoryButton = byId('open-theory');
    theoryButton.hidden = correct || !feedback.theory_available;
    theoryButton.textContent = feedback.theory_viewed ? 'Открыть разбор ещё раз' : 'Разобрать принцип';
    byId('theory-note').hidden = correct || !feedback.theory_viewed;
    byId('next-question').textContent = correct ? 'Следующий вопрос' : 'Продолжить';
    show('feedback');
  }

  function closeTheory() {
    byId('theory-dialog').hidden = true;
    document.body.classList.remove('theory-open');
    helpReturnFocus?.focus();
    recordActivity();
  }

  function renderTheory(theory, reference = false) {
    byId('help-kind').textContent = reference ? 'СПРАВКА · C++23' : 'РАЗБОР ПРИНЦИПА';
    byId('theory-title').textContent = theory.title;
    byId('theory-principle').textContent = theory.principle;
    byId('theory-mechanism').textContent = theory.mechanism;
    byId('theory-example').textContent = theory.example;
    byId('theory-mistake').textContent = theory.common_mistake;
    const sources = theory.sources.map((url, index) => {
      const link = element('a', '', `Источник ${index + 1} ↗`);
      link.href = url;
      link.target = '_blank';
      link.rel = 'noreferrer';
      return link;
    });
    byId('theory-sources').replaceChildren(...sources);
    byId('theory-dialog').hidden = false;
    document.body.classList.add('theory-open');
    byId('close-theory').focus();
    recordActivity();
  }

  async function openTheory() {
    const feedback = state.session?.feedback;
    if (!feedback) return;
    helpReturnFocus = byId('open-theory');
    try {
      const response = await api('session/theory', { question_id: feedback.question_id });
      state.session = response.session;
      renderFeedback(state.session.feedback);
      renderTheory(response.theory);
    } catch (error) {
      byId('result-title').textContent = error.message;
    }
  }

  async function openReference(questionId, referenceId, button) {
    if (state.busy || state.session?.question?.id !== questionId) return;
    helpReturnFocus = button;
    button.disabled = true;
    try {
      const response = await api('session/reference', { question_id: questionId, reference_id: referenceId });
      if (state.session?.question?.id !== questionId || state.session.phase === 'feedback') return;
      state.session.question.assisted = response.assisted;
      byId('answer-error').textContent = '';
      byId('reference-assisted').hidden = false;
      // Do not rerender: keep the typed answer and any selected matching pair.
      renderTheory(response.reference, true);
    } catch (error) {
      byId('answer-error').textContent = error.message;
    } finally { button.disabled = false; }
  }

  async function nextQuestion() {
    try {
      state.session = await api('session/next');
      renderSession();
    } catch (error) {
      byId('result-title').textContent = error.message;
    }
  }

  function newRequestId() {
    if (window.crypto.randomUUID) return window.crypto.randomUUID();
    const bytes = window.crypto.getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  }

  function openCardReport() {
    if (state.busy || !state.session) return;
    const phase = state.session.phase;
    const card = phase === 'feedback' ? state.session.feedback : state.session.question;
    const questionId = card.question_id || card.id;
    const key = `${questionId}:${card.content_revision}:${phase}`;
    if (reportDraft?.key !== key) {
      reportDraft = { key, questionId, revision: card.content_revision, phase, payload: null, receipt: null };
      byId('report-category').value = 'technical_error';
      byId('report-comment').value = '';
      byId('report-error').textContent = '';
    }
    byId('report-card-number').textContent = card.public_id;
    const sent = Boolean(reportDraft.receipt);
    byId('report-success').hidden = !sent;
    byId('report-success').textContent = sent ? `Отзыв №${reportDraft.receipt.feedback_id} принят. Спасибо, проверим!` : '';
    byId('report-category').disabled = sent;
    byId('report-comment').disabled = sent;
    byId('send-report').hidden = sent;
    byId('card-report-dialog').showModal();
    (sent ? byId('close-report') : byId('report-comment')).focus();
  }

  async function sendCardReport(event) {
    event.preventDefault();
    if (state.busy || !reportDraft || reportDraft.receipt) return;
    const comment = byId('report-comment').value.trim();
    if (!comment) {
      byId('report-error').textContent = 'Опиши, что стоит проверить.';
      return;
    }
    const category = byId('report-category').value;
    // Keep the same id after a timeout; the first request may already have been saved.
    if (!reportDraft.payload || reportDraft.payload.comment !== comment || reportDraft.payload.category !== category) {
      reportDraft.payload = { request_id: newRequestId(), question_id: reportDraft.questionId,
        content_revision: reportDraft.revision, phase: reportDraft.phase, category, comment };
    }
    byId('report-error').textContent = '';
    byId('report-category').disabled = true;
    byId('report-comment').disabled = true;
    try {
      reportDraft.receipt = await api('card-feedback', reportDraft.payload);
      byId('report-success').textContent = `Отзыв №${reportDraft.receipt.feedback_id} принят. Спасибо, проверим!`;
      byId('report-success').hidden = false;
      byId('send-report').hidden = true;
      byId('close-report').focus();
    } catch (error) {
      byId('report-error').textContent = `${error.message} Текст сохранён в форме, можно отправить повторно.`;
      byId('report-category').disabled = false;
      byId('report-comment').disabled = false;
    }
  }

  const reviewStatuses = { new: 'Новый', in_review: 'Проверяется', resolved: 'Исправлен', rejected: 'Отклонён' };
  const reportCategories = { technical_error: 'Техническая ошибка', unclear_wording: 'Неясная формулировка',
    answer_or_source: 'Ответ или источник', other: 'Другое' };

  function reviewItem(item) {
    const article = element('article', 'panel review-card');
    const heading = element('h2', '', `${item.public_id} · отзыв №${item.feedback_id}`);
    const metadataText = () => `${reportCategories[item.category]} · ${reviewStatuses[item.status]} · ${new Date(item.created_at).toLocaleString('ru-RU')}`;
    const metadata = element('p', 'muted', metadataText());
    article.append(heading, metadata);
    article.append(element('p', 'review-comment', item.comment));
    const details = element('details', 'review-content');
    details.append(element('summary', '', `Редакция ${item.content_revision.slice(0, 12)} · ${item.phase === 'question' ? 'во время ответа' : 'в разборе'}`));
    details.append(element('h3', '', item.content.text));
    if (item.content.code) details.append(element('pre', '', item.content.code));
    details.append(element('pre', '', JSON.stringify(item.content.task, null, 2)));
    details.append(element('p', '', item.content.explanation));
    if (item.content.source) details.append(element('p', 'muted', item.content.source));
    article.append(details);

    const referenceHistory = element('details', 'review-content review-reference-history');
    const references = item.references || [];
    referenceHistory.append(element('summary', '', `Справка до отправки отзыва: ${references.length}`));
    if (!references.length) {
      referenceHistory.append(element('p', 'muted',
        'Записей о конкретной статье нет. В старых версиях приложения такие сведения не сохранялись.'));
    }
    for (const view of references) {
      const content = view.content;
      referenceHistory.append(element('h3', '', content.title));
      referenceHistory.append(element('p', 'muted',
        `${view.reference_id} · редакция ${view.reference_content_revision.slice(0, 12)} · попытка ${view.attempt_number} · ${new Date(view.opened_at).toLocaleString('ru-RU')}`));
      referenceHistory.append(element('p', '', content.principle));
      referenceHistory.append(element('p', '', content.mechanism));
      referenceHistory.append(element('pre', '', content.example));
      referenceHistory.append(element('p', '', content.common_mistake));
      for (const source of content.sources) referenceHistory.append(element('p', 'muted', source));
    }
    article.append(referenceHistory);

    const form = element('form', 'review-form');
    const suffix = String(item.feedback_id);
    const status = element('select');
    status.id = `review-status-${suffix}`;
    for (const [value, label] of Object.entries(reviewStatuses)) {
      const option = element('option', '', label);
      option.value = value;
      status.append(option);
    }
    status.value = item.status;
    const statusLabel = element('label', '', 'Состояние');
    statusLabel.htmlFor = status.id;
    const decision = element('textarea');
    decision.id = `review-decision-${suffix}`;
    decision.rows = 3;
    decision.maxLength = 2000;
    decision.value = item.decision;
    const decisionLabel = element('label', '', 'Решение и обоснование');
    decisionLabel.htmlFor = decision.id;
    const source = element('input');
    source.id = `review-source-${suffix}`;
    source.type = 'url';
    source.placeholder = 'https://…';
    source.maxLength = 2048;
    source.value = item.source;
    const sourceLabel = element('label', '', 'Источник проверки');
    sourceLabel.htmlFor = source.id;
    const note = element('p', 'muted report-note', 'Для закрытия нужны решение и источник. Сначала внеси проверенную правку в банк, затем отметь отзыв исправленным.');
    const message = element('p', 'form-error');
    message.setAttribute('role', 'status');
    const save = element('button', 'secondary', 'Сохранить решение');
    save.type = 'submit';
    form.append(statusLabel, status, decisionLabel, decision, sourceLabel, source, note, message, save);
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      if (state.busy) return;
      try {
        const result = await api('admin/card-feedback/review', { feedback_id: item.feedback_id, version: item.version,
          status: status.value, decision: decision.value.trim(), source: source.value.trim() });
        item.version = result.version;
        item.status = result.status;
        metadata.textContent = metadataText();
        message.textContent = 'Решение сохранено.';
      } catch (error) {
        message.textContent = error.message;
      }
    });
    article.append(form);
    return article;
  }

  async function loadReviews(append = false) {
    if (state.busy || !state.bootstrap?.feedback_admin) return;
    byId('review-error').textContent = '';
    const filter = byId('review-filter');
    filter.disabled = true;
    try {
      const data = await api('admin/card-feedback', { status: filter.value, before_id: append ? reviewCursor : 0 });
      const cards = data.items.map(reviewItem);
      if (append) byId('review-items').append(...cards);
      else byId('review-items').replaceChildren(...(cards.length ? cards : [element('p', 'muted', 'Отзывов в этой очереди пока нет.')]));
      reviewCursor = data.next_before_id;
      byId('more-reviews').hidden = !reviewCursor;
    } catch (error) {
      byId('review-error').textContent = error.message;
    } finally {
      filter.disabled = false;
    }
  }

  async function loadStatistics() {
    if (state.busy || !state.bootstrap?.feedback_admin) return;
    byId('statistics-error').textContent = '';
    const period = byId('statistics-period');
    period.disabled = true;
    try {
      const data = await api('admin/statistics', { days: Number(period.value) });
      const content = byId('statistics-content');
      const date = (value) => value ? new Date(value).toLocaleString('ru-RU') : 'нет данных';
      const duration = (seconds) => seconds == null ? 'нет данных' : `${Math.round(seconds / 60)} мин`;
      const section = (title, pairs) => {
        const panel = element('section', 'panel');
        panel.append(element('h2', '', title));
        const list = element('dl', 'statistics-values');
        for (const [label, value] of pairs) list.append(element('dt', '', label), element('dd', '', String(value ?? 'нет данных')));
        panel.append(list);
        return panel;
      };
      const s = data.summary;
      const r = data.returns;
      const retention = (days) => r[`d${days}_eligible`]
        ? `${r[`d${days}_returned`]} из ${r[`d${days}_eligible`]} (${Math.round(100 * r[`d${days}_returned`] / r[`d${days}_eligible`])}%)`
        : 'ещё нет зрелой группы';
      const learningPanels = [];
      if (data.learning) {
        const l = data.learning;
        const first = l.first_training;
        const learningReturn = (value) => value.eligible
          ? `${value.returned} из ${value.eligible} (${value.rate}%) · ожидаем: ${value.pending}`
          : value.observed ? `ещё нет зрелой группы · ожидаем: ${value.pending}` : 'нет учебных ответов';
        learningPanels.push(section('Первый учебный опыт', [
          ['Первый принятый ответ (человек)', l.activated_users],
          ['Первую тренировку начали / ответили в ней', `${first.started_users} / ${first.answered_users}`],
          ['Первую тренировку полностью завершили', first.completed_users],
          ['Доля завершений первой тренировки', first.completion_rate == null ? 'нет данных' : `${first.completion_rate}%`],
        ]), element('p', 'muted', 'Активация — первый принятый ответ, в том числе неверный. Первая тренировка — первый наблюдаемый старт, а не первая за жизнь пользователя. Группы по датам первого ответа и первого старта считаются отдельно; повторный старт не становится первым. Доля завершений может расти. Конверсия приглашений не вычисляется: число приглашённых не учитывается.'),
        section('Возвраты к занятиям', [
          ['На следующий день после первого ответа (D1)', learningReturn(l.returns.d1)],
          ['На 7–9-й день после первого ответа', learningReturn(l.returns.d7_9)],
        ]), element('p', 'muted', `Группа первых ответов с ${l.cohort_since}, наблюдение до ${date(l.observed_at)}. Возврат требует принятого ответа, не просто открытия приложения. В знаменатель входят только те, для кого целевой день или всё окно уже закончилось по UTC. Учебная активность не доказывает сохранённое знание.`));
      } else {
        learningPanels.push(element('p', 'muted', 'Учебная статистика станет доступна после обновления сервера.'));
      }
      content.replaceChildren(element('p', 'muted', `Сбор начат: ${date(data.tracking_since)}. Дни — UTC. Прежняя история не восстановлена. Демонстрационные записи исключены; реальные тестеры Telegram учитываются как обычные пользователи. Автоматические проверки используют отдельную базу. Возвраты оцениваются только после полного окончания соответствующего дня.`),
        section('Пользователи и тренировки', [
          ['Запустили бота', s.bot_users], ['Открыли приложение', s.opened_users],
          ['Начали / завершили (человек)', `${s.started_users} / ${s.completed_users}`],
          ['Тренировок начато / завершено', `${s.sessions_started} / ${s.completed}`],
          ['Доля полных завершений', s.completion_rate == null ? 'нет данных' : `${s.completion_rate}%`],
          ['Незавершённые / истёк срок', `${s.active} / ${s.expired}`],
          ['Остановлены вручную / заменены новой', `${s.stopped} / ${s.replaced}`],
          ['Повреждённые / несовместимые снимки', s.technical_failure], ['Возобновлений', s.resumes],
          ['Открытие приложения на следующий день (D1)', retention(1)], ['Открытие приложения на седьмой день (D7)', retention(7)],
        ]), ...learningPanels, section('Практика и время', [
          ['Ответов / ошибок', `${s.attempts} / ${s.errors}`], ['Исправлений после ошибки', s.corrections],
          ['Открытий теории', s.theory_opened], ['Карточек со справкой', s.reference_opened],
          ['Новые / плановые повторения', `${s.new_cards} / ${s.review_cards}`],
          ['Досрочные повторения / попытки исправления', `${s.early_review_cards} / ${s.repair_attempts}`],
          ['Оценка активного времени', duration(s.active_seconds)],
          ['Медиана от старта до завершения, с паузами', duration(s.median_wall_seconds)],
          ...data.time_by_kind.map((row) => [
            `${({question: 'Ответ', feedback: 'Разбор ответа', theory: 'Теория'})[row.context] || row.context} · ${({new: 'новое', review: 'повторение', early_review: 'досрочное повторение', repair: 'исправление'})[row.practice_kind] || 'прочее'}`,
            duration(row.seconds),
          ]),
        ]));
      content.append(element('p', 'muted', 'Активное время — оценка по сигналам видимости приложения, не измерение внимания. Фон, длинные разрывы связи и работа владельца с отзывами не считаются учёбой. Доля завершений включает ещё открытые тренировки выбранной группы стартов и может расти.'));
      const daily = element('section', 'panel');
      daily.append(element('h2', '', 'Активность по дням'));
      const wrap = element('div', 'statistics-table-wrap');
      const table = element('table', 'statistics-table');
      const head = element('tr');
      for (const text of ['Дата', 'Входы¹', 'Отвечали¹', 'Старт', 'Финиш']) head.append(element('th', '', text));
      const thead = element('thead'); thead.append(head); table.append(thead);
      const tbody = element('tbody');
      for (const row of data.activity) {
        const tr = element('tr');
        for (const value of [row.day, row.visitors, row.learners, row.started, row.completed]) tr.append(element('td', '', value));
        tbody.append(tr);
      }
      table.append(tbody); wrap.append(table); daily.append(wrap, element('p', 'muted', '¹ Уникальные люди за день. Старт и финиш — события, поэтому могут относиться к разным тренировкам.'));
      content.append(daily);
      const t = data.technical;
      content.append(section('Состояние процесса', [
        ['Версия', t.version], ['Работает', duration(t.uptime_seconds)],
        ['HTTP-запросов / ошибок сервера', `${t.http_requests} / ${t.http_server_errors}`],
        ['Отказов из-за перегрузки', t.rejected_connections ?? 0],
        ['Ошибок чтения / отправки HTTP', `${t.http_read_errors ?? 0} / ${t.http_write_errors ?? 0}`],
        ['Очередь запросов', t.executor ? `${t.executor.pending} / ${t.executor.pending_limit}` : 'нет данных'],
        ['Занятых потоков', t.executor ? `${t.executor.active} / ${t.executor.workers}` : 'нет данных'],
        ['Соединений с БД: свободно / всего', `${t.pool_available} / ${t.pool_size}`],
        ['Последний успешный опрос Telegram', date(t.telegram_last_success ? t.telegram_last_success * 1000 : null)],
        ['Ошибок опроса Telegram', t.telegram_errors],
      ]));
      content.append(element('p', 'muted', 'Технические счётчики обнуляются при перезапуске процесса; учебные события сохраняются в PostgreSQL.'));
      const m = data.maintenance;
      content.append(section('Наблюдение за сервером', m ? [
        ['Последняя проверка', date(m.checked_at)], ['Проверена копия и восстановление', date(m.backup_verified_at)],
        ['Свободно на диске', m.disk_free_bytes == null ? 'нет данных' : `${(m.disk_free_bytes / 1073741824).toFixed(1)} ГиБ`],
        ['Замечания', m.issues?.join('; ') || 'нет'],
      ] : [['Состояние', 'Наблюдение на сервере ещё не подключено']]));
    } catch (error) { byId('statistics-error').textContent = error.message; }
    finally { period.disabled = false; }
  }

  function renderCompleted(summary, xpGained = 0, learning = null) {
    state.session = null;
    state.bootstrap.summary = summary;
    const confirmed = summary.skill_mastery.demonstrated + summary.skill_mastery.retained;
    const xpCopy = xpGained ? ` · +${xpGained} XP за тренировку` : '';
    const learningCopy = learning
      ? `Карточек завершено: ${learning.completed}. Самостоятельно с первого ответа: ${learning.first_try_correct}; исправлено после ошибки: ${learning.corrected_after_error}; со справкой с первого ответа: ${learning.assisted_first_try || 0}; разборов открыто: ${learning.theory_opened}; справок открыто: ${learning.reference_opened || 0}. `
      : '';
    byId('completed-copy').textContent = `${learningCopy}Всего подтверждено навыков: ${confirmed}; закреплено: ${summary.skill_mastery.retained}${xpCopy}.`;
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
    renderProfileAppearance();
    renderHome();
  }

  async function saveAndGoHome() {
    try { await refreshHome(); }
    catch (error) {
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
      renderProfileAppearance();
      renderHome();
    } catch (error) {
      byId('auth-error-text').textContent = error.message;
      byId('connection').textContent = 'Ошибка входа';
      show('auth-error');
    }
  }

  byId('refresh-statistics').addEventListener('click', loadStatistics);
  byId('statistics-period').addEventListener('change', loadStatistics);
  document.addEventListener('visibilitychange', recordActivity);
  window.addEventListener('pagehide', recordActivity);
  window.addEventListener('pageshow', recordActivity);
  byId('card-report-dialog').addEventListener('close', recordActivity);
  byId('report-question').addEventListener('click', () => queueMicrotask(recordActivity));
  byId('report-feedback').addEventListener('click', () => queueMicrotask(recordActivity));
  window.setInterval(recordActivity, 5000);
  byId('quick-start').addEventListener('click', () => {
    if (state.session && !trainingSelectionChanged) renderSession();
    else if (trainingSelectionChanged || byId('training-settings').open || selectedValues('topic').length) startSession('home-error');
    else startRecommendedSession();
  });
  byId('report-question').addEventListener('click', openCardReport);
  byId('report-feedback').addEventListener('click', openCardReport);
  byId('card-report-form').addEventListener('submit', sendCardReport);
  byId('close-report').addEventListener('click', () => byId('card-report-dialog').close());
  byId('refresh-reviews').addEventListener('click', () => loadReviews());
  byId('review-filter').addEventListener('change', () => loadReviews());
  byId('more-reviews').addEventListener('click', () => loadReviews(true));
  byId('open-rewards').addEventListener('click', () => openTab('achievements'));
  byId('notice-open-rewards').addEventListener('click', () => openTab('achievements'));
  byId('open-profile').addEventListener('click', () => openTab('achievements'));
  byId('start-session').addEventListener('click', () => startSession('setup-error'));
  byId('training-settings').addEventListener('change', (event) => {
    if (event.target.matches('input[name="topic"], input[name="mode"], input[name="level"]')) {
      trainingSelectionChanged = true;
      updateQuickStart();
    }
  });
  byId('open-settings').addEventListener('click', () => {
    const settings = byId('training-settings');
    settings.open = true;
    settings.scrollIntoView({ behavior: 'smooth', block: 'start' });
  });
  byId('submit-answer').addEventListener('click', submitAnswer);
  byId('open-theory').addEventListener('click', openTheory);
  byId('close-theory').addEventListener('click', closeTheory);
  byId('close-theory-bottom').addEventListener('click', closeTheory);
  byId('theory-dialog').addEventListener('click', (event) => {
    if (event.target === byId('theory-dialog')) closeTheory();
  });
  byId('next-question').addEventListener('click', nextQuestion);
  byId('stop-session').addEventListener('click', saveAndGoHome);
  byId('finish-session').addEventListener('click', stopSession);
  byId('back-home').addEventListener('click', async () => {
    try {
      await refreshHome();
    } catch (error) {
      byId('completed-copy').textContent = error.message;
    }
  });
  byId('clear-topics').addEventListener('click', () => {
    trainingSelectionChanged = true;
    updateQuickStart();
    for (const input of document.querySelectorAll('input[name="topic"]')) input.checked = false;
  });
  byId('frame-list').addEventListener('change', (event) => {
    if (event.target.name === 'profile-frame') selectProfileFrame(event.target.value);
  });
  for (const button of document.querySelectorAll('.tab-button')) {
    button.addEventListener('click', () => openTab(button.dataset.tab));
  }
  tg?.BackButton?.onClick?.(() => {
    if (byId('card-report-dialog').open) byId('card-report-dialog').close();
    else if (!byId('theory-dialog').hidden) closeTheory();
    else saveAndGoHome();
  });
  boot();
})();
