//
// soi-share rendezvous.
//
// Purpose: turn an 800-character SDP into a 6-character code a human can read
// out. A short code cannot carry an SDP, so it has to be a lookup key into
// shared storage -- which is the one thing that genuinely requires a server.
//
// What this server is NOT allowed to learn
// ----------------------------------------
// An SDP contains both peers' private and public IP addresses. A naive
// rendezvous would see all of it. This one cannot, by construction:
//
//   * the code never leaves the two peers. Clients send SHA-256(code) as the
//     room id, so the id in storage is useless for deriving the code.
//   * the payload is AES-256-GCM sealed with PBKDF2(code) before upload, so the
//     room holds ciphertext this Worker has no key for.
//
// So the worst a dump of this storage yields is opaque blobs and hashes. The
// Worker moves bytes; it never learns who is connecting to whom, or from which
// address. Media never touches it at all -- that stays peer-to-peer.
//
// Why a Durable Object and not KV
// -------------------------------
// This was KV, and reconnection did not work because of it. Measured against
// the deployed Worker: the sender publishes an offer and starts polling for the
// answer every 2s. Those early polls miss, and a miss is CACHED at the edge --
// so when the viewer's answer landed 5 seconds later the sender kept reading
// "nothing here" for another FIFTY seconds. Both peers had long since given up
// and torn their connections down; the sender then republished under a new
// session and seeded a fresh 60s miss-cache, forever.
//
// No retry logic can fix a signalling channel that reports stale state for a
// minute. A Durable Object is a single-threaded actor with strongly consistent
// storage: a write is visible to the next read, full stop. Handshakes now
// complete in about a second. The DO sees exactly what KV saw -- a hash and
// ciphertext -- so nothing about the privacy story changes.
//
// API
//   POST   /api/room              {id, session, offer}   -> 201
//   GET    /api/room/:id                                 -> {offer, session}   404 if unknown
//   POST   /api/room/:id/answer   {answer, session}      -> 204
//                                                           409 if that session is stale
//   GET    /api/room/:id/answer?session=                 -> {answer}   204 if not yet
//   DELETE /api/room/:id          {session}              -> 204
//   GET    /api/room/:id/relay?session=&role=host|viewer[&owner=]
//                                                        -> WebSocket (see Relay)
//   GET    /api/health                                   -> {ok, service}
//
// Relay
// -----
// Some pairs of networks can never reach each other directly: a carrier-grade
// NAT that picks a new public port per destination on one side, a router that
// drops packets from unexpected ports on the other. For those, host and viewer
// each open a WebSocket here, paired by the handshake session, and every
// message is forwarded verbatim to the other side. The viewer puts a fresh
// AES-256-GCM key inside its code-sealed answer, so this Worker forwards
// ciphertext it has no key for -- the same promise as the signalling. Hosts
// advertise the ability with caps:["relay"] on publish, and a viewer only
// tries it when it is there. Direct sessions never touch it.
//
const TTL_SECONDS = 600;          // a room with no activity evaporates
const MAX_BODY    = 16 * 1024;    // an SDP blob is ~1 KB
// Relay message caps. A video keyframe is the largest thing a host sends, and
// Cloudflare accepts WebSocket messages up to 1 MiB. A viewer only sends small
// control messages.
const RELAY_MAX_HOST   = 1024 * 1024;
const RELAY_MAX_VIEWER = 64 * 1024;
// Capabilities a host may advertise; anything else is dropped.
const CAPS = ['relay'];
const capsOf = (c) => Array.isArray(c) ? c.filter((x) => CAPS.includes(x)) : [];

const CORS = {
  'access-control-allow-origin': '*',
  'access-control-allow-methods': 'GET, POST, DELETE, OPTIONS',
  'access-control-allow-headers': 'content-type',
  'access-control-max-age': '86400',
};

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store', ...CORS },
  });

const empty = (status) =>
  new Response(null, { status, headers: { ...CORS, 'cache-control': 'no-store' } });

// Room ids are client-supplied SHA-256 hex. Validate the shape so a caller
// cannot address arbitrary Durable Objects.
const isRoomId  = (s) => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
const isSession = (s) => typeof s === 'string' && /^[0-9a-f]{16}$/.test(s);
// A random secret only the publishing host knows. Optional, so hosts that
// predate it keep publishing; only the relay's host role requires it.
const isOwner   = (s) => typeof s === 'string' && /^[0-9a-f]{32,64}$/.test(s);

async function sha256hex(s) {
  const h = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Blobs are the project's own "SOI1:"-prefixed base64url format.
const isBlob = (s) =>
  typeof s === 'string' && s.length > 16 && s.length < MAX_BODY &&
  s.startsWith('SOI1:') && /^[A-Za-z0-9_\-:]+$/.test(s);

async function readJson(request) {
  const raw = await request.text();
  if (raw.length > MAX_BODY) return null;
  try { return JSON.parse(raw); } catch { return null; }
}

// ---------------------------------------------------------------------------
// One room. Single-threaded, strongly consistent, self-expiring.
// ---------------------------------------------------------------------------
export class Room {
  constructor(state) {
    this.state = state;
    // Answered by the runtime itself, so an idle relay's keepalive neither
    // wakes nor bills the object.
    state.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  // Any activity extends the room's life. The alarm is the only thing that
  // deletes a room, so a forgotten one cannot linger.
  //
  // Only rewritten once the existing alarm is past its halfway point. The
  // sender polls for an answer every 2 seconds for the entire life of a share,
  // and resetting the alarm on each of those would be tens of thousands of
  // storage writes a day to express a deadline that moves by 2 seconds.
  async touch() {
    const now = Date.now();
    const current = await this.state.storage.getAlarm();
    if (current && current - now > (TTL_SECONDS * 1000) / 2) return;
    await this.state.storage.setAlarm(now + TTL_SECONDS * 1000);
  }

  async alarm() {
    // A live relay keeps its room: relayed frames deliberately never touch
    // storage, so they do not extend the alarm by themselves.
    if (this.state.getWebSockets().length > 0) {
      await this.state.storage.setAlarm(Date.now() + TTL_SECONDS * 1000);
      return;
    }
    await this.state.storage.deleteAll();
  }

  async fetch(request) {
    const url = new URL(request.url);
    const op = url.searchParams.get('op');

    if (op === 'publish') {
      const { offer, session, owner, caps } = await request.json();
      // Overwrite is deliberate: after a drop the sender republishes under the
      // SAME code with a NEW session, so the viewer rejoins without anyone
      // reading out a new code. Answers are per-session, so the previous
      // attempt's answer is invisible to this one.
      //
      // The owner is kept as a hash and belongs to this publish: whoever
      // published the offer is the only one who may join its relay as the host.
      await this.state.storage.put('room', {
        offer, session, at: Date.now(),
        caps: capsOf(caps),
        owner: isOwner(owner) ? await sha256hex(owner) : null,
      });
      // The old session's answer is dead weight now.
      const old = await this.state.storage.list({ prefix: 'ans:' });
      if (old.size) await this.state.storage.delete([...old.keys()]);
      await this.touch();
      return json({ ok: true, expiresIn: TTL_SECONDS }, 201);
    }

    if (op === 'offer') {
      const room = await this.state.storage.get('room');
      if (!room) return json({ error: 'unknown or expired code' }, 404);
      return json({ offer: room.offer, session: room.session, caps: room.caps || [] });
    }

    if (op === 'relay') {
      if (request.headers.get('upgrade') !== 'websocket')
        return json({ error: 'expected a websocket' }, 426);
      const session = url.searchParams.get('session');
      const role = url.searchParams.get('role');
      if (!isSession(session) || (role !== 'host' && role !== 'viewer'))
        return json({ error: 'bad request' }, 400);
      const room = await this.state.storage.get('room');
      if (!room) return json({ error: 'unknown or expired code' }, 404);
      if (role === 'host') {
        const owner = url.searchParams.get('owner');
        if (!room.owner || !isOwner(owner) || (await sha256hex(owner)) !== room.owner)
          return json({ error: 'forbidden' }, 403);
      } else if (room.session !== session) {
        // The host has moved on to a new offer; this session will never be served.
        return json({ error: 'stale offer', session: room.session }, 409);
      }
      const tag = `${role === 'host' ? 'rh' : 'rv'}:${session}`;
      // One socket per side: a reconnect replaces its own stale socket.
      for (const old of this.state.getWebSockets(tag)) { try { old.close(4000, 'replaced'); } catch {} }
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1], [tag]);
      pair[1].serializeAttachment({ relay: true, role, session });
      await this.touch();
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    if (op === 'answer-post') {
      const { answer, session } = await request.json();
      const room = await this.state.storage.get('room');
      if (!room) return json({ error: 'unknown or expired code' }, 404);

      // Refuse an answer to an offer we have already replaced. Without this the
      // viewer cannot tell "the sender has not read my answer yet" from "I am
      // answering a sender that no longer exists", and it burns a full ICE
      // timeout finding out.
      if (room.session !== session)
        return json({ error: 'stale offer', session: room.session }, 409);

      await this.state.storage.put(`ans:${session}`, answer);
      await this.touch();
      return empty(204);
    }

    if (op === 'answer-get') {
      const session = url.searchParams.get('session');
      // A sender polling for an answer is a live sender, so its room must not
      // be allowed to expire underneath it. Otherwise a share left running for
      // an hour quietly stops being joinable while the daemon sits there
      // believing the code is good.
      await this.touch();
      const answer = await this.state.storage.get(`ans:${session}`);
      if (!answer) return empty(204);      // not yet: keep polling
      await this.state.storage.delete(`ans:${session}`);
      return json({ answer });
    }

    if (op === 'delete') {
      // A sender that is shutting down clears its room, so a viewer in a retry
      // loop stops answering an offer nobody is listening to. Possession of the
      // room id is the credential here, exactly as it is for reading -- and the
      // room id requires the code.
      await this.state.storage.deleteAll();
      await this.state.storage.deleteAlarm();
      return empty(204);
    }

    return json({ error: 'not found' }, 404);
  }

  // --- relay sockets (hibernation API) --------------------------------------
  // Forwarded verbatim: the payload is sealed with a key this Worker never sees.
  async webSocketMessage(ws, msg) {
    const att = ws.deserializeAttachment() || {};
    if (!att.relay) return;
    const size = typeof msg === 'string' ? msg.length : msg.byteLength;
    if (size > (att.role === 'host' ? RELAY_MAX_HOST : RELAY_MAX_VIEWER)) return;
    const peer = `${att.role === 'host' ? 'rv' : 'rh'}:${att.session}`;
    for (const p of this.state.getWebSockets(peer)) { try { p.send(msg); } catch {} }
  }

  // One side left: close the other, so it notices now and reconnects instead
  // of waiting on a pipe nobody is at the other end of.
  async webSocketClose(ws, code) {
    const att = ws.deserializeAttachment() || {};
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, 'closed'); } catch {}
    if (!att.relay) return;
    const peer = `${att.role === 'host' ? 'rv' : 'rh'}:${att.session}`;
    for (const p of this.state.getWebSockets(peer)) { try { p.close(4001, 'peer left'); } catch {} }
  }

  async webSocketError(ws) { await this.webSocketClose(ws, 1011); }
}

// ---------------------------------------------------------------------------

const room = (env, id, op, init) =>
  env.ROOMS.get(env.ROOMS.idFromName(id))
    .fetch(`https://room/?op=${op}${init?.qs || ''}`, init?.req);

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    if (request.method === 'OPTIONS') return empty(204);

    if (path === '/api/health') return json({ ok: true, service: 'soi' });

    // Anything not under /api/ is the viewer page, served from static assets.
    //
    // The policy is sent as a real header rather than only as a <meta> tag,
    // because frame-ancestors is ignored when it arrives via <meta> -- so the
    // clickjacking protection silently did not apply. Everything the page needs
    // is inline, so no external origin is allowed to load anything at all.
    if (!path.startsWith('/api/')) {
      const asset = await env.ASSETS.fetch(request);
      const out = new Response(asset.body, asset);
      // The relay is a WebSocket to this same host. It is named explicitly
      // because older browsers do not let 'self' cover wss:.
      out.headers.set('content-security-policy',
        `default-src 'none'; connect-src 'self' wss://${url.host}; media-src blob:; ` +
        "style-src 'unsafe-inline'; script-src 'unsafe-inline'; " +
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
      out.headers.set('referrer-policy', 'no-referrer');
      out.headers.set('x-content-type-options', 'nosniff');
      return out;
    }

    // --- create or republish a room ----------------------------------------
    if (path === '/api/room' && request.method === 'POST') {
      const body = await readJson(request);
      if (!body || !isRoomId(body.id) || !isBlob(body.offer) || !isSession(body.session))
        return json({ error: 'bad request' }, 400);

      return room(env, body.id, 'publish', {
        req: { method: 'POST', body: JSON.stringify({
          offer: body.offer, session: body.session, owner: body.owner, caps: body.caps,
        }) },
      });
    }

    const match = path.match(/^\/api\/room\/([0-9a-f]{64})(\/answer|\/relay)?$/);
    if (match) {
      const id = match[1];
      const isAnswerPath = match[2] === '/answer';

      // --- relay WebSocket: the upgrade request itself goes to the room -----
      if (match[2] === '/relay') {
        if (request.method !== 'GET') return json({ error: 'not found' }, 404);
        const qs = new URLSearchParams({
          op: 'relay',
          session: url.searchParams.get('session') || '',
          role: url.searchParams.get('role') || '',
          owner: url.searchParams.get('owner') || '',
        });
        return env.ROOMS.get(env.ROOMS.idFromName(id)).fetch(new Request(`https://room/?${qs}`, request));
      }

      // --- viewer fetches the offer ----------------------------------------
      if (!isAnswerPath && request.method === 'GET')
        return room(env, id, 'offer');

      // --- sender clears the room on shutdown -------------------------------
      if (!isAnswerPath && request.method === 'DELETE')
        return room(env, id, 'delete');

      // --- viewer posts its answer ------------------------------------------
      if (isAnswerPath && request.method === 'POST') {
        const body = await readJson(request);
        if (!body || !isBlob(body.answer) || !isSession(body.session))
          return json({ error: 'bad request' }, 400);

        return room(env, id, 'answer-post', {
          req: { method: 'POST', body: JSON.stringify({ answer: body.answer, session: body.session }) },
        });
      }

      // --- sender polls for the answer --------------------------------------
      if (isAnswerPath && request.method === 'GET') {
        const session = url.searchParams.get('session');
        if (!isSession(session)) return json({ error: 'bad request' }, 400);
        return room(env, id, 'answer-get', { qs: `&session=${session}` });
      }
    }

    return json({ error: 'not found' }, 404);
  },
};
