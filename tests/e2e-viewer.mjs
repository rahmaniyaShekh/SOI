// End to end, the way a friend would see it: a real share from the shipped
// binary, watched in a real Google Chrome through the real viewer page.
//
//   soi-share start  ->  local handover page  ->  Chrome loads it, answers by
//   itself  ->  ICE / DTLS / SRTP  ->  H.264 decoded into the <video> element
//
// Then the viewer's own controls are exercised over the data channel: a
// quality change must come back as a different picture size.
//
//   npm install playwright-core
//   node tests/e2e-viewer.mjs <path-to-soi-share>
//
// Google Chrome rather than Playwright's bundled Chromium, because only the
// branded build ships the H.264 decoder every real viewer has.
import { spawnSync } from 'node:child_process';
import { chromium } from 'playwright-core';

const bin = process.argv[2];
if (!bin) { console.error('usage: e2e-viewer.mjs <soi-share>'); process.exit(2); }

const PORT = 8123;
let failures = 0;
const ok  = (m) => console.log(`  PASS  ${m}`);
const bad = (m) => { failures++; console.log(`  FAIL  ${m}`); };
const run = (...args) => spawnSync(bin, args, { encoding: 'utf8' });

// Host candidates only: both ends are on this machine, and a hosted runner's
// UDP to the internet is not something this test should depend on.
const started = run('start', '--no-code', '--no-stun', '--port', String(PORT));
process.stdout.write(started.stdout + started.stderr);
if (started.status !== 0) { bad('soi-share start'); process.exit(1); }

let browser;
try {
  browser = await chromium.launch({
    channel: 'chrome',
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required'],
  });
  const page = await browser.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.log(`  [viewer] ${m.text()}`); });
  await page.goto(`http://127.0.0.1:${PORT}/`);

  // Frames decoded and painted, not merely a connected peer.
  await page.waitForFunction(() => {
    const v = document.getElementById('video');
    const q = v && v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
    return v && v.videoWidth > 0 && q && q.totalVideoFrames >= 10;
  }, null, { timeout: 90_000 });
  const first = await page.evaluate(() => {
    const v = document.getElementById('video');
    return { w: v.videoWidth, h: v.videoHeight, frames: v.getVideoPlaybackQuality().totalVideoFrames };
  });
  ok(`Chrome decodes the stream: ${first.w}x${first.h}, ${first.frames} frames`);

  const status = run('status').stdout;
  if (/streaming/.test(status)) ok('soi-share reports streaming'); else bad(`status says: ${status}`);

  // The viewer asks for 360p over the data channel; the sender rebuilds its
  // encoder and the picture that arrives is smaller.
  const hasPicker = await page.evaluate(() => !document.getElementById('qualityWrap').hidden);
  if (hasPicker) {
    await page.selectOption('#quality', '360p');
    await page.waitForFunction(() => document.getElementById('video').videoHeight <= 360,
                               null, { timeout: 30_000 });
    const after = await page.evaluate(() => document.getElementById('video').videoHeight);
    ok(`a quality change from the viewer arrives as a ${after}-line picture`);
  } else {
    bad('the viewer never received the quality list over the data channel');
  }
} catch (err) {
  bad(String(err && err.message || err));
  const log = run('status');
  console.log(log.stdout);
} finally {
  if (browser) await browser.close();
  const stopped = run('stop');
  process.stdout.write(stopped.stdout);
}

console.log(failures ? `\n${failures} FAILED` : '\nend to end: all passed');
process.exit(failures ? 1 : 0);
