# Peer-to-Peer WebRTC with a 6-Character Join Code

**A reusable pattern:** one side (the *host*) shows a short code such as `K7Q-4MX`.
The other side (the *joiner*) opens a web page, types the code, and is connected
directly, peer to peer, within a second or two. The code is **persistent**: it belongs to the host
device, so it works again tomorrow with nobody re-sending anything. The joiner's browser
remembers it, so rejoining takes one tap.

The only server is a small Cloudflare Worker with a Durable Object. It runs on the free
plan. It swaps ~1 KB of **encrypted** connection data between the two peers and then
leaves the path. Media and data flow directly between the peers. The server never sees
the code, the SDP, or either peer's IP addresses in plaintext.

This works for screen sharing, camera streaming, file transfer, remote control, chat,
multiplayer games, and IoT device dashboards: anything where two endpoints need a WebRTC
link and you don't want to run a signalling server with accounts, sockets, and state.

---

## Contents

1. [How it works in one picture](#1-how-it-works-in-one-picture)
2. [The three things WebRTC needs, and which one you actually host](#2-the-three-things-webrtc-needs)
3. [The join code](#3-the-join-code)
4. [The sealed signalling blob](#4-the-sealed-signalling-blob)
5. [The rendezvous server (Cloudflare Worker + Durable Object)](#5-the-rendezvous-server)
6. [Deploying to Cloudflare on your own domain](#6-deploying-to-cloudflare)
7. [The joiner: the web page people type the code into](#7-the-joiner-web-page)
8. [The host: publish, wait, stream, re-offer](#8-the-host)
9. [Persistence: codes that survive restarts](#9-persistence)
10. [Reconnection that actually converges](#10-reconnection)
11. [Timing contract between the two sides](#11-timing-contract)
12. [Security model](#12-security-model)
13. [NAT, STUN, TURN](#13-nat-stun-turn)
14. [Testing](#14-testing)
15. [Pitfalls checklist](#15-pitfalls-checklist)
16. [Adapting it to your app](#16-adapting-it-to-your-app)

---

## 1. How it works in one picture

```
 HOST (owns the code)               RENDEZVOUS (Worker + DO)            JOINER (browser)
 ─────────────────────              ────────────────────────            ────────────────
 code = load or generate "K7Q4MX"
 room = SHA-256(code)
 pc.createOffer(), gather ALL ICE
 blob = AES-GCM(deflate(SDP), PBKDF2(code))
 POST /api/room {room, session, blob} ──►  Room[room] = {blob, session}
 poll GET /api/room/:room/answer ─────►  204 (not yet) … every 2 s
                                                                         user types K7Q-4MX
                                                                         room = SHA-256(code)
                                         Room[room]  ◄───────────────── GET /api/room/:room
                                         {blob, session} ─────────────► decrypt with code
                                                                         setRemoteDescription
                                                                         createAnswer, gather ALL ICE
                                                                         ans = AES-GCM(..., PBKDF2(code))
                                         Room[room].ans:session ◄────── POST /api/room/:room/answer
 poll ────────────────────────────────►  200 {answer}  (read once, deleted)
 decrypt, setRemoteDescription
 ══════════════ ICE / DTLS / SRTP: direct peer-to-peer, the server is no longer involved ═════════════
```

Key ideas:

* **Non-trickle ICE.** Each side waits for ICE gathering to *complete* and puts every
  candidate into its one SDP. So the whole handshake is exactly **one offer and one
  answer**, which a dumb key-value mailbox can carry.
* **The code is both the address and the key.** `SHA-256(code)` says *where* the offer is
  stored. `PBKDF2(code)` is *what decrypts it*. The code itself is never sent to the server.
* **The host is the offerer and polls. The joiner is the answerer and pushes.** Neither
  side needs a WebSocket or an open inbound port.

---

## 2. The three things WebRTC needs

| Need | What it does | Who provides it here |
|---|---|---|
| **Signalling** | Swap SDP offer/answer (incl. ICE candidates) once, before the peers can talk | **You**: the tiny rendezvous below |
| **STUN** | Tells a peer its public IP:port | Free public servers (Cloudflare, Google). No signup. |
| **TURN** | Relays traffic when both peers are behind symmetric NAT | Optional. Needed for the ~10–15% of pairs that can't connect directly. |

You only build and host signalling, and it's roughly 150 lines of Worker code.

---

## 3. The join code

### Format

* **6 characters** from a **31-symbol unambiguous alphabet**: `23456789ABCDEFGHJKMNPQRSTUVWXYZ`
  (no `0/O`, `1/I/L`), so it survives being read aloud or typed on a phone.
* Displayed as `ABC-DEF`. The hyphen is cosmetic and is stripped before hashing.
* 31⁶ ≈ **887 million** codes.

### Generation (must use a CSPRNG, must avoid modulo bias)

The code is the *only* credential, so `Math.random()` is not acceptable, and neither is a
plain `byte % 31`. 256 isn't a multiple of 31, so the first few symbols would come up
slightly more often. Reject bytes in the biased tail instead:

```js
const ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';   // 31 symbols
const LIMIT = 256 - (256 % ALPHABET.length);          // 248

function generateCode(len = 6) {
  let out = '';
  while (out.length < len) {
    const [b] = crypto.getRandomValues(new Uint8Array(1));
    if (b < LIMIT) out += ALPHABET[b % ALPHABET.length];   // reject 248..255
  }
  return out;
}

// Accept anything the user types: lower case, spaces, hyphens, look-alikes dropped.
const normalizeCode = s => (s || '').toUpperCase().split('')
  .filter(c => ALPHABET.includes(c)).join('');
const prettyCode = c => c.length > 3 ? c.slice(0, 3) + '-' + c.slice(3) : c;
```

In native code, use the OS generator (`BCryptGenRandom`, `getrandom`, `SecRandomCopyBytes`)
with the same rejection loop.

### Room id

```js
async function roomIdFor(code) {           // 64 lowercase hex chars
  const h = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(code));
  return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
}
```

### Session id

Every *connection attempt* also gets a random 16-hex-char **session id**. The room keeps
the same code forever, but answers are stored per session. That way a reconnect can never
pick up a stale answer from an earlier attempt (see §10).

---

## 4. The sealed signalling blob

A raw SDP contains your private LAN IPs, your public IP, and your DTLS fingerprint, so it is
never stored unencrypted. Both the offer and the answer are wrapped like this:

```
"APP1:" + base64url( header || salt || iv || AES-256-GCM( deflate-raw(SDP) ) || tag )

  header : 4-byte magic "APP1" || 1 flag byte (bit0 = encrypted)
  salt   : 16 random bytes  -> PBKDF2-HMAC-SHA256, 200 000 iterations -> 32-byte key
  iv     : 12 random bytes
  tag    : 16-byte GCM tag (WebCrypto appends it to the ciphertext automatically)
  AAD    : header || salt || iv     (so the flags, salt and iv are authenticated too)
  key    : PBKDF2(code)             (the join code is the passphrase)
```

Compression shrinks a ~2–3 KB SDP to ~1 KB. Use **raw** deflate (no zlib header) so the
browser's built-in `CompressionStream('deflate-raw')` and native libraries (miniz, zlib
with `windowBits = -15`) produce the same bytes.

Browser implementation, which also works in Node 18+, Deno, and Workers:

```js
const PREFIX = 'APP1:', MAGIC = [0x41, 0x50, 0x50, 0x31], FLAG_ENC = 0x01;
const SALT_LEN = 16, IV_LEN = 12, TAG_LEN = 16, PBKDF2_ITER = 200000;

const b64urlEncode = b => { let s = ''; for (const x of b) s += String.fromCharCode(x);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); };
const b64urlDecode = t => { const n = t.replace(/-/g, '+').replace(/_/g, '/');
  const b = atob(n + '='.repeat((4 - n.length % 4) % 4));
  return Uint8Array.from(b, c => c.charCodeAt(0)); };

const pipe = (t, b) => new Response(new Blob([b]).stream().pipeThrough(t))
  .arrayBuffer().then(a => new Uint8Array(a));
const deflateRaw = b => pipe(new CompressionStream('deflate-raw'), b);
const inflateRaw = b => pipe(new DecompressionStream('deflate-raw'), b);
const concat = (...p) => { const o = new Uint8Array(p.reduce((a, x) => a + x.length, 0));
  let k = 0; for (const x of p) { o.set(x, k); k += x.length; } return o; };

async function deriveKey(pass, salt) {
  const m = await crypto.subtle.importKey('raw', new TextEncoder().encode(pass),
                                          'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt, iterations: PBKDF2_ITER, hash: 'SHA-256' },
    m, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

async function sealBlob(sdp, code) {
  const body = await deflateRaw(new TextEncoder().encode(sdp));
  const salt = crypto.getRandomValues(new Uint8Array(SALT_LEN));
  const iv   = crypto.getRandomValues(new Uint8Array(IV_LEN));
  const head = concat(new Uint8Array(MAGIC), new Uint8Array([FLAG_ENC]), salt, iv);
  const key  = await deriveKey(code, salt);
  const ct   = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv, additionalData: head, tagLength: TAG_LEN * 8 }, key, body));
  return PREFIX + b64urlEncode(concat(head, ct));
}

async function openBlob(blob, code) {
  const raw = b64urlDecode(blob.trim().slice(PREFIX.length));
  if (raw.length < 5 || MAGIC.some((m, i) => raw[i] !== m)) throw new Error('not a blob');
  if (!(raw[4] & FLAG_ENC)) return new TextDecoder().decode(await inflateRaw(raw.subarray(5)));
  const headLen = 5 + SALT_LEN + IV_LEN;
  const head = raw.subarray(0, headLen);
  const key  = await deriveKey(code, raw.subarray(5, 5 + SALT_LEN));
  let plain;
  try {
    plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: raw.subarray(5 + SALT_LEN, headLen),
      additionalData: head, tagLength: TAG_LEN * 8 }, key, raw.subarray(headLen));
  } catch { throw new Error('wrong code'); }  // GCM can't tell "wrong key" from "tampered"
  return new TextDecoder().decode(await inflateRaw(new Uint8Array(plain)));
}
```

> If the host is native (C++, Rust, Go, Swift, Kotlin), implement the **byte-identical**
> format with the platform crypto (CNG, OpenSSL, ring, CryptoKit, javax.crypto). Keep one
> test that seals on one side and opens on the other.

---

## 5. The rendezvous server

### Why a Durable Object and not KV

Use **Durable Objects**, not Workers KV. KV is eventually consistent and *caches misses at
the edge*. The host polls for the answer every 2 s. Its early polls miss, the miss is
cached, and when the answer lands it can stay invisible for **up to about a minute**. By
then both peers have timed out, and reconnection never converges. A Durable Object is a
single-threaded actor with strongly consistent storage: a write is visible to the very
next read, and handshakes complete in about a second. The SQLite-backed Durable Object
class is available on the Workers free plan.

One Durable Object instance per room, addressed by `idFromName(roomId)`.

### API

| Method & path | Body / query | Result |
|---|---|---|
| `POST /api/room` | `{id, session, offer}` | `201`. Creates *or overwrites* the room. |
| `GET /api/room/:id` | — | `200 {offer, session}` · `404` unknown/expired |
| `POST /api/room/:id/answer` | `{answer, session}` | `204` · `409` stale session · `404` |
| `GET /api/room/:id/answer?session=` | — | `200 {answer}` (read once, then deleted) · `204` not yet |
| `DELETE /api/room/:id` | — | `204`. Host withdraws the code on shutdown. |

### `src/index.js`

```js
const TTL_SECONDS = 600;          // idle rooms evaporate after 10 min
const MAX_BODY    = 16 * 1024;    // a sealed SDP is ~1 KB
const PREFIX      = 'APP1:';

const CORS = {
  'access-control-allow-origin': '*',
  'access-control-allow-methods': 'GET, POST, DELETE, OPTIONS',
  'access-control-allow-headers': 'content-type',
  'access-control-max-age': '86400',
};
const json  = (o, status = 200) => new Response(JSON.stringify(o), { status,
  headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...CORS } });
const empty = status => new Response(null, { status, headers: { ...CORS, 'cache-control': 'no-store' } });

// Validate shapes so callers cannot address arbitrary objects or store junk.
const isRoomId  = s => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
const isSession = s => typeof s === 'string' && /^[0-9a-f]{16}$/.test(s);
const isBlob    = s => typeof s === 'string' && s.length > 16 && s.length < MAX_BODY &&
                       s.startsWith(PREFIX) && /^[A-Za-z0-9_\-:]+$/.test(s);

async function readJson(req) {
  const raw = await req.text();
  if (raw.length > MAX_BODY) return null;
  try { return JSON.parse(raw); } catch { return null; }
}

export class Room {
  constructor(state) { this.state = state; }

  // Any activity extends the room's life. Only rewrite the alarm once it is past
  // half-life: the host polls every 2 s, and resetting on every poll would be tens
  // of thousands of storage writes a day for a deadline that moves by 2 seconds.
  async touch() {
    const now = Date.now();
    const cur = await this.state.storage.getAlarm();
    if (cur && cur - now > (TTL_SECONDS * 1000) / 2) return;
    await this.state.storage.setAlarm(now + TTL_SECONDS * 1000);
  }
  async alarm() { await this.state.storage.deleteAll(); }

  async fetch(request) {
    const url = new URL(request.url);
    const op  = url.searchParams.get('op');
    const s   = this.state.storage;

    if (op === 'publish') {
      // Overwrite is deliberate: after a drop the host republishes under the SAME
      // code with a NEW session, so the joiner rejoins with no new code.
      const { offer, session } = await request.json();
      await s.put('room', { offer, session, at: Date.now() });
      const old = await s.list({ prefix: 'ans:' });
      if (old.size) await s.delete([...old.keys()]);
      await this.touch();
      return json({ ok: true, expiresIn: TTL_SECONDS }, 201);
    }
    if (op === 'offer') {
      const room = await s.get('room');
      return room ? json({ offer: room.offer, session: room.session })
                  : json({ error: 'unknown or expired code' }, 404);
    }
    if (op === 'answer-post') {
      const { answer, session } = await request.json();
      const room = await s.get('room');
      if (!room) return json({ error: 'unknown or expired code' }, 404);
      // Tell the joiner immediately that it answered an offer that was replaced,
      // instead of letting it burn a full ICE timeout finding out.
      if (room.session !== session) return json({ error: 'stale', session: room.session }, 409);
      await s.put(`ans:${session}`, answer);
      await this.touch();
      return empty(204);
    }
    if (op === 'answer-get') {
      await this.touch();   // a polling host is alive: never expire under it
      const key = `ans:${url.searchParams.get('session')}`;
      const answer = await s.get(key);
      if (!answer) return empty(204);
      await s.delete(key);
      return json({ answer });
    }
    if (op === 'delete') {
      await s.deleteAll();
      await s.deleteAlarm();
      return empty(204);
    }
    return json({ error: 'not found' }, 404);
  }
}

const room = (env, id, op, init = {}) =>
  env.ROOMS.get(env.ROOMS.idFromName(id)).fetch(`https://room/?op=${op}${init.qs || ''}`, init.req);

export default {
  async fetch(request, env) {
    const url = new URL(request.url), path = url.pathname;
    if (request.method === 'OPTIONS') return empty(204);

    // Everything outside /api/ is the joiner page, served from static assets WITH
    // security headers (see run_worker_first in wrangler.jsonc).
    if (!path.startsWith('/api/')) {
      const asset = await env.ASSETS.fetch(request);
      const out = new Response(asset.body, asset);
      out.headers.set('content-security-policy',
        "default-src 'none'; connect-src 'self'; media-src blob:; img-src 'self' data:; " +
        "style-src 'unsafe-inline'; script-src 'unsafe-inline'; " +
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
      out.headers.set('referrer-policy', 'no-referrer');
      out.headers.set('x-content-type-options', 'nosniff');
      return out;
    }

    if (path === '/api/room' && request.method === 'POST') {
      const b = await readJson(request);
      if (!b || !isRoomId(b.id) || !isBlob(b.offer) || !isSession(b.session))
        return json({ error: 'bad request' }, 400);
      return room(env, b.id, 'publish',
        { req: { method: 'POST', body: JSON.stringify({ offer: b.offer, session: b.session }) } });
    }

    const m = path.match(/^\/api\/room\/([0-9a-f]{64})(\/answer)?$/);
    if (m) {
      const [, id, isAnswer] = m;
      if (!isAnswer && request.method === 'GET')    return room(env, id, 'offer');
      if (!isAnswer && request.method === 'DELETE') return room(env, id, 'delete');
      if (isAnswer && request.method === 'POST') {
        const b = await readJson(request);
        if (!b || !isBlob(b.answer) || !isSession(b.session)) return json({ error: 'bad request' }, 400);
        return room(env, id, 'answer-post',
          { req: { method: 'POST', body: JSON.stringify({ answer: b.answer, session: b.session }) } });
      }
      if (isAnswer && request.method === 'GET') {
        const session = url.searchParams.get('session');
        if (!isSession(session)) return json({ error: 'bad request' }, 400);
        return room(env, id, 'answer-get', { qs: `&session=${session}` });
      }
    }
    return json({ error: 'not found' }, 404);
  },
};
```

> `'unsafe-inline'` is there because the reference joiner page is a single self-contained
> HTML file. If you split out your JS/CSS, replace it with `'self'`. Set CSP as a real
> **header**: `frame-ancestors` is ignored when it comes from a `<meta>` tag.

---

## 6. Deploying to Cloudflare

### Layout

```
rendezvous/
├── wrangler.jsonc
├── src/index.js          # the Worker + Durable Object above
└── public/index.html     # the joiner page (§7)
```

### `wrangler.jsonc`

```jsonc
{
  "$schema": "node_modules/wrangler/config-schema.json",
  "name": "my-app-connect",
  "main": "src/index.js",
  "compatibility_date": "2026-09-01",

  // Static assets serve the joiner page. run_worker_first makes EVERY request go
  // through the Worker. Without it, Cloudflare serves matching assets straight from
  // the edge and the security headers set in the Worker are silently dropped.
  "assets": {
    "directory": "./public",
    "binding": "ASSETS",
    "not_found_handling": "single-page-application",   // lets /K7Q-4MX deep links resolve
    "run_worker_first": true
  },

  "durable_objects": {
    "bindings": [{ "name": "ROOMS", "class_name": "Room" }]
  },
  "migrations": [
    { "tag": "v1", "new_sqlite_classes": ["Room"] }     // SQLite-backed: free-plan eligible
  ],

  // Custom domain on a zone already in your Cloudflare account. Wrangler creates the
  // DNS record and TLS certificate. Nothing else on the zone is touched.
  "routes": [
    { "pattern": "connect.example.com", "custom_domain": true }
  ],

  "workers_dev": true,                 // keep *.workers.dev as a fallback URL
  "observability": { "enabled": true }
}
```

### Steps

```bash
cd rendezvous
npx wrangler login            # or set CLOUDFLARE_API_TOKEN for CI / non-interactive
npx wrangler whoami
npx wrangler deploy           # expect bindings: env.ROOMS (Durable Object), env.ASSETS
```

* **No domain?** Delete the `routes` block. Your URL becomes
  `https://<name>.<subdomain>.workers.dev`.
* **Own domain:** the zone must already be on Cloudflare. Pick a subdomain
  (`connect.`, `join.`, `share.`) so the rest of the site is unaffected.
* The first deploy creates the Durable Object class from the `migrations` block. Nothing
  needs creating by hand.
* Build the service URL into the host as a default, and allow overriding it
  (`--service https://…`), so self-hosters and staging work without a rebuild.
* Remove everything later with `npx wrangler delete`.

---

## 7. The joiner web page

This is what the user sees at `https://connect.example.com`: one input box, a Connect button, and
a list of remembered hosts. It is a **single static HTML file** with no dependencies, served by
the Worker from `public/`.

### UX rules that matter

* **Auto-format as they type:** normalize and re-insert the hyphen (`k7q4mx` → `K7Q-4MX`).
* **Deep links:** `https://connect.example.com/K7Q-4MX` pre-fills the code, so the host
  can send a link instead of reading the code out. If the code is already known on this
  device, connect immediately.
* **Remember a code only after it worked** (first media or data received), never on the
  attempt. Otherwise typos fill the list, and because "known" changes how long the page waits
  (below), a typo would wait forever.
* **Let the user name each remembered code** ("Office PC", "Mum's laptop"). The name stays
  in `localStorage` and is never sent anywhere.
* **Known and unknown codes behave differently when there is no room.** For a new code, report
  "Nobody is using that code" after ~15 s, because it is almost always a typo. For a code
  that has worked on this device before, **wait indefinitely** and show "waiting for the
  host". The host is just restarting, and the point of the page is to rejoin without
  anyone touching anything.
* Request a **Wake Lock** while connected, so the screen doesn't sleep mid-session.
* Feature-check `DecompressionStream` and show a clear "browser too old" message if it's missing.

### Saved codes

```js
const STORE = 'app.hosts';
const loadSaved = () => { try { const v = JSON.parse(localStorage.getItem(STORE) || '[]');
  return Array.isArray(v) ? v.filter(e => e && typeof e.code === 'string') : []; } catch { return []; } };
const putSaved  = l => { try { localStorage.setItem(STORE, JSON.stringify(l.slice(0, 12))); } catch {} };
const findSaved = code => loadSaved().find(e => e.code === code);
function rememberHost(code, name) {
  const list = loadSaved().filter(e => e.code !== code);
  list.unshift({ code, name: (name || findSaved(code)?.name || '').trim(), at: Date.now() });
  putSaved(list);
}
```

### The driver loop

**One loop owns the whole lifecycle.** Each attempt owns its `RTCPeerConnection` as a
local variable, and a `generation` counter is checked after every `await`. That way a
superseded attempt can never close the peer connection of the one that replaced it, or
post an answer carrying the wrong SDP. Using free-running timers and a global
`pc` is the classic way to get reconnection that never converges.

```js
const ICE = { iceServers: [{ urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.l.google.com:19302'] }] };
const ATTEMPT_TIMEOUT  = 25000;   // must exceed the host's handshake timeout (15 s)
const GATHER_TIMEOUT   = 8000;
const SESSION_COOLDOWN = 30000;   // don't re-answer a session we already answered and lost
const sleep = ms => new Promise(r => setTimeout(r, ms));
let generation = 0;

function start(code, name) {
  const gen = ++generation;                       // cancels any previous session
  driver(code, name, Boolean(findSaved(code)), gen).catch(e => {
    if (gen === generation) showJoin(e.message);
  });
}
function stop() { generation++; }

async function getOffer(id) {
  const r = await fetch(`/api/room/${id}`, { cache: 'no-store' });
  if (r.status === 404) return null;
  if (!r.ok) throw new Error('server');
  return r.json();                                // {offer, session}
}

async function driver(code, name, known, gen) {
  const id = await roomIdFor(code);
  const spent = new Map();                        // session -> when we lost it
  let everConnected = false, waitingSince = Date.now();

  while (gen === generation) {
    let room;
    try { room = await getOffer(id); } catch { status('waiting for the network'); await sleep(2000); continue; }
    if (gen !== generation) return;

    if (!room) {
      if (!everConnected && !known && Date.now() - waitingSince > 15000)
        return showJoin('Nobody is using that code. Check it and try again.');
      status(known || everConnected ? 'waiting for the host' : 'looking for the host');
      await sleep(1500); continue;
    }

    const lost = spent.get(room.session);
    if (lost && Date.now() - lost < SESSION_COOLDOWN) { await sleep(1000); continue; }

    const outcome = await attempt(id, code, room, gen, () => {
      everConnected = known = true;
      rememberHost(code, name);                   // remember only once it worked
    });
    if (gen !== generation) return;
    if (outcome === 'badcode') return showJoin('That code is not right.');
    if (outcome !== 'stale') { spent.set(room.session, Date.now()); await sleep(500); }
    waitingSince = Date.now();
  }
}
```

### One attempt

```js
async function attempt(id, code, room, gen, onLive) {
  let sdp;
  try { sdp = await openBlob(room.offer, code); } catch { return 'badcode'; }

  const pc = new RTCPeerConnection(ICE);
  const done = deferred();
  let live = false;

  pc.ontrack = ev => { video.srcObject = ev.streams[0] || new MediaStream([ev.track]); live = true; };
  pc.ondatachannel = ev => { /* your app's control / data channel */ };
  pc.onconnectionstatechange = () => {
    const s = pc.connectionState;
    if (s === 'failed' || s === 'closed') done.resolve('failed');
    if (s === 'disconnected') done.resolve(live ? 'dropped' : 'failed');
  };

  try {
    await pc.setRemoteDescription({ type: 'offer', sdp });
    await pc.setLocalDescription(await pc.createAnswer());
    await gatherComplete(pc, GATHER_TIMEOUT);          // no trickle: wait for ALL candidates
    if (gen !== generation) { pc.close(); return 'aborted'; }

    const answer = await sealBlob(pc.localDescription.sdp, code);
    const post = await fetch(`/api/room/${id}/answer`, { method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ answer, session: room.session }) });
    if (post.status === 409) { pc.close(); return 'stale'; }   // host re-offered; not spent
    if (!post.ok)            { pc.close(); return 'failed'; }

    const timer = setTimeout(() => done.resolve('timeout'), ATTEMPT_TIMEOUT);
    // "Connected" means real media/data arrived, not just ICE saying so.
    const watch = setInterval(() => { if (live && video.videoWidth > 0) done.resolve('connected'); }, 250);
    // Notice quickly if the host replaced the offer under us.
    const supersede = setInterval(async () => {
      try { const now = await getOffer(id); if (!now || now.session !== room.session) done.resolve('stale'); } catch {}
    }, 2000);

    const why = await done.promise;
    clearTimeout(timer); clearInterval(watch); clearInterval(supersede);
    if (gen !== generation || why !== 'connected') { pc.close(); return why === 'stale' ? 'stale' : 'failed'; }

    onLive();
    await new Promise(r => { pc.onconnectionstatechange = () =>
      ['failed', 'disconnected', 'closed'].includes(pc.connectionState) && r(); });
    pc.close();
    return 'connected';                                 // driver loops and rejoins
  } catch { try { pc.close(); } catch {} return 'failed'; }
}

function gatherComplete(pc, ms) {
  if (pc.iceGatheringState === 'complete') return Promise.resolve();
  return new Promise(res => {
    const t = setTimeout(done, ms);
    function done() { clearTimeout(t); pc.removeEventListener('icegatheringstatechange', chk); res(); }
    function chk() { if (pc.iceGatheringState === 'complete') done(); }
    pc.addEventListener('icegatheringstatechange', chk);
  });
}
function deferred() { let r, s = false; const p = new Promise(x => r = x);
  return { promise: p, resolve: v => { if (!s) { s = true; r(v); } } }; }
```

For a data-only app (files, chat, control), replace the `video.videoWidth` check with
"the data channel opened and the first message arrived".

---

## 8. The host

The host can be a browser tab, a desktop app, a CLI daemon, a phone app, or an embedded device.
In native code, [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (C/C++)
or [Pion](https://github.com/pion/webrtc) (Go) keep the host small. The flow is the same
everywhere:

```
code   = loadPersistedCode() ?? generateAndPersistCode()
roomId = SHA-256(code)
register "on exit" → DELETE /api/room/:roomId          (withdraw the code)

loop until stopped:                                     (the reconnect loop)
    session = random 16 hex
    pc      = new PeerConnection(STUN [, TURN])
    add tracks / create data channel
    createOffer → setLocalDescription → wait for ICE gathering complete (≤15 s)
    blob    = seal(pc.localDescription.sdp, code)
    POST /api/room {id: roomId, session, offer: blob}   (retry ×3 with backoff)
    poll GET /api/room/:roomId/answer?session=… every 2 s, indefinitely
        (polling also keeps the room alive past its 10-min idle TTL)
    answer  = open(response.answer, code)  →  setRemoteDescription
    wait ≤15 s for "connected"; if not → next iteration (fresh session, SAME code)
    stream …
        meanwhile keep polling the same session's answer slot:
        a NEW, different answer = a joiner is knocking → end session, re-offer
    on drop → back off 2 s, 4 s, 8 s … max 15 s (reset to 2 s after a good session)
```

A browser host in ~40 lines:

```js
async function host(code) {
  const id = await roomIdFor(code);
  addEventListener('pagehide', () => fetch(`/api/room/${id}`, { method: 'DELETE', keepalive: true }));
  let backoff = 2000;
  for (;;) {
    const session = [...crypto.getRandomValues(new Uint8Array(8))]
      .map(b => b.toString(16).padStart(2, '0')).join('');
    const pc = new RTCPeerConnection(ICE);
    const channel = pc.createDataChannel('app');        // and/or pc.addTrack(...)
    await pc.setLocalDescription(await pc.createOffer());
    await gatherComplete(pc, 15000);
    const offer = await sealBlob(pc.localDescription.sdp, code);
    await fetch('/api/room', { method: 'POST', headers: { 'content-type': 'application/json' },
                               body: JSON.stringify({ id, session, offer }) });

    let answer;
    while (!answer) {
      const r = await fetch(`/api/room/${id}/answer?session=${session}`, { cache: 'no-store' });
      if (r.status === 200) answer = (await r.json()).answer;
      else await sleep(2000);
    }
    await pc.setRemoteDescription({ type: 'answer', sdp: await openBlob(answer, code) });

    const ok = await waitFor(() => pc.connectionState === 'connected', 15000);
    if (ok) { backoff = 2000; await waitFor(() => ['failed', 'disconnected', 'closed']
                                                   .includes(pc.connectionState), Infinity); }
    pc.close();
    await sleep(backoff); backoff = Math.min(backoff * 2, 15000);
  }
}

const waitFor = (pred, ms) => new Promise(res => {
  const t0 = Date.now();
  const i = setInterval(() => {
    if (pred()) { clearInterval(i); res(true); }
    else if (Date.now() - t0 > ms) { clearInterval(i); res(false); }
  }, 250);
});
```

(This sketch omits the knock watcher and treats a `404` while polling as "keep waiting".
A production host should republish on `404`, because it means the room expired or was deleted.)

Two host details that save a lot of debugging:

* **Publish with retries.** Publishing is the one call the short-code flow depends on. A
  single timeout should not cost the session its code. Retry about three times with
  1 s / 2 s backoff, then keep retrying in the outer loop *with the same code*.
* **Corporate networks and proxies.** On native Windows, automatic proxy discovery (WPAD)
  can stall the *first* HTTPS request for ~8 s on networks with no WPAD server. Try a
  direct connection (or the user's explicit proxy) first. Fall back to auto-discovery only
  if that fails, and remember which route worked.

---

## 9. Persistence

"The code belongs to the device, not the session."

**Host side**

* Generate the code once and store it: a file in the app's data dir
  (`%LOCALAPPDATA%\<app>\host.code`, `~/.config/<app>/host.code`), `localStorage` for a
  browser host, or Keychain/Keystore on mobile.
* On every start, reuse it. **Validate** it (length 6, alphabet-only) and regenerate if the
  file is truncated or hand-edited.
* Provide a **rotate** action (`--new-code`, "Reset code" button). Rotating is also how you
  **revoke** access from everyone holding the old code.
* Keep a separate *display* copy (e.g. `code.txt`) that is written only once the code is
  actually published, and deleted on stop. Then the UI never shows a code nobody can
  join with yet.
* A "purge/uninstall" action deletes the persisted code so the next start behaves like a
  fresh device.

**Server side**

* Rooms self-expire after **10 minutes of inactivity** via a Durable Object alarm. The
  alarm is the only thing that deletes a room, so a forgotten room can't linger.
* The host's 2 s answer polling counts as activity, so a room stays alive for as long as
  the host is running (an hour, a day), and it disappears within 10 minutes of the host dying
  without cleaning up.
* On clean shutdown the host sends `DELETE`, so joiners stop answering an offer nobody is
  listening to.

**Joiner side**

* `localStorage` list of `{code, name, lastUsed}`, capped at ~12 entries, with Rename and
  Remove buttons. Optionally keep per-host preferences, like a chosen quality, and re-apply them
  after reconnect.

Result: the host restarts or reboots, the same code is republished within seconds, and the
joiner's page, which was waiting because the code is "known", reconnects automatically.

---

## 10. Reconnection

These are the mechanisms that make reconnection work in practice:

| Mechanism | Problem it solves |
|---|---|
| **Same code, new session per attempt** | Answers live under `ans:<session>`, so a reconnect can never consume a previous attempt's answer. |
| **Publish overwrites and clears old answers** | The joiner doesn't need a new code after a drop. |
| **`409 stale` on answer-post** | Joiner answered an offer that was just replaced. It learns instantly and goes back for the new one, instead of waiting out an ICE timeout. The session isn't marked as spent. |
| **Joiner's "spent sessions" cooldown (30 s)** | Never re-answer an offer you already answered and lost. That burns a full timeout talking to a host that has already given up on it. |
| **Joiner watches for supersession (poll every 2 s mid-handshake)** | If the host re-offers, the joiner notices in about 2 s, not 25 s. |
| **Host "knock" detection while streaming** | If the joiner closes the tab or presses Disconnect, the host may not notice until ICE consent expires. It keeps polling the current session's answer slot, and a *new, different* answer means "someone wants in". It ends the session and re-offers. |
| **Ignore a repeat of the answer already used** | A duplicate delivery of the same bytes is not a knock. Treating it as one causes an endless teardown loop. |
| **Minimum session age before a knock is honoured (15 s)** | If two joiners hold the code, they'd otherwise knock each other out as soon as each connects. The delay turns a thrash into slow alternation. (If your app supports multiple joiners, use one room/offer per joiner instead; see §16.) |
| **Exponential backoff, 2 → 15 s, reset after a good session** | A long outage doesn't hammer the server, and a quick blip after a long session doesn't inherit a 15 s wait. |
| **Known codes wait forever** | Joiner rides out host restarts with no user action. |

---

## 11. Timing contract

The two sides must agree on these values, or they race each other:

| Value | Side | Why |
|---|---|---|
| ICE gather ≤ 15 s (host), ≤ 8 s (joiner) | both | Non-trickle: send what you have if gathering stalls. |
| Answer poll every **2 s** | host | Fast joins, and cheap because the DO alarm is rewritten only at half-life. |
| Handshake timeout **15 s** after reading an answer | host | Then republish a fresh session. |
| Attempt timeout **25 s** | joiner | **Must be longer than the host's 15 s**, so the joiner's next poll finds the fresh session instead of racing the dead one. |
| Session cooldown **30 s** | joiner | Longer than the host's rotation window. |
| No-room give-up **15 s** (unknown code only) | joiner | Fast typo feedback. |
| Room idle TTL **600 s**, rewritten at half-life | server | Self-cleaning without per-poll writes. |
| Reconnect backoff **2 → 15 s** | host | See §10. |

---

## 12. Security model

**What the rendezvous stores:** `SHA-256(code)` as the key, plus an AES-256-GCM
ciphertext sealed with a key derived from the code. It never receives the code, and it can't
read the SDP (IP addresses, DTLS fingerprint) in plaintext. Media and data are encrypted
end to end by WebRTC's DTLS-SRTP/SCTP no matter which relay is in the path.

**Limits to be aware of:**

* **A 6-character code is ~30 bits.** That's fine for "type it on a phone", but it is not a strong
  secret against someone who can see the storage. 887 M SHA-256 guesses take seconds on
  a GPU, so the server operator, or anyone with a storage dump, could recover a code from its room
  id and then decrypt that room's blob. PBKDF2 slows *that* step, not the room-id lookup.
  The hashing and encryption protect against **casual exposure** (logs, dashboards,
  bystanders), not against a determined operator.
* **Online guessing:** anyone can `GET /api/room/<sha256(guess)>`. Add a Cloudflare
  **rate-limiting rule** on `/api/room/*` (e.g. 60 req/min/IP). At that rate, finding
  *some* live code among thousands is still slow, and finding a *specific* one is not practical.
* **Anyone with the code can join.** That's by design. Make revocation easy (rotate code) and
  make it visible to the host when someone connects.

**Hardening options, pick what fits your threat model:**

| Option | Cost to UX |
|---|---|
| Longer codes (8 chars ≈ 40 bits, 10 ≈ 50 bits) | Slightly longer to type |
| Deep links carrying a long secret in the URL **fragment** (`/#<code>.<32-char secret>`). The fragment is never sent to the server, so use the secret for PBKDF2 and the short part for routing. | None, when sharing a link |
| Host approval prompt ("Alice wants to connect: Allow?") before accepting an answer | One click on the host |
| Verify the DTLS fingerprint out of band (show a short hash on both screens) | Manual comparison |
| Rate-limit + Turnstile on the joiner page | Invisible/one click |

**Web page hardening:** strict CSP as an HTTP header, `frame-ancestors 'none'`
(clickjacking), `referrer-policy: no-referrer`, `nosniff`, `cache-control: no-store` on
API responses, shape validation on every input, body size cap. Never render server
responses as HTML, and use `textContent`.

---

## 13. NAT, STUN, TURN

* **Same LAN:** host candidates connect. STUN isn't needed, and you can offer a
  "no external contact" mode.
* **One side behind a normal home router:** STUN server-reflexive candidates connect.
* **Both behind symmetric/CGNAT (mobile carriers, some corporate networks):** no direct path.
  **You need TURN.** This is a protocol fact, not a bug. Detect it (ICE `failed` with only host/srflx
  candidates) and tell the user, instead of retrying forever.

Free STUN (no signup): `stun:stun.cloudflare.com:3478`, `stun:stun.l.google.com:19302`.
Use two, in parallel. More than two or three slows gathering without improving results.

TURN options: self-hosted `coturn` on a ~$5/month VPS; **Cloudflare Realtime TURN**
(a free tier; the Worker can mint short-lived credentials. Add a `GET /api/turn` endpoint
that calls the Cloudflare API with a secret and returns `iceServers`); Metered, Twilio,
or Xirsys. TURN is only used when direct paths fail, and it relays ciphertext it can't read.

---

## 14. Testing

Keep a Node script that simulates **both** peers against the deployed Worker
(`node test-rendezvous.js https://connect.example.com`). Assert:

1. Publish → `201`. Fetch offer → same blob back.
2. Answer with the right session → `204`. Host poll → `200` once, then `204` (read-once).
3. Answer with a stale session → `409` with the current session.
4. Republish under the same room with a new session → old answers are gone.
5. `DELETE` → next fetch `404`.
6. Malformed ids, sessions, non-prefixed or oversized blobs → `400`.
7. **Negative:** the stored/returned blob does not contain the code, and it doesn't contain the
   SDP or any IP address in plaintext. Decrypting with a wrong code fails.
8. **Consistency:** poll immediately after answer-post sees the answer (this catches a
   regression to an eventually-consistent store).
9. Cross-implementation: a blob sealed by the host implementation opens in the browser
   implementation, and vice versa.

Then test by hand: two different networks (phone on mobile data), host restart while
joiner waits, joiner closes the tab and reopens, two joiners holding the same code.

---

## 15. Pitfalls checklist

- [ ] Using **KV** for rooms, which causes cached misses and handshakes that never complete. Use a Durable Object.
- [ ] Forgetting **`run_worker_first: true`**, which means the security headers never reach the page.
- [ ] CSP in `<meta>` only, which makes `frame-ancestors` silently ignored.
- [ ] Trickle ICE with a mailbox-style server, which loses candidates. Wait for gathering to complete.
- [ ] Answers stored per *room* instead of per *session*, so a reconnect consumes a stale answer.
- [ ] Joiner attempt timeout ≤ host handshake timeout, so the two sides race forever.
- [ ] Global `pc` plus free-running timers, so an old watchdog kills the new connection.
- [ ] Remembering a code before it worked, so typos are saved and waited on forever.
- [ ] `Math.random()` or modulo-biased code generation.
- [ ] Resetting the Durable Object alarm on every poll, which means thousands of needless writes a day.
- [ ] Host not deleting its room on exit, so joiners answer a dead offer.
- [ ] Treating "ICE connected" as success. Wait for the first frame or message.
- [ ] Browser `CompressionStream('deflate')` (zlib) on one side and raw deflate on the other.
  Use `'deflate-raw'` everywhere.
- [ ] Hard-coding the service URL with no override, which makes staging and self-hosting painful.

---

## 16. Adapting it to your app

| App type | Host | Joiner | Changes |
|---|---|---|---|
| Screen / camera share | Desktop app or browser (`getDisplayMedia`) | Browser `<video>` | Send-only video track. Add a data channel for quality/control. |
| File transfer | Browser tab | Browser tab | Data channel only. Chunk files at 16–64 KB and respect `bufferedAmountLowThreshold`. |
| Remote control / support | Native agent | Browser | Video track + data channel for input events. **Add a host approval prompt.** |
| IoT device dashboard | Device (libdatachannel/Pion) | Browser | Data channel. Print the code on a label or screen. A persistent code is ideal here. |
| 1:1 game / chat | Browser | Browser | Data channel. Either side can be host. |
| One host, many joiners | Any | Browser | Publish one offer per joiner: have the joiner POST a "hello" to get its own sub-room/session, or run a small SFU. A single 1:1 offer can't serve two joiners. |

What stays the same in every case: the code alphabet and generation, SHA-256 room ids, the
sealed blob format, the Worker + Durable Object API, the timing contract, and the
persistence rules. Reuse those parts as they are, and only change what flows over the peer connection.
