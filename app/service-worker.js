// Cache-first for the app shell only - live data always comes fresh over MQTT,
// never through this cache.
//
// IMPORTANT: bump this version string on every change to index.html/app.js/
// style.css/manifest.json (the SHELL list below), not just when SHELL itself
// changes. Browsers only re-check the app shell when this file's own bytes
// change - if only e.g. app.js changed, installed PWAs would keep serving the
// stale cached version indefinitely otherwise.
const CACHE = 'hms-gw-s3-remote-v7';
const SHELL = [
  './',
  './index.html',
  './style.css',
  './app.js',
  './manifest.json',
  './icon.svg',
  'https://unpkg.com/mqtt@5.15.2/dist/mqtt.min.js',
];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  event.respondWith(
    caches.match(event.request).then((cached) => cached || fetch(event.request))
  );
});
