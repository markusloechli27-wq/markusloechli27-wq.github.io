const CACHE = "sg-review-v4";
const CORE = ["./", "./index.html", "./manifest.webmanifest", "./icon-192.png", "./icon-512.png"];
self.addEventListener("install", e => { e.waitUntil(caches.open(CACHE).then(c => c.addAll(CORE))); self.skipWaiting(); });
self.addEventListener("activate", e => e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener("fetch", e => {
  if (e.request.method !== "GET") return;
  const u = new URL(e.request.url);
  // the flop data: network first (it changes with every new batch of flops), one cached copy for offline use
  if (u.origin === location.origin && /\/(cb|mr2)\.txt$/.test(u.pathname)) {
    e.respondWith(fetch(e.request).then(res => { if (res.ok) { const c = res.clone(); const f = "." + u.pathname.slice(u.pathname.lastIndexOf("/")); caches.open(CACHE).then(ca => ca.put(f, c)); } return res; }).catch(() => caches.match("." + u.pathname.slice(u.pathname.lastIndexOf("/")))));
    return;
  }
  // the page itself: network first, so updates arrive right away; cached copy when offline
  if (e.request.mode === "navigate" || (u.origin === location.origin && /\/(index\.html)?$/.test(u.pathname))) {
    e.respondWith(fetch(e.request).then(res => { if (res.ok) { const c = res.clone(); caches.open(CACHE).then(ca => ca.put("./index.html", c)); } return res; }).catch(() => caches.match("./index.html")));
    return;
  }
  e.respondWith(caches.match(e.request).then(hit => hit || fetch(e.request).then(res => {
    if (res.ok || res.type === "opaque") if (u.origin === location.origin || /fonts\.(googleapis|gstatic)\.com$/.test(u.hostname)) { const c = res.clone(); caches.open(CACHE).then(ca => ca.put(e.request, c)); }
    return res;
  }).catch(() => hit)));
});
