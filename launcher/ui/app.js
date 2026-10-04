// State
let launcherState = {
  status: 'CHECKING', // 'CHECKING' | 'READY' | 'UPDATE_AVAILABLE' | 'NOT_INSTALLED' | 'DOWNLOADING' | 'EXTRACTING' | 'PLAYING' | 'ERROR'
  localVersion: 'v0.0.0',
  latestVersion: '',
  isGameInstalled: false,
  isGameRunning: false,
  gamePath: '',
  releaseInfo: null
};

// Elements
const elTitlebarDrag = document.getElementById('titlebar');
const elBtnMin = document.getElementById('btn-minimize');
const elBtnClose = document.getElementById('btn-close');

const elHeroVersionBadge = document.getElementById('hero-version-badge');
const elReleaseTag = document.getElementById('release-tag');
const elReleaseTitle = document.getElementById('release-title');
const elReleaseDate = document.getElementById('release-date');
const elReleaseBody = document.getElementById('release-body');

const elSettingGamePath = document.getElementById('setting-game-path');
const elBtnOpenFolder = document.getElementById('btn-open-folder');
const elBtnRecheck = document.getElementById('btn-recheck');
const elBtnOpenGithub = document.getElementById('btn-open-github');

const elStatusIcon = document.getElementById('status-icon');
const elStatusHeadline = document.getElementById('status-headline');
const elStatusDetail = document.getElementById('status-detail');

const elProgressContainer = document.getElementById('progress-container');
const elProgressFill = document.getElementById('progress-fill');
const elProgressText = document.getElementById('progress-text');
const elProgressSpeed = document.getElementById('progress-speed');

const elBtnCancel = document.getElementById('btn-cancel');
const elBtnMainAction = document.getElementById('btn-main-action');
const elMainBtnText = document.getElementById('main-btn-text');
const elMainBtnIcon = document.getElementById('main-btn-icon');

// IPC Bridge
function sendNativeMessage(action, payload = {}) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage({ action, payload });
  } else {
    console.log('[NativeBridge mock]', action, payload);
  }
}

// Window Controls
if (elBtnMin) {
  elBtnMin.addEventListener('click', () => sendNativeMessage('WINDOW_MINIMIZE'));
}
if (elBtnClose) {
  elBtnClose.addEventListener('click', () => sendNativeMessage('WINDOW_CLOSE'));
}

// Native window dragging on titlebar
elTitlebarDrag.addEventListener('mousedown', (e) => {
  // Do not initiate drag if clicking buttons
  if (e.target.closest('.window-controls') || e.target.closest('button')) return;
  if (e.button === 0) { // Left click
    sendNativeMessage('WINDOW_DRAG');
  }
});

// Tab Switching
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));

    btn.classList.add('active');
    const targetPane = document.getElementById(btn.dataset.tab);
    if (targetPane) targetPane.classList.add('active');
  });
});

// Settings & Actions
if (elBtnOpenFolder) {
  elBtnOpenFolder.addEventListener('click', () => sendNativeMessage('OPEN_FOLDER'));
}
if (elBtnRecheck) {
  elBtnRecheck.addEventListener('click',  () => {
    setStatus('CHECKING', 'Проверка обновлений...', 'Подключение к GitHub API...');
    sendNativeMessage('CHECK_UPDATE');
  });
}
if (elBtnOpenGithub) {
  elBtnOpenGithub.addEventListener('click', () => {
    sendNativeMessage('OPEN_LINK', { url: 'https://github.com/Welloty/launcher' });
  });
}
if (elBtnCancel) {
  elBtnCancel.addEventListener('click', () => {
    sendNativeMessage('CANCEL_UPDATE');
  });
}

// Main Action Button
elBtnMainAction.addEventListener('click', () => {
  switch (launcherState.status) {
    case 'READY':
      sendNativeMessage('LAUNCH_GAME');
      break;
    case 'UPDATE_AVAILABLE':
    case 'NOT_INSTALLED':
      sendNativeMessage('START_UPDATE');
      break;
    default:
      break;
  }
});

// Helpers
function formatBytes(bytes) {
  if (!bytes || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return (bytes / Math.pow(1024, i)).toFixed(1) + ' ' + units[i];
}

// Update UI state
function setStatus(status, headline, detail) {
  launcherState.status = status;
  if (headline) elStatusHeadline.textContent = headline;
  if (detail) elStatusDetail.textContent = detail;

  // Icon
  elStatusIcon.innerHTML = '';
  const indicator = document.createElement('span');
  indicator.className = 'status-indicator';

  switch (status) {
    case 'READY':
      indicator.classList.add('ready');
      elBtnMainAction.disabled = false;
      elMainBtnText.textContent = 'Играть';
      elMainBtnIcon.innerHTML = `
        <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor">
          <polygon points="5 3 19 12 5 21 5 3"/>
        </svg>`;
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'UPDATE_AVAILABLE':
      indicator.classList.add('busy');
      elBtnMainAction.disabled = false;
      elMainBtnText.textContent = 'Обновить';
      elMainBtnIcon.innerHTML = `
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5">
          <path d="M12 4v12m0 0l-4-4m4 4l4-4M4 18v2h16v-2"/>
        </svg>`;
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'NOT_INSTALLED':
      indicator.classList.add('busy');
      elBtnMainAction.disabled = false;
      elMainBtnText.textContent = 'Скачать';
      elMainBtnIcon.innerHTML = `
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5">
          <path d="M12 4v12m0 0l-4-4m4 4l4-4M4 18v2h16v-2"/>
        </svg>`;
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'DOWNLOADING':
      indicator.classList.add('busy');
      elBtnMainAction.disabled = true;
      elMainBtnText.textContent = 'Загрузка...';
      elMainBtnIcon.innerHTML = '';
      elProgressContainer.classList.add('active');
      elBtnCancel.style.display = 'inline-flex';
      break;

    case 'EXTRACTING':
      indicator.classList.add('busy');
      elBtnMainAction.disabled = true;
      elMainBtnText.textContent = 'Установка...';
      elMainBtnIcon.innerHTML = '';
      elProgressContainer.classList.add('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'PLAYING':
      indicator.classList.add('ready');
      elBtnMainAction.disabled = true;
      elMainBtnText.textContent = 'В игре';
      elMainBtnIcon.innerHTML = '';
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'CHECKING':
      indicator.classList.add('busy');
      elBtnMainAction.disabled = true;
      elMainBtnText.textContent = 'Проверка...';
      elMainBtnIcon.innerHTML = '';
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;

    case 'ERROR':
      indicator.classList.add('error');
      elBtnMainAction.disabled = false;
      elMainBtnText.textContent = 'Повторить';
      elMainBtnIcon.innerHTML = '';
      elProgressContainer.classList.remove('active');
      elBtnCancel.style.display = 'none';
      break;
  }

  elStatusIcon.appendChild(indicator);
}

// Receive messages from C++ Backend
function handleNativeMessage(msg) {
  if (!msg || !msg.event) return;
  const { event, data } = msg;

  switch (event) {
    case 'INIT_STATE':
      launcherState.localVersion = data.localVersion || 'v0.0.0';
      launcherState.isGameInstalled = !!data.isGameInstalled;
      launcherState.isGameRunning = !!data.isGameRunning;
      launcherState.gamePath = data.gamePath || '';

      elHeroVersionBadge.textContent = launcherState.localVersion;
      if (elSettingGamePath) elSettingGamePath.textContent = launcherState.gamePath;

      if (launcherState.isGameRunning) {
        setStatus('PLAYING', 'Игра запущена', 'Fireline активна');
      } else if (!launcherState.isGameInstalled) {
        setStatus('NOT_INSTALLED', 'Игра не установлена', 'Требуется начальная установка');
      } else {
        setStatus('READY', 'Готов к запуску', `Текущая версия: ${launcherState.localVersion}`);
      }
      break;

    case 'UPDATE_AVAILABLE':
      launcherState.latestVersion = data.latestVersion;
      launcherState.releaseInfo = data;

      if (elHeroVersionBadge) elHeroVersionBadge.textContent = data.latestVersion;
      if (elReleaseTag) elReleaseTag.textContent = data.latestVersion;
      if (elReleaseTitle) elReleaseTitle.textContent = data.title || 'Новое обновление доступно';
      if (elReleaseDate) elReleaseDate.textContent = data.publishedAt ? new Date(data.publishedAt).toLocaleDateString('ru-RU') : '';
      if (elReleaseBody) elReleaseBody.textContent = data.body || 'Описание релиза отсутствует.';

      if (!launcherState.isGameInstalled) {
        setStatus('NOT_INSTALLED', 'Доступна новая версия для загрузки', `Размер: ${formatBytes(data.assetSize)}`);
      } else {
        setStatus('UPDATE_AVAILABLE', `Доступна версия ${data.latestVersion}`, `Текущая: ${launcherState.localVersion} • Размер: ${formatBytes(data.assetSize)}`);
      }
      break;

    case 'UPDATE_NOT_FOUND':
      if (data.title) {
        if (elReleaseTag) elReleaseTag.textContent = data.latestVersion || launcherState.localVersion;
        if (elReleaseTitle) elReleaseTitle.textContent = data.title;
        if (elReleaseDate) elReleaseDate.textContent = data.publishedAt ? new Date(data.publishedAt).toLocaleDateString('ru-RU') : '';
        if (elReleaseBody) elReleaseBody.textContent = data.body || 'У вас установлена самая свежая версия.';
      }

      if (launcherState.isGameInstalled) {
        setStatus('READY', 'Установлена последняя версия', `Версия игры: ${launcherState.localVersion}`);
      } else {
        setStatus('NOT_INSTALLED', 'Игра не установлена', 'Файлы игры не найдены');
      }
      break;

    case 'DOWNLOAD_PROGRESS':
      setStatus('DOWNLOADING', 'Загрузка обновления...', `${formatBytes(data.downloadedBytes)} из ${formatBytes(data.totalBytes)}`);
      elProgressFill.style.width = `${Math.min(100, Math.max(0, data.percent))}%`;
      elProgressText.textContent = `Скачивание: ${data.percent}%`;
      elProgressSpeed.textContent = `${formatBytes(data.downloadedBytes)} / ${formatBytes(data.totalBytes)} (${data.speedMbS.toFixed(1)} MB/s)`;
      break;

    case 'EXTRACT_PROGRESS':
      setStatus('EXTRACTING', 'Распаковка и установка...', `Файлы: ${data.extractedFiles} из ${data.totalFiles}`);
      elProgressFill.style.width = `${Math.min(100, Math.max(0, data.percent))}%`;
      elProgressText.textContent = `Распаковка: ${data.percent}%`;
      elProgressSpeed.textContent = data.currentFile ? data.currentFile.split('/').pop() : '';
      break;

    case 'UPDATE_COMPLETE':
      launcherState.localVersion = data.version;
      launcherState.isGameInstalled = true;
      elHeroVersionBadge.textContent = data.version;
      setStatus('READY', 'Обновление завершено!', `Установлена версия: ${data.version}`);
      break;

    case 'GAME_STARTED':
      launcherState.isGameRunning = true;
      setStatus('PLAYING', 'Игра запущена', 'Fireline выполняется в фоновом режиме');
      break;

    case 'GAME_STOPPED':
      launcherState.isGameRunning = false;
      setStatus('READY', 'Игра завершена', `Готов к повторному запуску (${launcherState.localVersion})`);
      break;

    case 'ERROR':
      setStatus('ERROR', 'Произошла ошибка', data.message || 'Неизвестная ошибка');
      break;
  }
}

// Bind native listener
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (event) => {
    let data = event.data;
    if (typeof data === 'string') {
      try { data = JSON.parse(data); } catch (e) {}
    }
    handleNativeMessage(data);
  });
}

// Tell native side that UI is ready
window.addEventListener('DOMContentLoaded', () => {
  sendNativeMessage('UI_READY');
});
