//
// End-to-end check of the rendezvous, simulating both peers.
//
// The property that matters most here is NEGATIVE: the server must never be
// able to read the SDP or recover the code. This walks the real deployed Worker
// and asserts both the happy path and that failure modes behave.
//
//   node cloud/test-rendezvous.js [base-url]
//
const zlib = require('node:zlib');
const { webcrypto: crypto } = require('node:crypto');

const BASE = process.argv[2] || 'https://share.mdarif.online';

const PREFIX = 'SOI1:', MAGIC = [0x53, 0x4F, 0x49, 0x31], FLAG_ENC = 0x01;
const SALT_LEN = 16, IV_LEN = 12, TAG_LEN = 16, PBKDF2_ITER = 200000;
const ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';

let pass = 0, fail = 0;
const ok  = (m, d = '') => { pass++; console.log(`  \x1b[32mPASS\x1b[0m  ${m}${d ? '  ' + d : ''}`); };
const bad = (m, d = '') => { fail++; console.log(`  \x1b[31mFAIL\x1b[0m  ${m}${d ? '  -- ' + d : ''}`); };

const b64urlEncode = b => Buffer.from(b).toString('base64')
  .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const b64urlDecode = t => new Uint8Array(
  Buffer.from(t.replace(/-/g, '+').replace(/_/g, '/'), 'base64'));

const deflateRaw = b => new Uint8Array(zlib.deflateRawSync(Buffer.from(b), { level: 9 }));
const inflateRaw = b => new Uint8Array(zlib.inflateRawSync(Buffer.from(b)));

function concat(...parts) {
  const n = parts.reduce((a, p) => a + p.length, 0);
  const o = new Uint8Array(n); let k = 0;
  for (const p of parts) { o.set(p, k); k += p.length; }
  return o;
}

async function deriveKey(passphrase, salt) {
  const m = await crypto.subtle.importKey(
    'raw', new TextEncoder().encode(passphrase), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt, iterations: PBKDF2_ITER, hash: 'SHA-256' },
    m, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

async function roomIdFor(code) {
  const h = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(code));
  return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
}

async function seal(text, code) {
  const body = deflateRaw(new TextEncoder().encode(text));
  const salt = crypto.getRandomValues(new Uint8Array(SALT_LEN));
  const iv   = crypto.getRandomValues(new Uint8Array(IV_LEN));
  const head = concat(new Uint8Array(MAGIC), new Uint8Array([FLAG_ENC]), salt, iv);
  const key  = await deriveKey(code, salt);
  const sealed = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv, additionalData: head, tagLength: TAG_LEN * 8 }, key, body));
  return PREFIX + b64urlEncode(concat(head, sealed));
}

async function open(blob, code) {
  const raw = b64urlDecode(blob.slice(PREFIX.length));
  const headLen = 5 + SALT_LEN + IV_LEN;
  const head = raw.subarray(0, headLen);
  const salt = raw.subarray(5, 5 + SALT_LEN);
  const iv   = raw.subarray(5 + SALT_LEN, headLen);
  const key  = await deriveKey(code, salt);
  const plain = new Uint8Array(await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv, additionalData: head, tagLength: TAG_LEN * 8 },
    key, raw.subarray(headLen)));
  return new TextDecoder().decode(inflateRaw(plain));
}

function makeCode() {
  const r = crypto.getRandomValues(new Uint8Array(6));
  return [...r].map(b => ALPHABET[b % ALPHABET.length]).join('');
}

// This network drops connections intermittently, so retry transport errors --
// otherwise a flaky link looks like a failing server.
const rawFetch = globalThis.fetch;
globalThis.fetch = async (url, init) => {
  let lastErr;
  for (let attempt = 0; attempt < 4; attempt++) {
    try { return await rawFetch(url, { ...init, signal: AbortSignal.timeout(20000) }); }
    catch (e) { lastErr = e; await new Promise(r => setTimeout(r, 800 * (attempt + 1))); }
  }
  throw lastErr;
};

const SDP = 'v=0\r\no=rtc 1 0 IN IP4 127.0.0.1\r\ns=-\r\n' +
  'a=candidate:1 1 UDP 2122317823 192.168.1.42 51234 typ host\r\n' +
  'a=candidate:2 1 UDP 1686109951 203.0.113.77 51234 typ srflx\r\n';

(async () => {
  console.log(`\n\x1b[1mrendezvous end-to-end\x1b[0m\n  ${BASE}\n`);

  const code = makeCode();
  const id   = await roomIdFor(code);
  // One session per connection attempt; answers are keyed by it.
  const makeSession = () =>
    [...crypto.getRandomValues(new Uint8Array(8))]
      .map(b => b.toString(16).padStart(2, '0')).join('');
  let session = makeSession();

  // --- the page itself -----------------------------------------------------
  const page = await fetch(`${BASE}/`);
  const html = await page.text();
  page.ok && html.includes('Join a screen share')
    ? ok('viewer page is served')
    : bad('viewer page is served', `status ${page.status}`);

  // --- sender publishes ----------------------------------------------------
  const offer = await seal(SDP, code);
  const created = await fetch(`${BASE}/api/room`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ id, session, offer }),
  });
  created.status === 201 ? ok('sender creates a room', `code ${code}`)
                         : bad('sender creates a room', `status ${created.status}`);

  // --- the server must not be able to read what it stores -------------------
  const stored = await (await fetch(`${BASE}/api/room/${id}`)).json();
  !stored.offer.includes('192.168.1.42') && !stored.offer.includes('candidate')
    ? ok('stored payload leaks no SDP content (ciphertext only)')
    : bad('stored payload leaks no SDP content', 'plaintext visible in KV');

  !stored.offer.includes(code)
    ? ok('stored payload does not contain the code')
    : bad('stored payload does not contain the code');

  // --- viewer fetches and decrypts -----------------------------------------
  try {
    const got = await open(stored.offer, code);
    got === SDP ? ok('viewer decrypts the offer with the code')
                : bad('viewer decrypts the offer', 'content differs');
  } catch (e) { bad('viewer decrypts the offer', e.message); }

  // --- a wrong code must fail ----------------------------------------------
  try {
    await open(stored.offer, makeCode());
    bad('a wrong code is rejected', 'it decrypted anyway');
  } catch { ok('a wrong code is rejected'); }

  // --- answer round trip ----------------------------------------------------
  const answer = await seal('v=0\r\no=answer\r\n', code);
  const posted = await fetch(`${BASE}/api/room/${id}/answer`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ answer, session }),
  });
  posted.status === 204 ? ok('viewer posts its answer')
                        : bad('viewer posts its answer', `status ${posted.status}`);

  const polled = await fetch(`${BASE}/api/room/${id}/answer?session=${session}`);
  if (polled.status === 200) {
    const { answer: back } = await polled.json();
    back === answer ? ok('sender polls and receives the answer')
                    : bad('sender polls and receives the answer', 'mismatch');
  } else bad('sender polls and receives the answer', `status ${polled.status}`);

  // --- room consumption -----------------------------------------------------
  // The Worker deletes both keys once the sender collects the answer, but KV
  // deletes are only eventually consistent: an edge may keep serving a cached
  // value for up to ~60s. So consumption is best-effort cleanup, NOT a security
  // boundary -- what actually bounds exposure is the 10-minute TTL plus a code
  // space of 31^6 (~887 million) that an attacker would have to guess, and
  // guessing it yields only ciphertext they still cannot decrypt.
  const again = await fetch(`${BASE}/api/room/${id}/answer?session=${session}`);
  [200, 204].includes(again.status)
    ? ok('answer pickup is idempotent (delete is eventually consistent)',
         `status ${again.status}`)
    : bad('answer pickup is idempotent', `status ${again.status}`);

  // The offer deliberately SURVIVES pickup: after a network blip the viewer
  // re-reads it to rejoin on the same code. Deleting it here would mean a
  // dropped connection could never recover without a new code being read out.
  const stillThere = await fetch(`${BASE}/api/room/${id}`);
  stillThere.status === 200
    ? ok('offer survives pickup so a dropped viewer can rejoin')
    : bad('offer survives pickup', `status ${stillThere.status}`);

  // Republishing under the same code must be allowed -- that is how the sender
  // recovers after its own network drops.
  const prevSession = session;
  session = makeSession();
  const republished = await fetch(`${BASE}/api/room`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ id, session, offer: await seal(SDP, code) }),
  });
  republished.status === 201 ? ok('sender can republish under the same code (reconnect)')
                             : bad('sender can republish', `status ${republished.status}`);

  // The reconnect polls a NEW session key, so the previous attempt's answer is
  // invisible to it. This is what makes reconnection safe despite KV deletes
  // being eventually consistent.
  const staleAnswer = await fetch(`${BASE}/api/room/${id}/answer?session=${session}`);
  staleAnswer.status === 204
    ? ok('a reconnect never sees the previous attempt answer')
    : bad('reconnect isolation', `status ${staleAnswer.status}`);

  // And the viewer now reads the new session from the republished offer.
  const reread = await (await fetch(`${BASE}/api/room/${id}`)).json();
  reread.session === session && reread.session !== prevSession
    ? ok('republished offer carries the new session id')
    : bad('republished offer carries the new session id', `got ${reread.session}`);

  // --- reconnect: the properties the retry path actually depends on ---------
  //
  // These exist because reconnection was broken and the reasons were only found
  // by measuring the deployed service. Each assertion below pins one of them.

  // 1. Read-after-write must be immediate.
  //
  // This was the fault that made reconnect impossible. On KV the sender's
  // answer poll missed, the miss was cached at the edge, and an answer written
  // five seconds later stayed invisible for ~50 MORE seconds -- long after both
  // peers had given up and torn their connections down. A signalling channel
  // that reports minute-old state cannot be retried into working, so the room
  // moved to a Durable Object. Guard the property, not the implementation.
  {
    const s = makeSession();
    await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id, session: s, offer: await seal(SDP, code) }),
    });

    // Poll first, exactly as the sender does, so any negative cache is primed.
    for (let i = 0; i < 3; i++)
      await fetch(`${BASE}/api/room/${id}/answer?session=${s}`);

    const t0 = Date.now();
    await fetch(`${BASE}/api/room/${id}/answer`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ answer: await seal('v=0\r\no=a\r\n', code), session: s }),
    });

    let sawItAfter = null;
    for (let i = 0; i < 12 && sawItAfter === null; i++) {
      const r = await fetch(`${BASE}/api/room/${id}/answer?session=${s}`);
      if (r.status === 200) sawItAfter = Date.now() - t0;
      else await new Promise(r2 => setTimeout(r2, 1000));
    }
    // The sender gives up on a handshake after 15s, so anything close to that
    // is already fatal. 5s is a generous ceiling for "immediately".
    sawItAfter !== null && sawItAfter < 5000
      ? ok('an answer is visible to the sender at once', `${sawItAfter} ms`)
      : bad('an answer is visible to the sender at once',
            sawItAfter === null ? 'never arrived within 12s' : `took ${sawItAfter} ms`);
  }

  // 2. An answer to a superseded offer is refused, and says so.
  //
  // The viewer cannot otherwise distinguish "the sender has not picked up my
  // answer yet" from "I am answering a sender that moved on", and it burns a
  // full ICE timeout finding out. A 409 lets it retry immediately.
  {
    const older = makeSession();
    const newer = makeSession();
    await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id, session: older, offer: await seal(SDP, code) }),
    });
    await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id, session: newer, offer: await seal(SDP, code) }),
    });
    const stale = await fetch(`${BASE}/api/room/${id}/answer`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ answer: await seal('v=0\r\no=a\r\n', code), session: older }),
    });
    stale.status === 409
      ? ok('an answer to a replaced offer is rejected with 409')
      : bad('an answer to a replaced offer is rejected', `status ${stale.status}`);

    const fresh = await fetch(`${BASE}/api/room/${id}/answer`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ answer: await seal('v=0\r\no=a\r\n', code), session: newer }),
    });
    fresh.status === 204
      ? ok('an answer to the current offer is still accepted')
      : bad('an answer to the current offer is accepted', `status ${fresh.status}`);
  }

  // 3. A stopped sender withdraws its room.
  //
  // Left behind, a dead offer keeps a viewer's retry loop answering a peer that
  // no longer exists -- once per retry, a full ICE timeout each. Observed in a
  // live run: two wasted attempts over 35s before a real offer appeared.
  {
    const gone = makeCode();
    const goneId = await roomIdFor(gone);
    await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id: goneId, session: makeSession(), offer: await seal(SDP, gone) }),
    });
    const del = await fetch(`${BASE}/api/room/${goneId}`, { method: 'DELETE' });
    const after = await fetch(`${BASE}/api/room/${goneId}`);
    del.status === 204 && after.status === 404
      ? ok('a stopped sender withdraws its offer')
      : bad('a stopped sender withdraws its offer',
            `delete ${del.status}, read back ${after.status}`);
  }

  // --- relay ------------------------------------------------------------------
  //
  // For network pairs with no direct path, host and viewer each hold a
  // WebSocket on the room, paired by session, and the room forwards every
  // message to the other side. What it forwards is sealed with a key it never
  // sees; these check the plumbing.
  {
    const health = await fetch(`${BASE}/api/health`);
    const hj = health.ok ? await health.json() : {};
    hj.ok && hj.service === 'soi' ? ok('health endpoint') : bad('health endpoint', `status ${health.status}`);

    const csp = page.headers.get('content-security-policy') || '';
    // wrangler dev reports the custom domain as the host, so only the shape
    // of the rule is checked here.
    /connect-src 'self' wss:\/\/[a-z0-9.:-]+;/.test(csp)
      ? ok('the page may open the relay WebSocket (CSP)')
      : bad('the page may open the relay WebSocket (CSP)', csp);

    const hex = n => [...crypto.getRandomValues(new Uint8Array(n))]
      .map(b => b.toString(16).padStart(2, '0')).join('');
    const rcode = makeCode(), rid = await roomIdFor(rcode);
    const owner = hex(16), rs = makeSession();
    const pub = await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id: rid, session: rs, offer: await seal(SDP, rcode), owner,
                             caps: ['relay', 'teleport'] }),
    });
    const got = await (await fetch(`${BASE}/api/room/${rid}`)).json();
    pub.status === 201 && JSON.stringify(got.caps) === '["relay"]'
      ? ok('caps round trip, unknown abilities dropped', JSON.stringify(got.caps))
      : bad('caps round trip', `status ${pub.status}, caps ${JSON.stringify(got.caps)}`);

    // An old host: no owner, no caps. Still publishes; advertises nothing.
    const oldCode = makeCode(), oldId = await roomIdFor(oldCode);
    await fetch(`${BASE}/api/room`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id: oldId, session: makeSession(), offer: await seal(SDP, oldCode) }),
    });
    const oldGot = await (await fetch(`${BASE}/api/room/${oldId}`)).json();
    Array.isArray(oldGot.caps) && oldGot.caps.length === 0 && oldGot.offer
      ? ok('a host that predates the relay still publishes, advertising nothing')
      : bad('old host compatibility', JSON.stringify(oldGot));

    const wsBase = BASE.replace(/^http/, 'ws');
    const relayUrl = (role, s, o) =>
      `${wsBase}/api/room/${rid}/relay?session=${s}&role=${role}${o ? `&owner=${o}` : ''}`;
    // Resolves with an open socket, or null if the upgrade is refused.
    const connect = url => new Promise(resolve => {
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      const t = setTimeout(() => { try { ws.close(); } catch {} resolve(null); }, 10000);
      ws.onopen = () => { clearTimeout(t); resolve(ws); };
      ws.onerror = () => { clearTimeout(t); resolve(null); };
    });
    const nextMessage = (ws, ms = 5000) => new Promise(resolve => {
      const t = setTimeout(() => resolve(null), ms);
      ws.onmessage = ev => { clearTimeout(t); resolve(ev.data); };
    });
    const closed = (ws, ms = 5000) => new Promise(resolve => {
      const t = setTimeout(() => resolve(null), ms);
      ws.onclose = ev => { clearTimeout(t); resolve(ev.code); };
    });

    const noOwner = await connect(relayUrl('host', rs));
    const wrongOwner = await connect(relayUrl('host', rs, hex(16)));
    !noOwner && !wrongOwner
      ? ok('the host role is refused without the owner secret')
      : bad('the host role is refused without the owner secret');
    try { noOwner && noOwner.close(); wrongOwner && wrongOwner.close(); } catch {}

    const stale = await connect(relayUrl('viewer', makeSession()));
    !stale ? ok('a viewer for a session the host replaced is refused')
           : bad('a viewer for a stale session is refused');
    try { stale && stale.close(); } catch {}

    const host = await connect(relayUrl('host', rs, owner));
    const viewer = await connect(relayUrl('viewer', rs));
    host && viewer ? ok('host and viewer pair on the session')
                   : bad('host and viewer pair on the session', `host ${!!host}, viewer ${!!viewer}`);

    if (host && viewer) {
      const frame = new Uint8Array(require('node:crypto').randomBytes(200 * 1024));   // keyframe-sized
      const toViewer = nextMessage(viewer);
      host.send(frame);
      const a = await toViewer;
      a && Buffer.compare(Buffer.from(a), Buffer.from(frame)) === 0
        ? ok('host -> viewer forwarded verbatim', `${frame.length} bytes`)
        : bad('host -> viewer forwarded verbatim', a ? `${a.byteLength} bytes` : 'nothing arrived');

      const ctl = crypto.getRandomValues(new Uint8Array(64));
      const toHost = nextMessage(host);
      viewer.send(ctl);
      const b = await toHost;
      b && Buffer.compare(Buffer.from(b), Buffer.from(ctl)) === 0
        ? ok('viewer -> host forwarded verbatim')
        : bad('viewer -> host forwarded verbatim');

      // The keepalive is answered by the room itself and never reaches the peer.
      const pong = nextMessage(host);
      const leaked = nextMessage(viewer, 1500);
      host.send('ping');
      (await pong) === 'pong' && (await leaked) === null
        ? ok('keepalive is answered by the room, not forwarded')
        : bad('keepalive is answered by the room, not forwarded');

      const viewerClosed = closed(viewer);
      host.close(1000);
      const code = await viewerClosed;
      code !== null ? ok('the host leaving closes the viewer', `code ${code}`)
                    : bad('the host leaving closes the viewer', 'viewer stayed open');
    }
  }

  // --- unknown code ---------------------------------------------------------
  const unknown = await fetch(`${BASE}/api/room/${await roomIdFor(makeCode())}`);
  unknown.status === 404 ? ok('unknown code returns 404')
                         : bad('unknown code returns 404', `status ${unknown.status}`);

  // --- input validation -----------------------------------------------------
  const badId = await fetch(`${BASE}/api/room`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ id: 'not-a-hash', offer: 'SOI1:abcdefghijklmnop' }),
  });
  badId.status === 400 ? ok('malformed room id is rejected')
                       : bad('malformed room id is rejected', `status ${badId.status}`);

  const badBlob = await fetch(`${BASE}/api/room`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ id: await roomIdFor(makeCode()), offer: 'not-a-blob' }),
  });
  badBlob.status === 400 ? ok('malformed payload is rejected')
                         : bad('malformed payload is rejected', `status ${badBlob.status}`);

  console.log(`\n  \x1b[32m${pass} passed\x1b[0m${fail ? `, \x1b[31m${fail} FAILED\x1b[0m` : ''}\n`);
  process.exit(fail ? 1 : 0);
})();
