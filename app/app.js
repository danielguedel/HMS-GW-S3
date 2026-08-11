// HMS-GW-S3 Remote PWA
// Talks directly to the cloud MQTT broker over WebSockets (wss://) - no backend.
// Topic layout matches taskMQTT.cpp's non-OpenDTU schema: individual topics under
// "<topicPrefix>/...", not a single JSON payload. See publishPvData()/publishGpioState()
// in src/taskMQTT.cpp for the authoritative list.

const STORAGE_KEY = 'hms-gw-s3-remote-cfg';
const LANG_KEY = 'hms-gw-s3-remote-lang';
const $ = id => document.getElementById(id);

let client = null;
let cfg = null;

// --- i18n (EN/DE) --------------------------------------------------------------
// Same key names/wording as data/www/index.html's I18N object where a shared
// concept exists (Relay/IO/status labels etc), so both UIs read identically.
// Persisted via localStorage instead of the gateway's /api/config/lang used by
// the dashboard, since the PWA has no REST access to the device - only MQTT.
const I18N = {
  en: {
    btnSettings: 'Settings', secLive: 'Live', secStatus: 'Status', secSystem: 'System',
    secControl: 'Control', secRelayIo: 'Relay / IO', secPowerLimit: 'Power Limit',
    statTemp: 'Temp', statLimit: 'Limit', statYieldToday: 'Yield today', statTotal: 'Total',
    statActive: 'Active', statStandby: 'Standby', statNoData: 'No data', statOffline: 'Offline',
    btnSet: 'Set', lblRelay: 'Relay', lblIo1: 'IO1', lblIo2: 'IO2', lblIo3: 'IO3',
    lblOn: 'ON', lblOff: 'OFF',
    lblBrokerHost: 'Broker Host', lblPortWs: 'Port (WebSocket)',
    lblUsername: 'Username', lblPassword: 'Password', lblTopic: 'Topic',
    btnCancel: 'Cancel', btnSaveConnect: 'Save & Connect',
    connConnecting: 'Connecting…', connConnected: 'Connected', connReconnecting: 'Reconnecting…',
    connDisconnected: 'Disconnected', connError: 'Error', connNotConfigured: 'Not configured',
    toastHostTopicRequired: 'Host and topic are required', toastNotConnected: 'Not connected',
  },
  de: {
    btnSettings: 'Einstellungen', secLive: 'Live', secStatus: 'Status', secSystem: 'System',
    secControl: 'Steuerung', secRelayIo: 'Relais / E/A', secPowerLimit: 'Leistungsbegrenzung',
    statTemp: 'Temp', statLimit: 'Limit', statYieldToday: 'Ertrag heute', statTotal: 'Gesamt',
    statActive: 'Aktiv', statStandby: 'Standby', statNoData: 'Keine Daten', statOffline: 'Offline',
    btnSet: 'Setzen', lblRelay: 'Relais', lblIo1: 'EA1', lblIo2: 'EA2', lblIo3: 'EA3',
    lblBrokerHost: 'Broker Host', lblPortWs: 'Port (WebSocket)',
    lblUsername: 'Benutzername', lblPassword: 'Passwort', lblTopic: 'Topic',
    btnCancel: 'Abbrechen', btnSaveConnect: 'Speichern & Verbinden',
    connConnecting: 'Verbinde…', connConnected: 'Verbunden', connReconnecting: 'Verbinde neu…',
    connDisconnected: 'Getrennt', connError: 'Fehler', connNotConfigured: 'Nicht konfiguriert',
    toastHostTopicRequired: 'Host und Topic sind erforderlich', toastNotConnected: 'Nicht verbunden',
  },
};
let _lang = (localStorage.getItem(LANG_KEY) === 'de') ? 'de' : 'en';

// Looks up `key` in the active language, falling back to English, then the key
// itself if truly missing (never throws / never shows "undefined").
function tr(key) { return I18N[_lang][key] ?? I18N.en[key] ?? key; }

// Applies the active language to every element carrying data-i18n, and updates
// the lang toggle button to show the language you'd switch *to* (matching
// data/www/index.html's lang-btn convention: shows the other language's code).
function applyI18n() {
  document.documentElement.lang = _lang;
  document.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = tr(el.dataset.i18n); });
  $('lang-btn').textContent = _lang === 'en' ? 'DE' : 'EN';
}

// Toggles EN<->DE, persists the choice, and re-renders both the static markup
// and anything currently showing dynamically-built translated text (connection
// label, status badge).
function toggleLang() {
  _lang = _lang === 'en' ? 'de' : 'en';
  localStorage.setItem(LANG_KEY, _lang);
  applyI18n();
  if (_connLabelKey) $('dot-conn').title = tr(_connLabelKey);
  updateStatusBadge();
  Object.keys(gpioState).forEach(renderGpioLabel);
}
$('lang-btn').addEventListener('click', toggleLang);

// Reads the saved broker connection settings from localStorage; returns null if
// none are stored yet or the JSON is corrupt (never throws).
function loadCfg() {
  try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null'); }
  catch { return null; }
}
// Persists the broker connection settings to localStorage (never sent anywhere else).
function saveCfg(c) { localStorage.setItem(STORAGE_KEY, JSON.stringify(c)); }

// Opens the settings overlay, pre-filled from the currently active cfg, or blank
// on first run (no cfg yet).
function openSettings() {
  const c = cfg || {};
  $('cf-host').value  = c.host  || '';
  $('cf-port').value  = c.port  || 9001;
  $('cf-user').value  = c.user  || '';
  $('cf-pass').value  = c.pass  || '';
  $('cf-topic').value = c.topic || '';
  $('settings').classList.add('open');
}
function closeSettings() { $('settings').classList.remove('open'); }

// Validates and stores the settings form, then (re)connects with the new values.
$('btn-settings').addEventListener('click', openSettings);
$('btn-cancel').addEventListener('click', closeSettings);
$('btn-save').addEventListener('click', () => {
  const next = {
    host:  $('cf-host').value.trim(),
    port:  +$('cf-port').value || 9001,
    user:  $('cf-user').value.trim(),
    pass:  $('cf-pass').value,
    topic: $('cf-topic').value.trim(),
  };
  if (!next.host || !next.topic) { toast(tr('toastHostTopicRequired')); return; }
  saveCfg(next);
  cfg = next;
  closeSettings();
  connect();
});

// Shows a transient toast notification, auto-hiding after 2.5s; re-triggering
// before that resets the timer instead of stacking hides. `ok` toggles the
// border/glow color (cyan/pink), same convention as data/www/index.html's toast().
function toast(msg, ok = true) {
  const t = $('toast');
  t.textContent = msg;
  t.style.borderColor = ok ? 'var(--cyan)' : 'var(--pink)';
  t.style.boxShadow = ok ? '0 0 24px -4px var(--cyan)' : '0 0 24px -4px var(--pink)';
  t.classList.add('show');
  clearTimeout(toast._h);
  toast._h = setTimeout(() => t.classList.remove('show'), 2500);
}

// Updates the header MQTT status dot to reflect the current connection state
// (color only, fixed "MQTT" label - same convention as data/www/index.html's
// WiFi/DTU/MQTT dots), then re-applies card glow (see powerGlow()) so Grid/PV1/PV2
// flip to/from pink immediately on connect/disconnect rather than waiting for new
// data. Takes an I18N key (not literal text) so toggleLang() can re-translate the
// dot's tooltip without needing to know what it was.
let _connLabelKey = null;
function setConn(ok, labelKey) {
  _connLabelKey = labelKey;
  $('dot-conn').classList.toggle('ok', ok);
  $('dot-conn').classList.toggle('err', !ok);
  $('dot-conn').title = tr(labelKey);
  updateGridCard(); updatePvCard(0); updatePvCard(1);
}

// Converts a power value to a clamped 0-100% CSS width string for the bar gauges.
function pct(val, max) {
  const v = Math.max(0, Math.min(100, (val / max) * 100));
  return v + '%';
}

// Grid/PV1/PV2 card border+text glow scales with power - same thresholds as
// data/www/index.html's powerGlow(). Caller decides the disconnected/inactive
// fallback (see updateStatusBadge()), matching that dashboard's convention.
function powerGlow(p) {
  if (p < 250) return 'var(--cyan)';
  if (p < 500) return 'var(--purple)';
  return 'var(--pink)';
}

// --- Live data state (assembled incrementally, one MQTT message at a time) ----
const live = { grid: {}, pv0: {}, pv1: {}, inverter: {}, system: {} };

// Renders the MAC/FW/Build part of the header info bar from live.system, once
// both fields have arrived (published retained on connect, see taskMQTT.cpp's
// publishSystemInfo()) - same "<fw> (Build <build>)" format as data/www/index.html.
function renderSystemInfo() {
  if (live.system.mac !== undefined) $('hi-mac').textContent = live.system.mac;
  if (live.system.fw !== undefined && live.system.build !== undefined)
    $('hi-fw').textContent = `${live.system.fw} (Build ${live.system.build})`;
}

// Header clock - purely client-side (the browser's own time), same as
// data/www/index.html's tickClock(): the device doesn't publish a live clock.
const pad2 = n => String(n).padStart(2, '0');
function tickClock() {
  const now = new Date();
  $('hi-time').textContent = `${pad2(now.getHours())}:${pad2(now.getMinutes())}:${pad2(now.getSeconds())}`;
  $('hi-date').textContent = `${pad2(now.getDate())}.${pad2(now.getMonth() + 1)}.${now.getFullYear()}`;
}
tickClock();
setInterval(tickClock, 1000);

// Re-renders the Grid card from the current `live` state; fields that haven't
// arrived over MQTT yet are left at their previous (or initial "–") value. Glow
// is always refreshed (even with no new data) so a connection-status change
// alone flips the card color without waiting for the next MQTT message.
function updateGridCard() {
  if (live.grid.P !== undefined) {
    $('grid-p').textContent = Number(live.grid.P).toFixed(0);
    $('bar-grid').style.width = pct(live.grid.P, 900);
  }
  if (live.grid.U !== undefined && live.grid.I !== undefined)
    $('grid-ui').textContent = `${Number(live.grid.U).toFixed(1)} V / ${Number(live.grid.I).toFixed(2)} A`;
  updateStatusBadge();
}

// Updates the Active/Standby/No data/Offline badge, matching data/www/index.html's
// updateStatusBadge() states - "active" is derived from grid power > 0, since the
// PWA has no separate WiFi/DTU signal of its own, only what MQTT carries. Also
// re-applies card border/glow: cyan while active, pink for standby/no-data/offline
// on every dashboard card except Power Limit (its glow tracks pending/confirmed
// instead, see updateLimitGlow()), with Grid/PV1/PV2 additionally power-tiered
// while active - same split as that dashboard's updateStatusBadge().
function updateStatusBadge() {
  const connected = !!(client && client.connected);
  const gridP = live.grid.P;
  let key, cls;
  if (!connected)               { key = 'statOffline'; cls = 'b-err'; }
  else if (gridP === undefined) { key = 'statNoData';  cls = 'b-off'; }
  else if (Number(gridP) > 0)   { key = 'statActive';  cls = 'b-ok';  }
  else                           { key = 'statStandby'; cls = 'b-off'; }
  const el = $('s-active');
  el.textContent = tr(key);
  el.className = 'badge badge-lg ' + cls;

  const active = key === 'statActive';
  const statusGlow = active ? 'var(--cyan)' : 'var(--pink)';
  document.querySelectorAll('main .card:not(#card-limit)').forEach(c => c.style.setProperty('--glow', statusGlow));
  $('card-grid').style.setProperty('--glow', active ? powerGlow(Number(live.grid.P) || 0) : 'var(--pink)');
  $('card-pv0').style.setProperty('--glow', active ? powerGlow(Number(live.pv0.P) || 0) : 'var(--pink)');
  $('card-pv1').style.setProperty('--glow', active ? powerGlow(Number(live.pv1.P) || 0) : 'var(--pink)');
}
// Re-renders the PV1 (n=0) or PV2 (n=1) card, same partial-update + glow-refresh behavior as updateGridCard().
function updatePvCard(n) {
  const d = live['pv' + n];
  if (d.P !== undefined) {
    $(`pv${n}-p`).textContent = Number(d.P).toFixed(0);
    $(`bar-pv${n}`).style.width = pct(d.P, 450);
  }
  if (d.U !== undefined && d.I !== undefined)
    $(`pv${n}-ui`).textContent = `${Number(d.U).toFixed(1)} V / ${Number(d.I).toFixed(2)} A`;
  updateStatusBadge();
}
// Re-renders the temperature/power-limit/energy cards from the current `live` state.
function updateSystemCards() {
  const inv = live.inverter;
  if (inv.Temp !== undefined)       $('s-temp').textContent  = Number(inv.Temp).toFixed(1);
  if (inv.PowerLimit !== undefined) $('s-limit').textContent = inv.PowerLimit;
  if (live.grid.dailyEnergy !== undefined) $('s-de').textContent = Number(live.grid.dailyEnergy).toFixed(3);
  if (live.grid.totalEnergy !== undefined) $('s-te').textContent = Number(live.grid.totalEnergy).toFixed(1);
}

// Guards against re-publishing a control the moment we receive its own retained
// state back from the broker (would otherwise loop set -> state -> set -> ...).
let suppressPublish = false;

// Syncs the power-limit slider to the broker's PowerLimitTarget, unless the user
// is actively dragging it (checked via document.activeElement). Slider/label turn
// pink ("pending") while PowerLimitTarget hasn't been confirmed by PowerLimit yet -
// same convention as data/www/index.html's updateLimitGlow(). Toasts once when a
// pending target flips to confirmed, matching that dashboard's refreshData().
let _limitWasPending = false;
function updateLimitRange() {
  const target = live.inverter.PowerLimitTarget;
  if (target === undefined) return;
  const pending = target !== live.inverter.PowerLimit;
  $('limit-range').classList.toggle('pending', pending);
  $('range-val').classList.toggle('pending', pending);
  $('bar-limit').classList.toggle('pending', pending);
  $('bar-limit').style.width = pct(target, 100);
  updateLimitGlow(pending);
  if (_limitWasPending && !pending) toast('Limit → ' + target + '%');
  _limitWasPending = pending;
  if (document.activeElement === $('limit-range')) return; // don't fight the user mid-drag
  suppressPublish = true;
  $('limit-range').value = target;
  $('range-val').textContent = target + ' %';
  suppressPublish = false;
}
// Power Limit card glow: pink while the sent setpoint hasn't been confirmed by the
// DTU yet, cyan once it has - independent of connectivity-based statusGlow (see
// updateStatusBadge()), same convention as data/www/index.html's updateLimitGlow().
function updateLimitGlow(pending) {
  $('card-limit').style.setProperty('--glow', pending ? 'var(--pink)' : 'var(--cyan)');
  $('btn-limit-set').classList.toggle('text-danger', pending);
  $('btn-limit-set').classList.toggle('text-ok', !pending);
}
// Syncs a toggle switch + its ON/OFF badge + card border glow to its retained
// MQTT state topic (key: 'relay'/'io1'/'io2'/'io3', matching the
// sw-<key>/lbl-<key>/card-<key> element ids), without re-triggering the switch's
// own 'change' publish handler (see suppressPublish above). Same badge text/class
// + card.active convention as data/www/index.html's applyGpio().
const gpioState = {};
function updateSwitch(key, val) {
  if (val === undefined) return;
  const on = (val === '1' || val === 1 || val === true);
  gpioState[key] = on;
  suppressPublish = true;
  $('sw-' + key).checked = on;
  suppressPublish = false;
  renderGpioLabel(key);
}
function renderGpioLabel(key) {
  const lbl = $('lbl-' + key);
  const card = $('card-' + key);
  if (gpioState[key] === undefined) return;
  if (lbl) {
    lbl.textContent = tr(gpioState[key] ? 'lblOn' : 'lblOff');
    lbl.className = 'badge ' + (gpioState[key] ? 'b-ok' : 'b-off');
  }
  if (card) card.classList.toggle('active', gpioState[key]);
}

// Routes one incoming MQTT message to the matching `live` field and re-render,
// based on taskMQTT.cpp's non-OpenDTU topic layout (see file header). Messages
// outside cfg.topic are ignored - shouldn't happen given the subscribed filter
// is `<topic>/#`, but defensive since the broker could carry other topics too.
function onMqttMessage(topic, payloadBuf) {
  const prefix = cfg.topic + '/';
  if (!topic.startsWith(prefix)) return;
  const rel = topic.slice(prefix.length);
  const val = payloadBuf.toString();

  switch (rel) {
    case 'grid/P':  live.grid.P  = val; updateGridCard(); break;
    case 'grid/U':  live.grid.U  = val; updateGridCard(); break;
    case 'grid/I':  live.grid.I  = val; updateGridCard(); break;
    case 'grid/dailyEnergy': live.grid.dailyEnergy = val; updateSystemCards(); break;
    case 'grid/totalEnergy': live.grid.totalEnergy = val; updateSystemCards(); break;

    case 'pv0/P': live.pv0.P = val; updatePvCard(0); break;
    case 'pv0/U': live.pv0.U = val; updatePvCard(0); break;
    case 'pv0/I': live.pv0.I = val; updatePvCard(0); break;
    case 'pv1/P': live.pv1.P = val; updatePvCard(1); break;
    case 'pv1/U': live.pv1.U = val; updatePvCard(1); break;
    case 'pv1/I': live.pv1.I = val; updatePvCard(1); break;

    case 'inverter/Temp':             live.inverter.Temp = val; updateSystemCards(); break;
    case 'inverter/PowerLimit':       live.inverter.PowerLimit = val; updateSystemCards(); updateLimitRange(); break;
    case 'inverter/PowerLimitTarget': live.inverter.PowerLimitTarget = val; updateLimitRange(); break;

    case 'relay/state': updateSwitch('relay', val); break;
    case 'io1/state':   updateSwitch('io1', val); break;
    case 'io2/state':   updateSwitch('io2', val); break;
    case 'io3/state':   updateSwitch('io3', val); break;

    case 'system/fw':    live.system.fw    = val; renderSystemInfo(); break;
    case 'system/build': live.system.build = val; renderSystemInfo(); break;
    case 'system/mac':   live.system.mac   = val; renderSystemInfo(); break;
  }
}

// --- Controls -> publish -------------------------------------------------------
// Publishes a control command under `<topic>/<suffix>`; toasts instead of
// throwing if there's currently no live connection to publish on. Returns
// whether the publish actually happened, so callers can decide whether to
// optimistically reflect the change (matching data/www/index.html's setGpio()/
// setLimit(), which only update the UI after their fetch() resolves).
function pub(suffix, value) {
  if (!client || !client.connected) { toast(tr('toastNotConnected')); return false; }
  client.publish(`${cfg.topic}/${suffix}`, String(value));
  return true;
}

// Dragging updates the visible percentage and bar fill (but sends nothing until
// "Set" is clicked) - matches data/www/index.html's onLimitInput().
$('limit-range').addEventListener('input', () => {
  const v = $('limit-range').value;
  $('range-val').textContent = v + ' %';
  $('bar-limit').style.width = pct(v, 100);
});
// Optimistically shows the sent value as "pending" (pink) right away, same as
// data/www/index.html's setLimit() - it's re-confirmed (or corrected) once the
// retained PowerLimit topic catches up, see updateLimitRange().
$('btn-limit-set').addEventListener('click', () => {
  const val = $('limit-range').value;
  if (!pub('inverter/PowerLimitSet/set', val)) return;
  $('range-val').textContent = val + ' %';
  $('bar-limit').style.width = pct(val, 100);
  $('range-val').classList.add('pending');
  $('limit-range').classList.add('pending');
  $('bar-limit').classList.add('pending');
  updateLimitGlow(true);
  toast('Limit → ' + val + '%', false);
});

// Optimistically updates the badge/border the moment a command is accepted for
// publish, same as data/www/index.html's setGpio() calling applyGpio() right
// after its fetch() resolves - the later retained state topic just re-confirms it.
const gpioLabelKey = { relay: 'lblRelay', io1: 'lblIo1', io2: 'lblIo2', io3: 'lblIo3' };
[['sw-relay', 'relay', 'relay/set'], ['sw-io1', 'io1', 'io1/set'], ['sw-io2', 'io2', 'io2/set'], ['sw-io3', 'io3', 'io3/set']]
  .forEach(([id, key, topic]) => {
    $(id).addEventListener('change', () => {
      if (suppressPublish) return;
      const on = $(id).checked;
      if (!pub(topic, on ? '1' : '0')) return;
      gpioState[key] = on;
      renderGpioLabel(key);
      toast(tr(gpioLabelKey[key]).toUpperCase() + ' → ' + tr(on ? 'lblOn' : 'lblOff'));
    });
  });

// --- Connection ------------------------------------------------------------
// (Re)connects to the broker over WSS, tearing down any existing client first;
// subscribes to the full `<topic>/#` tree on connect. Reconnection after a drop
// is handled by mqtt.js itself (reconnectPeriod below), not re-implemented here.
function connect() {
  if (client) { client.end(true); client = null; }
  setConn(false, 'connConnecting');

  const url = `wss://${cfg.host}:${cfg.port}`;
  client = mqtt.connect(url, {
    username: cfg.user,
    password: cfg.pass,
    reconnectPeriod: 4000,
    connectTimeout: 10000,
    clientId: 'hmsgws3-remote-' + Math.random().toString(16).slice(2, 8),
  });

  client.on('connect', () => {
    setConn(true, 'connConnected');
    client.subscribe(cfg.topic + '/#');
  });
  client.on('reconnect', () => setConn(false, 'connReconnecting'));
  client.on('close',     () => setConn(false, 'connDisconnected'));
  client.on('error', (err) => { setConn(false, 'connError'); console.error('MQTT error', err); });
  client.on('message', onMqttMessage);
}

// --- Boot --------------------------------------------------------------------
// Auto-connects if valid settings are already stored; otherwise opens the
// settings overlay so first-run users are prompted immediately.
applyI18n();
cfg = loadCfg();
if (cfg && cfg.host && cfg.topic) {
  connect();
} else {
  setConn(false, 'connNotConfigured');
  openSettings();
}

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('service-worker.js').catch(() => {});
  });
}
