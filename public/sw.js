// NuiGraph Studio service worker.
//
// Scope is deliberately narrow: only the static shell is cached. Everything
// under /api/ is per-session data owned by whoever holds the session cookie,
// and the Cache API is origin-wide and outlives the cookie, so caching a
// response there would hand one visitor's diagrams to the next person using
// the same browser profile. Those requests are never touched.

const VERSION = "ngs-v1";
const SHELL = [
  "/styles.css",
  "/app.js",
  "/editor.js",
  "/manifest.webmanifest",
  "/icons/icon.svg",
  "/icons/icon-maskable.svg"
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(VERSION)
      // A missing asset must not abort the whole install, so each is added
      // individually and failures are tolerated.
      .then((cache) => Promise.allSettled(SHELL.map((url) => cache.add(url))))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  const { request } = event;
  if (request.method !== "GET") return;

  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;

  // Session data, and the login and logout endpoints, always go to the network.
  if (url.pathname.startsWith("/api/") ||
      url.pathname === "/login" ||
      url.pathname === "/logout" ||
      url.pathname === "/health") {
    return;
  }

  // Navigations are network-first: the editor shell carries the signed-in
  // username, so a cached copy is only ever an offline fallback.
  if (request.mode === "navigate") {
    event.respondWith(
      fetch(request).catch(() => caches.match("/offline.html").then((r) => r || Response.error()))
    );
    return;
  }

  // Static assets are served from cache and refreshed in the background, so a
  // deploy is picked up on the visit after it lands. The server sends ETag and
  // no-cache, so the revalidation is usually a cheap 304.
  event.respondWith(
    caches.match(request).then((cached) => {
      const network = fetch(request)
        .then((response) => {
          if (response && response.ok && response.type === "basic") {
            const copy = response.clone();
            caches.open(VERSION).then((cache) => cache.put(request, copy));
          }
          return response;
        })
        .catch(() => cached);
      return cached || network;
    })
  );
});
