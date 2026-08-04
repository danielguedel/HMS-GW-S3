// HMS-GW-S3 Remote PWA
// Talks directly to the cloud MQTT broker over WebSockets (wss://) - no backend.
// Topic layout matches taskMQTT.cpp's non-OpenDTU schema: individual topics under
// "<topicPrefix>/...", not a single JSON payload. See publishPvData()/publishGpioState()
// in src/taskMQTT.cpp for the authoritative list.

const STORAGE_KEY = 'hms-gw-s3-remote-cfg';
const $ = id => document.getElementById(id);

let client = null;
let cfg = null;

function loadCfg() {
  try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null'); }
  catch { return null; }
}
function saveCfg(c) { localStorage.setItem(STORAGE_KEY, JSON.stringify(c)); }

function openSettings(prefill) {
  const c = prefill || cfg || {};
  $('cf-host').value  = c.host  || '';
  $('cf-port').value  = c.port  || 9001;
  $('cf-user').value  = c.user  || '';
  $('cf-pass').value  = c.pass  || '';
  $('cf-topic').value = c.topic || '';
  $('settings').classList.add('open');
}
function closeSettings() { $('settings').classList.remove('open'); }

$('btn-settings').addEventListener('click', () => openSettings());
$('btn-cancel').addEventListener('click', closeSettings);
$('btn-save').addEventListener('click', () => {
  const next = {
    host:  $('cf-host').value.trim(),
    port:  +$('cf-port').value || 9001,
    user:  $('cf-user').value.trim(),
    pass:  $('cf-pass').value,
    topic: $('cf-topic').value.trim(),
  };
  if (!next.host || !next.topic) { toast('Host and topic are required'); return; }
  saveCfg(next);
  cfg = next;
  closeSettings();
  connect();
});

function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast._h);
  toast._h = setTimeout(() => t.classList.remove('show'), 2500);
}

function setConn(ok, label) {
  $('dot-conn').classList.toggle('ok', ok);
  $('dot-conn').classList.toggle('err', !ok);
  $('conn-label').textContent = label;
}

function pct(val, max) {
  const v = Math.max(0, Math.min(100, (val / max) * 100));
  return v + '%';
}

// --- Live data state (assembled incrementally, one MQTT message at a time) ----
const live = { grid: {}, pv0: {}, pv1: {}, inverter: {} };

function updateGridCard() {
  if (live.grid.P !== undefined) {
    $('grid-p').textContent = Number(live.grid.P).toFixed(0);
    $('bar-grid').style.width = pct(live.grid.P, 900);
  }
  if (live.grid.U !== undefined && live.grid.I !== undefined)
    $('grid-ui').textContent = `${Number(live.grid.U).toFixed(1)} V / ${Number(live.grid.I).toFixed(2)} A`;
}
function updatePvCard(n) {
  const d = live['pv' + n];
  if (d.P !== undefined) {
    $(`pv${n}-p`).textContent = Number(d.P).toFixed(0);
    $(`bar-pv${n}`).style.width = pct(d.P, 450);
  }
  if (d.U !== undefined && d.I !== undefined)
    $(`pv${n}-ui`).textContent = `${Number(d.U).toFixed(1)} V / ${Number(d.I).toFixed(2)} A`;
}
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

function updateLimitRange() {
  const target = live.inverter.PowerLimitTarget;
  if (target === undefined) return;
  if (document.activeElement === $('limit-range')) return; // don't fight the user mid-drag
  suppressPublish = true;
  $('limit-range').value = target;
  $('range-val').textContent = target + ' %';
  suppressPublish = false;
}
function updateSwitch(id, val) {
  if (val === undefined) return;
  suppressPublish = true;
  $(id).checked = (val === '1' || val === 1 || val === true);
  suppressPublish = false;
}

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
    case 'inverter/PowerLimit':       live.inverter.PowerLimit = val; updateSystemCards(); break;
    case 'inverter/PowerLimitTarget': live.inverter.PowerLimitTarget = val; updateLimitRange(); break;

    case 'relay/state': updateSwitch('sw-relay', val); break;
    case 'io1/state':   updateSwitch('sw-io1', val); break;
    case 'io2/state':   updateSwitch('sw-io2', val); break;
    case 'io3/state':   updateSwitch('sw-io3', val); break;
  }
}

// --- Controls -> publish -------------------------------------------------------
function pub(suffix, value) {
  if (!client || !client.connected) { toast('Not connected'); return; }
  client.publish(`${cfg.topic}/${suffix}`, String(value));
}

$('limit-range').addEventListener('input', () => {
  $('range-val').textContent = $('limit-range').value + ' %';
});
$('limit-range').addEventListener('change', () => {
  if (suppressPublish) return;
  pub('inverter/PowerLimitSet/set', $('limit-range').value);
});

[['sw-relay', 'relay/set'], ['sw-io1', 'io1/set'], ['sw-io2', 'io2/set'], ['sw-io3', 'io3/set']]
  .forEach(([id, topic]) => {
    $(id).addEventListener('change', () => {
      if (suppressPublish) return;
      pub(topic, $(id).checked ? '1' : '0');
    });
  });

// --- Connection ------------------------------------------------------------
function connect() {
  if (client) { client.end(true); client = null; }
  setConn(false, 'Connecting…');

  const url = `wss://${cfg.host}:${cfg.port}`;
  client = mqtt.connect(url, {
    username: cfg.user,
    password: cfg.pass,
    reconnectPeriod: 4000,
    connectTimeout: 10000,
    clientId: 'hmsgws3-remote-' + Math.random().toString(16).slice(2, 8),
  });

  client.on('connect', () => {
    setConn(true, 'Connected');
    client.subscribe(cfg.topic + '/#');
  });
  client.on('reconnect', () => setConn(false, 'Reconnecting…'));
  client.on('close',     () => setConn(false, 'Disconnected'));
  client.on('error', (err) => { setConn(false, 'Error'); console.error('MQTT error', err); });
  client.on('message', onMqttMessage);
}

// --- Boot --------------------------------------------------------------------
cfg = loadCfg();
if (cfg && cfg.host && cfg.topic) {
  connect();
} else {
  setConn(false, 'Not configured');
  openSettings();
}

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('service-worker.js').catch(() => {});
  });
}
