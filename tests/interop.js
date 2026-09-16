//
// Cross-checks the signalling blob format between the C++ sender and the browser
// viewer.
//
// This is the single most important interop surface in the project: the viewer
// is the only thing that ever reads what the sender writes, and the two are
// independent implementations (Windows CNG + miniz vs WebCrypto +
// DecompressionStream). A one-byte disagreement about the AAD, the tag position
// or the deflate variant means nothing ever connects.
//
// This file re-implements the format exactly as viewer.html does -- same
// WebCrypto calls, same raw-deflate, same layout -- and round-trips it against
// the real soi-selftest binary in both directions.
//
//   node tests/interop.js [path-to-soi-selftest.exe]
//
const { execFileSync } = require('node:child_process');
const zlib = require('node:zlib');
const { webcrypto } = require('node:crypto');
const path = require('node:path');

const crypto = webcrypto;

const EXE = process.argv[2] ||
  path.join(__dirname, '..', 'build', 'Release', 'soi-selftest.exe');

const PREFIX      = 'SOI1:';
const MAGIC       = [0x53, 0x4F, 0x49, 0x31];
const FLAG_ENC    = 0x01;
const SALT_LEN    = 16, IV_LEN = 12, TAG_LEN = 16;
const PBKDF2_ITER = 200000;

let pass = 0, fail = 0;
const ok  = (m, d = '') => { pass++; console.log(`  \x1b[32mPASS\x1b[0m  ${m}${d ? '  ' + d : ''}`); };
const bad = (m, d = '') => { fail++; console.log(`  \x1b[31mFAIL\x1b[0m  ${m}${d ? '  -- ' + d : ''}`); };

// --- the format, mirroring viewer.html exactly ------------------------------

const b64urlEncode = b =>
  Buffer.from(b).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

const b64urlDecode = t =>
  new Uint8Array(Buffer.from(t.replace(/-/g, '+').replace(/_/g, '/'), 'base64'));

const deflateRaw = b => new Uint8Array(zlib.deflateRawSync(Buffer.from(b), { level: 9 }));
const inflateRaw = b => new Uint8Array(zlib.inflateRawSync(Buffer.from(b)));

function concat(...parts) {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}

async function deriveKey(passphrase, salt) {
  const material = await crypto.subtle.importKey(
    'raw', new TextEncoder().encode(passphrase), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt, iterations: PBKDF2_ITER, hash: 'SHA-256' },
    material, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

async function encodeBlob(text, passphrase) {
  const body = deflateRaw(new TextEncoder().encode(text));
  if (!passphrase)
    return PREFIX + b64urlEncode(concat(new Uint8Array(MAGIC), new Uint8Array([0]), body));

  const salt = crypto.getRandomValues(new Uint8Array(SALT_LEN));
  const iv   = crypto.getRandomValues(new Uint8Array(IV_LEN));
  const head = concat(new Uint8Array(MAGIC), new Uint8Array([FLAG_ENC]), salt, iv);
  const key  = await deriveKey(passphrase, salt);

  const sealed = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv, additionalData: head, tagLength: TAG_LEN * 8 }, key, body));
  return PREFIX + b64urlEncode(concat(head, sealed));
}

async function decodeBlob(blobText, passphrase) {
  let blob = blobText.trim();
  const hash = blob.indexOf('#');
  if (hash >= 0) blob = blob.slice(hash + 1);
  if (!blob.startsWith(PREFIX)) throw new Error('missing SOI1: prefix');

  const raw = b64urlDecode(blob.slice(PREFIX.length));
  if (raw.length < 5 || MAGIC.some((m, i) => raw[i] !== m)) throw new Error('bad header');

  if ((raw[4] & FLAG_ENC) === 0)
    return new TextDecoder().decode(inflateRaw(raw.subarray(5)));

  if (!passphrase) throw new Error('encrypted but no passphrase');

  const headLen = 5 + SALT_LEN + IV_LEN;
  const head = raw.subarray(0, headLen);
  const salt = raw.subarray(5, 5 + SALT_LEN);
  const iv   = raw.subarray(5 + SALT_LEN, headLen);
  const key  = await deriveKey(passphrase, salt);

  const plain = new Uint8Array(await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv, additionalData: head, tagLength: TAG_LEN * 8 },
    key, raw.subarray(headLen)));
  return new TextDecoder().decode(inflateRaw(plain));
}

// --- the real binary --------------------------------------------------------

// No .trim(): the binary writes raw bytes with no trailing newline, and an
// SDP legitimately ends in CRLF -- trimming would mask a real mismatch.
const cppEncode = (text, pass) =>
  execFileSync(EXE, pass ? ['blob-encode', text, pass] : ['blob-encode', text],
               { encoding: 'utf8' });

const cppDecode = (blob, pass) =>
  execFileSync(EXE, pass ? ['blob-decode', blob, pass] : ['blob-decode', blob],
               { encoding: 'utf8' });

// A realistic SDP: exercises compression on the content that actually ships.
const SDP = [
  'v=0', 'o=rtc 2394857 0 IN IP4 127.0.0.1', 's=-', 't=0 0',
  'a=group:BUNDLE video control',
  'a=fingerprint:sha-256 8E:4F:A1:22:9C:03:BE:71:5D:0A:44:E6:19:B2:77:38:A0:5C:14:6D',
  'm=video 51234 UDP/TLS/RTP/SAVPF 96', 'c=IN IP4 192.168.1.42',
  'a=mid:video', 'a=sendonly', 'a=rtcp-mux', 'a=rtpmap:96 H264/90000',
  'a=fmtp:96 profile-level-id=4d0028;packetization-mode=1;level-asymmetry-allowed=1',
  'a=candidate:1 1 UDP 2122317823 192.168.1.42 51234 typ host',
  'a=candidate:2 1 UDP 1686109951 203.0.113.77 51234 typ srflx raddr 192.168.1.42 rport 51234',
].join('\r\n') + '\r\n';

const PASSPHRASE = 'correct-horse-battery-staple';

(async () => {
  console.log('\n\x1b[1mC++ <-> browser signalling blob interop\x1b[0m');
  console.log(`  binary: ${EXE}\n`);

  // 1. unencrypted, C++ -> JS
  try {
    const blob = cppEncode(SDP, '');
    const got  = await decodeBlob(blob, '');
    got === SDP ? ok('C++ encodes -> browser decodes (unencrypted)')
                : bad('C++ encodes -> browser decodes (unencrypted)', 'content differs');
  } catch (e) { bad('C++ encodes -> browser decodes (unencrypted)', e.message); }

  // 2. unencrypted, JS -> C++
  try {
    const blob = await encodeBlob(SDP, '');
    const got  = cppDecode(blob, '');
    got === SDP ? ok('browser encodes -> C++ decodes (unencrypted)')
                       : bad('browser encodes -> C++ decodes (unencrypted)', 'content differs');
  } catch (e) { bad('browser encodes -> C++ decodes (unencrypted)', e.message); }

  // 3. encrypted, C++ -> JS. This is the real proof: PBKDF2 iteration count,
  //    salt/IV placement, AAD span and GCM tag position must all agree.
  try {
    const blob = cppEncode(SDP, PASSPHRASE);
    const got  = await decodeBlob(blob, PASSPHRASE);
    got === SDP ? ok('C++ encrypts -> browser decrypts (AES-256-GCM + PBKDF2)')
                : bad('C++ encrypts -> browser decrypts', 'content differs');
  } catch (e) { bad('C++ encrypts -> browser decrypts', e.message); }

  // 4. encrypted, JS -> C++
  try {
    const blob = await encodeBlob(SDP, PASSPHRASE);
    const got  = cppDecode(blob, PASSPHRASE);
    got === SDP ? ok('browser encrypts -> C++ decrypts (AES-256-GCM + PBKDF2)')
                       : bad('browser encrypts -> C++ decrypts', 'content differs');
  } catch (e) { bad('browser encrypts -> C++ decrypts', e.message); }

  // 5. wrong passphrase must fail on the C++ side
  try {
    const blob = await encodeBlob(SDP, PASSPHRASE);
    cppDecode(blob, 'wrong-passphrase');
    bad('C++ rejects a browser blob under the wrong passphrase', 'it accepted it');
  } catch { ok('C++ rejects a browser blob under the wrong passphrase'); }

  // 6. wrong passphrase must fail on the browser side
  try {
    const blob = cppEncode(SDP, PASSPHRASE);
    await decodeBlob(blob, 'wrong-passphrase');
    bad('browser rejects a C++ blob under the wrong passphrase', 'it accepted it');
  } catch { ok('browser rejects a C++ blob under the wrong passphrase'); }

  // 7. tampering must fail
  try {
    const blob = cppEncode(SDP, PASSPHRASE);
    const i = Math.floor(blob.length / 2);
    const tampered = blob.slice(0, i) + (blob[i] === 'A' ? 'B' : 'A') + blob.slice(i + 1);
    await decodeBlob(tampered, PASSPHRASE);
    bad('browser detects a tampered C++ blob', 'it accepted it');
  } catch { ok('browser detects a tampered C++ blob'); }

  // 8. the URL-fragment hand-off the sender actually uses
  try {
    const blob = cppEncode(SDP, '');
    const got  = await decodeBlob('file:///C:/x/viewer.html#' + blob, '');
    got === SDP ? ok('browser accepts the blob from a viewer.html#... URL fragment')
                : bad('URL fragment hand-off', 'content differs');
  } catch (e) { bad('URL fragment hand-off', e.message); }

  console.log(`\n  \x1b[32m${pass} passed\x1b[0m${fail ? `, \x1b[31m${fail} FAILED\x1b[0m` : ''}\n`);
  process.exit(fail ? 1 : 0);
})();
