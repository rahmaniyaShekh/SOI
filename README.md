# SOI — Serverless P2P Screen Share (Windows, WebRTC)

A terminal-only Windows screen-sharing sender in C++20. Streams hardware-encoded H.264
over WebRTC **directly to a peer, with no signalling server of your own**, to a
single-file browser viewer that needs no install.

It captures **everything on the screen** — ordinary windows, GPU-composited apps like
Chrome and Electron, exclusive-fullscreen games, and hardware-overlay video — by choosing
automatically between DXGI Desktop Duplication, Windows.Graphics.Capture and GDI, and
switching between them live if one stops delivering. See §1.2a.

No GUI. No service to install. No configuration. Runs detached, controlled from any
terminal.

## Install

On 64-bit Windows 10 (1803+) or 11, you don't need to install anything first: no runtime,
compiler, package manager, admin rights or DLLs. `soi-share.exe` is the whole program.

**One line in PowerShell.** The repository is private, so you need a read-only GitHub
token. [SETUP.md](SETUP.md#private-repository-the-access-token) shows how to make one in
about a minute. The line asks for the token without echoing it:

```powershell
$env:SOI_SHARE_GITHUB_TOKEN = [Net.NetworkCredential]::new('', (Read-Host 'GitHub token' -AsSecureString)).Password; irm https://api.github.com/repos/rahmaniyaShekh/SOI/contents/install.ps1 -Headers @{ Authorization = "Bearer $env:SOI_SHARE_GITHUB_TOKEN"; Accept = 'application/vnd.github.raw' } | iex
```

It downloads the latest release, checks its SHA-256, installs it to
`%LOCALAPPDATA%\Programs\soi-share`, and puts that folder on your PATH. `soi-share` then
works in the same window and in every new one.

**Or double-click.** Download `soi-share.exe` from the
[latest release](https://github.com/rahmaniyaShekh/SOI/releases/latest) and double-click it.
It installs itself the same way and explains what to type next. The exe is unsigned, so if
SmartScreen appears, click **More info → Run anyway**.

## Use

```powershell
soi-share start          # start sharing in the background; prints a 6-character code
soi-share status         # pid, uptime, the code, live stats
soi-share stop           # stop sharing
soi-share update         # newest release, checksum-verified, replaced in place
soi-share uninstall      # remove it (your share code is kept unless --purge)
soi-share help           # everything else
```

Your friend opens **https://share.mdarif.online** and types the code. Any modern browser
works, and they don't need to install anything.

[SETUP.md](SETUP.md) covers the other install options, tokens, updating, uninstalling,
troubleshooting and (for maintainers) building and releasing.

---

# Part 1 — Requirements analysis

Three claims in the original brief needed correcting before any code made sense.

## 1.1 "WebRTC with no server"

**Partly achievable. The precise statement is: you can remove *your* server, not *all*
servers.**

WebRTC needs three distinct things from the network. They get lumped together as "a
server", which is why the question is confusing:

| Need | What it does | Can it be removed? |
|---|---|---|
| **Signalling** | Exchange SDP + ICE candidates before the peers know each other | **Yes** — it is a ~700-character text blob. Any channel works. |
| **STUN** | Tells a peer its own public IP:port so it can advertise a reachable address | **On LAN, yes.** Over the internet, practically no. Free and stateless. |
| **TURN** | Relays media when both peers are behind symmetric NAT | **Yes, if you accept ~10–15% failures.** Optional via `--turn`. |

### Serverless signalling, as implemented

The sender gathers ICE to completion (no trickle — there is no channel to trickle over),
emits one self-contained blob, and embeds it in the viewer's URL fragment. The viewer
answers and copies its answer to the clipboard automatically. Net cost: **one paste back
into a terminal.**

The blob is `deflate`-compressed and optionally **AES-256-GCM encrypted with a
passphrase** (PBKDF2-HMAC-SHA256, 200k iterations, via Windows CNG). This matters: a raw
SDP contains your private LAN addresses, your public IP, and your DTLS fingerprint. Paste
one into a group chat and you have published your network topology.

Rejected alternatives: DHT/BitTorrent bootstrap ("no server you run" is still servers with
extra steps), and any paste/gist relay (that is a server).

### Free STUN and TURN

**STUN is free, plentiful, and used by default.** Two are queried in parallel so one being
down does not stall gathering:

| Server | Signup | Notes |
|---|---|---|
| `stun:stun.cloudflare.com:3478` | none | Anycast, usually lowest latency. **Default.** |
| `stun:stun.l.google.com:19302` | none | The long-standing default everywhere. **Default.** |
| `stun:global.stun.twilio.com:3478` | none | Fine third option. |
| `stun:stun.nextcloud.com:443` | none | Port 443 helps where 3478 is blocked. |

More than two or three is counterproductive: it lengthens gathering without improving the
odds.

**TURN is the hard one, because it relays your actual media.** That costs the operator real
bandwidth, so genuinely free, no-signup, unlimited TURN does not exist. Realistic options,
best first:

| Option | Cost | Honest assessment |
|---|---|---|
| **Self-hosted `coturn`** on a small VPS | ~$4–5/mo | Most reliable, and nobody else is in the path. ~20 minutes to set up. |
| **Cloudflare Realtime TURN** | free tier, account | Generous, anycast. Needs an API call to mint short-lived credentials, so not paste-and-go. Verify current terms. |
| **Metered / Open Relay** | free tier, signup | Easiest hosted start. The anonymous `openrelayproject` credentials in old blog posts are unreliable now — get your own. |
| **Twilio NTS, Xirsys** | paid / small free tier | Fine, but you are buying a relay. |

Hosted quotas change; check current pricing rather than trusting this table.

```powershell
soi-share start --monitor 0 --turn turn:your-host:3478 --turn-user u --turn-pass p
```

Two things worth being clear about:

* **TURN is a fallback, not a downgrade.** ICE still prefers a direct path; the relay is used only when every direct candidate pair fails. Adding it costs nothing when unneeded.
* **A relay cannot see your screen.** DTLS-SRTP terminates at the two peers, so TURN forwards ciphertext it has no key for. It does learn both IP addresses and your traffic volume — a metadata leak, not a content leak.

And the honest caveat: **using TURN means using a server.** Signalling stays serverless
either way. Whether to add a relay is a separate decision about whether you would rather
have a failure rate or a third party in the path.

### The NAT reality to plan for

* Both peers on the same LAN → host candidates connect, **no STUN needed at all**. `--no-stun` gives a genuinely zero-external-contact session.
* One side behind an ordinary home router → STUN srflx candidates connect. Works.
* **Both** behind symmetric NAT → **no connection without TURN.** A protocol-level fact, not an implementation gap. The app reports it instead of hanging.

## 1.2 "BitBlt capture so it's safe from other screen capture"

**These are two unrelated mechanisms.** BitBlt provides exactly zero protection.

### What BitBlt capture is

GDI `BitBlt`/`PrintWindow` from a window or screen DC into a DIB section.

**Genuine advantages:** captures a specific window even when occluded; no OS capture
indicator (WGC drew a yellow border on every captured window before Win11 22H2); works on
Windows 7+ with no WinRT dependency; trivially simple.

**Measured disadvantages** (1920×1200, this machine — see Part 3):

* **Pinned to the display refresh rate: 16.63 ms/frame** at 1920×1200 — one 60 Hz vsync. GDI screen reads synchronise with DWM composition, so ~60fps is a hard ceiling regardless of GPU. This is why `--fps` is capped at 30, which leaves real headroom.
* **Returns black for GPU-composited windows** — Chrome, Edge, Electron, most games. Mitigated with `PrintWindow(PW_RENDERFULLCONTENT)`, which fixes most but not all.
* No damage/dirty-region information, so a static screen would be re-encoded forever unless you detect duplicates yourself (this does — §3.4).

`CAPTUREBLT` (layered windows) turned out to be **free**: 16.58 ms with it vs 16.63 ms
without — within noise, because both are refresh-rate-bound. It stays opt-in
(`--layered`) only because it can make the cursor flicker. This contradicted the common
warning that CAPTUREBLT is slow, which is why it was measured rather than assumed.

That black-frame disadvantage is the one that made the tool unusable for its actual
purpose — "share whatever is on my screen" — because *most* of a modern screen is
GPU-composited. It is why BitBlt is no longer the only backend. See §1.2a.

## 1.2a Capturing everything on the screen, whatever draws it

The brief is "my friend should see exactly what I see." No single Windows capture API
delivers that, because the desktop is assembled from sources that live in different
places:

| What is on screen | Where its pixels are | GDI BitBlt sees it? |
|---|---|---|
| Ordinary windows | DWM's composed surface | yes |
| Chrome / Edge / Electron | a DirectComposition swapchain | **black** (mitigated, not fixed, by `PrintWindow`) |
| Exclusive-fullscreen games | the game's own swapchain, bypassing DWM | **black** |
| Hardware-overlay video (MPO) | merged by the display controller at scanout | **black** |

So SOI has **three capture backends behind the one `FrameSource` interface**, and picks
between them automatically:

| Backend | Reads | Cost/frame | Gets fullscreen & overlays? |
|---|---|---|---|
| **DXGI Desktop Duplication** | the composed scanout image of a whole display | ~0 ms idle, GPU-side copy | **yes** — this is the fix |
| **Windows.Graphics.Capture** | a DWM-composed window or monitor | ~0 ms idle | window path; monitor path where duplication is refused |
| **GDI BitBlt** | a window/screen DC | 16 ms (a full re-read + hash) | no — the floor |

The order is not arbitrary (`capture/CaptureFactory.h`):

* **monitor / desktop:** `dxgi → wgc → bitblt`. Duplication reads the image the display
  is actually being sent, so fullscreen games and overlay video are *in* it. WGC is next
  because it still works where duplication is refused — an RDP session, a VM's basic
  display driver, a hybrid-GPU laptop whose output hangs off the other adapter. GDI is the
  floor that needs no D3D device at all.
* **window:** `wgc → bitblt`. Duplication cannot address a single window. WGC is first
  because it captures DirectComposition windows correctly, which is exactly what GDI turns
  black.

Selection is also **live**: if the running backend stops producing frames — a driver
reset, a duplication the OS revokes on a mode change, a display unplugged — the wrapper
moves to the next one mid-session without the encoder, the peer connection or the share
code noticing. `--capture <dxgi|wgc|bitblt>` pins one and disables the fallback, which is
for diagnosing, not for sharing.

Two commands make the whole thing legible from a terminal:

```
soi-share capture-check     # start every backend against the target, report what works,
                            # time each, and flag any that come back all-black
soi-share gpu-check         # whether frames can stay on the GPU, and if not, which
                            # step said no (see §3.2a)
soi-share status            # among other things, which backend is live right now
```

Measured on this machine (1920×1200), `capture-check` against monitor 0:

```
BACKEND   STARTS   SIZE         MS/GRAB   RESULT
dxgi      yes      1920x1200    0.00      working
wgc       yes      1920x1200    0.00      working
bitblt    yes      1920x1200    18.27     working
```

DXGI is ~2300× cheaper per grab than GDI on a still screen, because the compositor
*tells* it nothing changed instead of it having to re-read and hash the whole frame.

### GPU capture vs pixel copies — which one meets the requirement

The requirement is "share exactly what is on my screen, in any case." That maps directly
onto *where you read the image from*, and there are only two honest places:

* **The final scanout image on the GPU** — the fully-composed frame the display controller
  is about to send to the monitor. Everything that is visible is here, by definition:
  the desktop, GPU-composited apps, an exclusive-fullscreen game, a hardware video overlay.
  **DXGI Desktop Duplication reads exactly this**, GPU-side, and only the finished frame
  crosses the bus to the CPU. This is why it is the primary backend and the one that
  actually satisfies the brief.
* **A per-window pixel copy** (GDI `BitBlt`/`PrintWindow`) — asks one window, or the
  compositor's CPU-visible desktop surface, for its pixels. It misses anything that never
  lands in that surface, which today is most of a screen. It is the floor, not the goal.

So "capture from the GPU" is not an optimisation here — it is *the* mechanism that makes
the tool do what was asked. Reading raw framebuffer memory or hooking another app's
present calls would add nothing DXGI does not already give, and both are exactly the
invasive behaviour that gets a tool flagged. DXGI is the sanctioned API that reads the
same GPU image, so it is both the most complete answer and the cleanest one.

**Which backend is best:** DXGI for anything full-screen, always. WGC exists only for the
cases DXGI physically cannot serve (a single window; a display that refuses duplication —
RDP, some VMs, some hybrid-GPU laptops). GDI exists only as the dependency-free floor.
You never choose; the factory does, and it converges back to DXGI the moment DXGI is
available again (see "recovery" below).

### Overlays, and separate desktops

**Overlays are captured.** Two kinds, both covered:

* A **layered, topmost window** (Discord's in-game overlay, Steam, on-screen widgets) is
  composed by DWM, so DXGI and WGC read it directly. The self-test proves it: a layered
  green probe window is counted in every backend's output.
* A **hardware overlay plane** (MPO — a video player pushing frames straight to the display
  controller) is normally not in any CPU surface. But the moment a Desktop Duplication or
  WGC capture goes active, Windows disables the overlay/independent-flip optimisation and
  composites normally, so the captured frame is complete. This is the same mechanism OBS
  and Teams rely on.

**Separate desktops are a hard Windows boundary, and SOI handles them the only correct
way — by not fighting them.** Windows can host more than one *desktop object*:

* The **secure desktop** — the UAC prompt, `Ctrl+Alt+Del`, the lock and sign-in screens.
  No user-mode capture API of any kind can read it; DXGI returns `E_ACCESSDENIED`, WGC and
  GDI return black. This is a deliberate security guarantee, not a gap. SOI does not crash
  or die when it happens: the duplication is dropped and cleanly re-acquired the instant
  the secure desktop is dismissed, so the stream simply resumes. (This is *why* the
  recovery logic below re-promotes to DXGI rather than ratcheting down to GDI.)
* A **separate interactive desktop** created by another program (some sandboxes do this):
  a process on desktop A cannot read desktop B, by design. If something switches the user
  to its own desktop, SOI keeps capturing the desktop it lives on. There is no user-mode
  way around this and nothing SOI should do about it.

### Recovery: converging back to the best backend

A capture backend can stop delivering for reasons that have nothing to do with it being
the wrong choice — a driver reset, a display mode change, a fullscreen transition, a UAC
prompt that outlasts the grace period. The wrapper (`capture/CaptureFactory.cpp`) handles
this without ratcheting downward:

* A momentary gap is absorbed by a grace period; the backend usually recovers itself.
* A longer failure puts *that* backend on a short cooldown and re-selects from the **top**
  of the preference order — so once the cause clears, capture climbs straight back to DXGI
  instead of being stranded on GDI. The cooldown stops a genuinely-broken backend from
  being hammered every frame.
* A total blackout (every backend dark — i.e. a secure desktop) parks with no active
  backend and keeps retrying a few times a second until the screen comes back.

The net effect is what the requirement demands: whatever is on the screen reaches the
viewer, and the *best* reader for it is used whenever it is available, in any case.

### What actually makes a window safe from other screen capture

`SetWindowDisplayAffinity(hwnd, affinity)`:

| Affinity | Value | Effect |
|---|---|---|
| `WDA_NONE` | `0x00` | Normal. |
| `WDA_MONITOR` | `0x01` | Window renders **black** in any capture. Win7+. |
| `WDA_EXCLUDEFROMCAPTURE` | `0x11` | Window is **absent** from capture but fully visible on the physical monitor. **Win10 2004+.** |

DWM enforces this at composition time, so it applies uniformly to BitBlt, WGC, DXGI
Duplication and print-screen. **No user-mode capture method bypasses it** — the choice of
BitBlt over WGC is irrelevant to protection.

**This is verified, not assumed.** The test suite puts a magenta window on the real screen,
captures the desktop exactly as any other tool would, and counts pixels:

```
PASS  the unprotected probe window IS captured
PASS  WDA_EXCLUDEFROMCAPTURE: the window is COMPLETELY absent from the capture
PASS  WDA_MONITOR also keeps the colour out of the capture
PASS  the window reappears once cleared (so the capture was genuinely live)
PASS  the watchdog re-protects a window created after startup
        probe pixels: 117600 unprotected -> 0 excluded
```

### Verified against every capture mode, not just ours

"Safe from screen capture" is only meaningful if it holds for the APIs other
recorders use. The same probe window is captured through three independent paths,
before and after protection:

```
        DXGI Desktop Duplication available: yes
PASS  unprotected: visible to GDI BitBlt
PASS  unprotected: visible to GDI PrintWindow
PASS  unprotected: visible to DXGI Desktop Duplication
PASS  PROTECTED: absent from GDI BitBlt
PASS  PROTECTED: absent from GDI PrintWindow
PASS  PROTECTED: absent from DXGI Desktop Duplication
        BitBlt 117600 -> 0 | PrintWindow 117600 -> 0 | DXGI 117600 -> 0
```

DXGI Desktop Duplication is the path OBS, Discord, Teams and TeamViewer actually
use, so that third line is the one that matters most.

**Windows.Graphics.Capture is the fourth modern path, and it is now measured
too.** SOI ships a real WGC backend (see §1.2a), so the self-test points it at
the monitor and counts probe pixels exactly as it does for the other three:

```
PASS  unprotected: visible to Windows.Graphics.Capture
PASS  PROTECTED: absent from Windows.Graphics.Capture
        WGC 117600 -> 0
```

All four user-mode capture paths on Windows now agree: a protected window is
completely absent. This was previously inference from the shared DWM step; it is
no longer.

A `ProtectionWatchdog` re-applies affinity every second, because a single sweep at startup
would miss any window created later.

### Honest limits of that protection

It is a compositor-level guarantee, not a security boundary. It does **not** stop
kernel-mode or driver-level capture, an injected DLL reading process memory, a mirror
driver, a hardware HDMI capture card, or a phone pointed at the monitor.

**And one specific gap you should know about.** The app is terminal-only and owns no
windows, so there is normally nothing of ours on screen. But `SetWindowDisplayAffinity`
only works on windows owned by the calling process, and under Windows Terminal the visible
window belongs to `WindowsTerminal.exe`. **The terminal cannot be protected, so anything
printed in it — including the offer blob — is visible to screen capture.**

The mitigation is the detached design: `soi-share start` returns immediately and the
streaming process has no console at all. Start it, let it go into the background, and close
or ignore the terminal. Or share a single window, or a monitor that is not the one with
your terminal on it.

## 1.3 Language choice

Scored against what this workload demands: Win32/COM for BitBlt, a hardware H.264 encoder,
per-frame latency control, and a WebRTC stack.

| | C++20 | Rust | C# / .NET 8 | Python | Go | Electron/JS |
|---|---|---|---|---|---|---|
| Win32 + GDI BitBlt | **Native** | via `windows` crate, verbose | P/Invoke, workable | pywin32, slow marshalling | painful | none |
| Media Foundation HW encoder | **Native COM** | awkward COM | thin interop | **Not available** | **Not available** | **Not available** |
| WebRTC library maturity | libdatachannel, libwebrtc | webrtc-rs (no HW codec) | SIPSorcery (software only) | aiortc (software only) | pion (software only) | built-in, but no BitBlt |
| Per-frame latency / GC pauses | **None** | None | GC pauses at p99 | GIL + GC, bad | GC, tolerable | GC, bad |
| Development speed | Slow | Slow | Fast | **Fastest** | Fast | Fast |

**Verdict: C++20.** The only option where BitBlt, Media Foundation hardware encoding, and a
low-level WebRTC stack are all first-class simultaneously. Rust is a genuine second choice
and would win for a greenfield cross-platform effort, but every Media Foundation call
becomes `unsafe` COM boilerplate — you pay Rust's cost without collecting its benefit on
the hardest part of the program.

**Python is disqualified**, not merely slower: `aiortc` has no hardware encoder binding, so
1080p H.264 runs on the CPU through PyAV and a single stream saturates a core. Fine for a
proof of concept, nothing more.

**The viewer is a browser page** — the one place JavaScript wins outright. Hardware H.264
decode, jitter buffering and rendering for free, on any OS, with no install.

### Library: libdatachannel, not libwebrtc

`libwebrtc` is a ~10 GB checkout needing `depot_tools` and about an hour to build, and it
drags in its own capture/encode/render stack we would have to fight. `libdatachannel`
builds in seconds via `FetchContent` and hands us raw RTP — exactly the right seam, because
we want to own capture and encoding.

The accepted trade-off: libdatachannel has **no bandwidth estimator** (Google's GCC lives
in libwebrtc), so §3.6 implements a loss-based AIMD controller from RTCP. Weaker than GCC,
but honest and ~150 lines.

---

# Part 2 — Architecture

```
soi-share start ──spawns──► start --foreground --log-file ...  (DETACHED: no console)
   │                              │
   │  127.0.0.1:<ephemeral> ◄────►│  status / stop / answer, secret-checked
   │                              │  %LOCALAPPDATA%\soi-share\
   │                              │    instance.txt  (pid, port, secret)
   │                              │    soi-share.log machine.code
   ▼
 terminal (any terminal: status / answer / stop)

┌──────────────────── DAEMON ────────────────────────────────────────────┐
│  BitBltCapture ──► ColorConvert ──► H264Encoder ──► Streamer           │
│  GDI BitBlt 1:1    BGRA→NV12 +      Media Foundation  libdatachannel   │
│  + cursor          box downscale    async HW MFT      RTP/SRTP/DTLS    │
│  + dup-detect      SSE2 + MT        CBR low-latency   NACK + SR + AIMD │
│                    BT.709 limited   IDR on demand                       │
│                                                                         │
│  ProtectionWatchdog: SetWindowDisplayAffinity every 1s                  │
└──────────────────────────────┬──────────────────────────────────────────┘
                               │  SignalBlob: deflate + AES-256-GCM (CNG)
                               │              + base64url
              offer (URL fragment / paste) ──►
              ◄── answer (one paste back)
                               │
┌──────────────────────────────▼──────────────────────────────────────────┐
│  VIEWER — viewer/viewer.html, single file, opened from disk (file://)   │
│  WebCrypto PBKDF2+AES-GCM · DecompressionStream('deflate-raw') · <video>│
│  Screen picker (any monitor / all screens) · quality · keyframe request │
│  Live stats: bitrate, fps, resolution, loss, jitter, RTT, decode, path  │
└─────────────────────────────────────────────────────────────────────────┘

Media is strictly peer-to-peer after connect. No third party sees pixels.
```

## File map

| File | Role |
|---|---|
| `src/main.cpp` | CLI subcommands, session orchestration |
| `src/app/Service.{h,cpp}` | Detached spawn, pid file, state files, stop event, single instance |
| `src/capture/BitBltCapture.{h,cpp}` | GDI capture: desktop / monitor / window, cursor, duplicate detection |
| `src/capture/CaptureProtect.{h,cpp}` | `SetWindowDisplayAffinity` + watchdog + console-window reality check |
| `src/encode/ColorConvert.{h,cpp}` | BGRA→NV12, BT.709 limited, SSE2 `madd`, box downscale, multithreaded |
| `src/encode/H264Encoder.{h,cpp}` | Media Foundation **async** hardware MFT driver, system-memory and DXGI-surface input |
| `src/gpu/GpuPipeline.{h,cpp}` | Shared D3D11 device + the BGRA→NV12 conversion shader (§3.2a) |
| `src/net/Streamer.{h,cpp}` | libdatachannel peer, H264 RTP, RTCP parser, AIMD |
| `src/net/SignalBlob.{h,cpp}` | deflate + AES-256-GCM (CNG) + base64url |
| `src/util/*` | logging (console + file), COM/GDI RAII, thread pool, `std::format` shim |
| `tests/selftest.cpp` | 60+ assertions incl. the pixel-level protection proof |
| `viewer/viewer.html` | Zero-install browser receiver |

---

# Part 3 — Implementation notes

## 3.1 Capture

Three modes: `--desktop` (full virtual desktop), `--monitor N`, `--window <hwnd|substring>`
(tries `PrintWindow(PW_RENDERFULLCONTENT)` first so GPU-composited windows are not black,
falling back to `BitBlt`).

The DIB section is created once with a negative `biHeight` for a top-down BGRA buffer,
which is what the converter wants and avoids a per-frame vertical flip.

The cursor is not included by GDI, so it is composited manually (`GetCursorInfo` →
`GetIconInfo` → `DrawIconEx`). Off by default, because a moving cursor defeats
duplicate-frame detection on an otherwise static screen.

**Capture is 1:1 only.** Downscaling moved into the conversion pass — see §3.2 for why.

## 3.1a Choosing what to watch, from the viewer's side

`--monitor N` only decides where the session *starts*. Once connected, the viewer is
told what screens the sharing PC has and picks between them from their browser — no
one has to touch the sharing machine, and nothing about the session is renegotiated:

```
Screen  [ All 3 screens · 5120×1440 ▾ ]
        [ Screen 1 · 1920×1200 (main) ]
        [ Screen 2 · 2560×1440        ]
        [ Screen 3 · 1920×1080        ]
```

**"All screens" is the virtual desktop** — the bounding box of every monitor, captured as
one wide picture. DXGI duplicates each output and composes them; on a machine where
duplication is refused, WGC cannot address more than one display and the factory falls
through to GDI, which reads the whole virtual screen in one blit.

The sender enumerates monitors **fresh on every announcement**, so a screen unplugged
mid-session disappears from the menu rather than becoming a dead entry. The announcement
is also the single source of truth: the viewer never assumes a switch worked, so a request
the sender refuses simply snaps the menu back to what is really on screen.

A switch costs a capture retarget and an encoder rebuild — resolution is baked into the
encoder's media type — so it runs on the capture thread, the same path a quality change
takes, and is rate-limited to one per 750 ms. Requests arriving faster collapse into the
latest one rather than queueing: two quick clicks land on the second screen, and a viewer
holding the menu open cannot make the sender rebuild its encoder in a loop.

One consequence worth stating, because getting it wrong produces a frozen picture rather
than an error: `profile-level-id` is negotiated **once**, at handshake time, but the
viewer can switch afterwards to a screen — or to all of them — larger than the one the
session started on. So the SDP advertises the largest size this session could *ever*
encode, the virtual desktop included, not the size it opens with.

### Windows virtual desktops (Task View) are not capture targets

`Win+Ctrl+D` desktops are not separate things to capture: they are the same monitors
showing different windows. Capturing monitor 0 shows whichever virtual desktop is
currently active, and follows the user when they switch. "Virtual desktop" in this
document always means the multi-monitor bounding box.

A *separate desktop object* — the secure desktop, or one created by a sandbox — is a
different matter entirely, and is a hard Windows boundary. See §1.2a.

### What the viewer is allowed to change

Letting the far end choose the screen means the far end can now influence **what is
shared**, not merely how it looks, so the scope is fixed before the peer exists:

| Started with | Viewer may switch? | What the viewer is told |
|---|---|---|
| `--monitor N` or `--desktop` | any screen, or all of them | the full screen list |
| `--monitor N --lock-target` | no | nothing beyond the current size |
| `--window <spec>` | **no, unconditionally** | nothing beyond the current size |

A window share is locked whether or not `--lock-target` is given. Sharing one window is an
explicit narrowing — the operator picked that window and nothing else — and widening it to
a whole screen on the strength of a message from the far end would hand over more than was
agreed to.

**The screen list is withheld, not merely ignored.** A locked sender sends an empty list,
so a viewer who was given one window never learns how many monitors this PC has or how big
they are. Refusing the request on arrival while still publishing the inventory would leak
exactly the thing the lock exists to protect.

Three checks, each somewhere the far end cannot reach:

* `Streamer.cpp` bounds the index so a nonsense value never reaches the enumeration code. It decides nothing else.
* Permission is decided **once**, at `QualityDirector` construction, from the operator's own command line, and held in a `const` member — no later code path can talk itself into flipping it.
* The request is re-checked on the capture thread against a monitor list read a moment ago, so an index that stopped existing is ignored rather than acted on.

A refused, stale or impossible request re-announces the true state and keeps streaming. A
target that fails to open restores the previous one — the viewer asked for a different
screen, not for the share to end.

None of this touches capture protection: `SetWindowDisplayAffinity` is enforced by DWM at
composition time and the watchdog runs for the life of the process, independent of what is
being captured. A protected window stays absent from every screen in the picker, including
"all screens". The test suite proves this against all four capture paths (§1.2).

## 3.2 Colour conversion and scaling

* **BT.709, limited range (16–235)** — the correct matrix for HD. Using BT.601 here is the single most common cause of "the colours look slightly washed out" in homegrown screen sharers.
* Y plane via SSE2 `_mm_madd_epi16` + SSSE3 `_mm_hadd_epi32`, 8 pixels per iteration, with a scalar fallback and a scalar tail.
* UV subsampled 2×2 with proper box averaging before conversion, not point sampling.
* Row bands split across a persistent thread pool; no per-frame thread creation.

**Downscaling is folded in here, and that was a measured decision.** The original design
used GDI `StretchBlt` with `HALFTONE`. Benchmarking showed HALFTONE costs ~16.7 ms of pure
CPU — as much as the capture itself — which pushed the pipeline over a vsync boundary to 50
ms/frame (20fps). The conversion pass already reads every source pixel, so box filtering
there is nearly free, and it beats HALFTONE's approximation on text.

Each output pixel box-averages its source footprint; per-pixel channel sums are computed
once for both rows of an output row pair, then Y is derived per row and UV by combining the
2×2 block — so every source pixel is read exactly once.

## 3.2a The GPU pipeline — never bringing the frame back

Everything in §3.2 describes work done on the CPU, on pixels that were dragged off
the graphics card to get there. For a screen share that round trip is pure loss: the
frame is *born* on the GPU (Desktop Duplication reads the scanout image) and *dies* on
the GPU (the hardware encoder is on the same chip). At 1920×1200 the CPU path costs, per
frame:

| Step | Cost |
|---|---|
| `CopyResource` to a staging texture | GPU-side |
| `Map` + read back | **~9.2 MB across the bus** |
| BGRA→NV12 + downscale, SSE2 × 8 threads | ~0.5–2 ms of CPU |
| hand ~3.5 MB of system memory to the MFT | **uploaded to the GPU again** |

`--gpu` removes all four. Each output's duplication texture is blitted into one
BGRA texture with `CopySubresourceRegion`, a pixel shader converts that to NV12,
and the NV12 texture goes to Media Foundation as a DXGI surface. Nothing is ever
mapped; the only thing crossing the bus is the compressed bitstream.

```
soi-share gpu-check

  target      yes -- a screen, which Desktop Duplication can read
  capture     yes -- Intel(R) UHD Graphics
  encoder     yes -- Intel® Quick Sync Video H.264 Encoder MFT (hardware)
  shader      yes -- BGRA to NV12 at 1920x1200

Result: the GPU pipeline.
```

### How the shader writes NV12

NV12 is planar, and a pixel shader writes to render targets, so the trick is to
put **two render target views on one NV12 texture**: `R8_UNORM` addresses the Y
plane at full size, `R8G8_UNORM` addresses the interleaved UV plane at half. Two
draws of a single fullscreen triangle — built from `SV_VertexID`, so there is no
vertex buffer, no index buffer and no input layout — and the texture is complete.

The colour maths is the same BT.709 limited-range matrix as §3.2, and **the
downscale is the same box filter**, not a bilinear tap. That matters for the same
reason it did on the CPU: one tap reads two of the nine pixels a 3× downscale
covers, and the seven it skips are the ones that made the text legible.

### What it gives up, and why there are three modes

The frame is no longer visible to the CPU. Two things depended on that:

* **Cursor compositing is GDI**, drawing onto CPU pixels that no longer exist. `--cursor` therefore selects the CPU pipeline, and says so.
* **Duplicate detection was a sampled hash.** No loss here — duplication already reports content change exactly and for free (§3.4), which is both cheaper and more accurate than hashing.

It also needs one D3D11 device shared by capture and encoder, so **every output
must be on one adapter** (spanning two would need a cross-adapter copy costing
about what the readback costs) and **the encoder MFT must be D3D11-aware** — the
Microsoft software MFT is not.

None of that is reliably knowable in advance, which is why the mode is explicit:

| Flag | Behaviour |
|---|---|
| `--pipeline auto` *(default)* | GPU where every precondition holds, CPU otherwise, silently |
| `--gpu` | GPU or **refuse to start**. For when "it fell back and I did not notice" is the bug |
| `--cpu` | always SSE2. Required by `--cursor`, and the way to prove a GPU-path bug is a GPU-path bug |

`soi-share gpu-check` reports which one you get by *doing* each step — starting
the capture, sharing the device, asking the encoder whether it will take it —
rather than inferring it from a driver version.

### It is verified numerically, not by eye

A shader that writes Y into the UV view, or uses BT.601, or gets the limited-range
scaling wrong, still encodes to a plausible number of bytes. It just looks wrong,
and nothing but a human would notice. So the suite reads the NV12 back (test only
— the real path never does) and checks it against the same reference values the
SSE2 path is held to:

```
PASS  the shader converts a BGRA texture to NV12
PASS  luma matches the BT.709 limited-range reference
        black 16(want 16) white 235(want 235) grey 126(want 126) red 63(want 63)
PASS  grey is exactly neutral chroma, so the UV plane is addressed correctly
PASS  red lands on the right chroma axis (U and V are not swapped)
```

and then drives the whole thing for real — duplicate the actual screen, convert
with the actual shader, feed the actual hardware encoder:

```
PASS  the DXGI backend keeps frames on the GPU when asked
PASS  the frame is a texture, with no CPU copy taken
PASS  the encoder really took the D3D11 device (zero-copy input)
PASS  the GPU pipeline produces encoded frames
PASS  the bitstream carries real content, not a flat frame
```

### Where the idea came from

[RustFrame](https://github.com/salihcantekin/RustFrame) mirrors a screen region by
taking the WGC texture, cropping it in an HLSL shader and presenting it to a
DirectX swapchain — the frame never touches the CPU. The principle ports directly;
only the destination differs. RustFrame's is a window, so its shader presents.
Ours is an encoder, so the shader writes NV12 and the texture goes to Media
Foundation instead.

## 3.3 Encoding

`MFTEnumEx` finds the vendor encoder (NVENC / Quick Sync / AMF), falling back to the
Microsoft software MFT when there is no GPU encoder — so it works on any laptop.

Hardware MFTs are **asynchronous**, which is the part most implementations get wrong. They
must be unlocked with `MF_TRANSFORM_ASYNC_UNLOCK` and driven by an event loop
(`METransformNeedInput` / `METransformHaveOutput`), not the synchronous call-and-check
pattern. A dedicated thread does a blocking `GetEvent`. Both modes are implemented.

Configured for interactive latency: `CODECAPI_AVLowLatencyMode` (no lookahead, no
B-frames), CBR, long GOP (IDRs are expensive and PLI gets us one on demand),
`CODECAPI_AVEncVideoForceKeyFrame` on PLI/FIR. Output is Annex-B; SPS/PPS is cached from
`MF_MT_MPEG_SEQUENCE_HEADER` and re-prepended on keyframes if the MFT omits it, so a viewer
joining mid-stream can always decode.

The submit queue is capped at 3 frames and **drops the oldest** when full. A stale screen
frame has no value, and buffering it would add latency the viewer can never recover. The
test suite asserts this rather than tripping over it.

## 3.4 Duplicate frame suppression

Every frame is sampled-hashed (FNV-1a over a strided subset); identical consecutive frames
are dropped without encoding or sending. A frame is forced at least every `--idle-refresh`
seconds so a late joiner is never stuck on a blank video element. On an idle desktop this
drops steady-state bandwidth from megabits to a few kbps.

## 3.5 Transport

H.264 payload type 96, **packetization-mode=1**, MTU-safe 1200-byte fragments.
`RtcpSrReporter` for sender reports; `RtcpNackResponder` with 512-packet history recovers
single-packet loss without a full IDR — a large subjective win on lossy Wi-Fi.

`profile-level-id` is **computed from the actual resolution and frame rate**, not
hardcoded. 1080p30 needs Main level 4.0 (`4d0028`); the commonly copy-pasted `42e01f`
(3.1) understates it, and strict decoders can reject an understated level.

## 3.6 Congestion control

RTCP is parsed by hand for PLI/FIR (→ force IDR, rate-limited to one per 500 ms), REMB (→
authoritative target), and Receiver Reports (→ fraction lost, RTT from LSR/DLSR).

AIMD once per second: loss > 10% → `target *= 0.85`; loss < 2% → `target *= 1.05`; clamped
to `[--min-bitrate, --bitrate]`. REMB overrides AIMD when present, since Chrome's estimate
is better than ours.

## 3.7 Detached operation

`start` relaunches the executable as `start --foreground --log-file <path>` under
`DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB`. The first flag gives it no console at all,
not merely a hidden one. The second is needed because some terminals put children in a job
object that kills the whole tree on exit; it retries without the flag if the job forbids
breakaway. Because the child is launched with `--foreground`, it can never detach again.
Its working directory is the state folder, so it never pins the folder the user typed
`start` in. `start --foreground` is the same process attached to the terminal, with a live
log.

The running instance keeps everything `status` shows (state, code, LAN URL, stream stats,
pending offer) **in memory**. It serves that over a control channel on `127.0.0.1`, on an
ephemeral port, bound with `SO_EXCLUSIVEADDRUSE`. `%LOCALAPPDATA%\soi-share\instance.txt`
records the port, pid, image path and a random per-run secret. The file is in the user's own
profile, so another account on the PC can't read the code or stop the share. Every client
request carries the secret.

If the recorded process is gone after a crash, `TerminateProcess` or a reboot, the next
command detects that and deletes the record. A recycled pid never counts as ours, because
the image path must match. Shutdown is cooperative: `stop` asks over the channel, and only
`stop --force` escalates to `TerminateProcess`. Daemons started by 1.0.x, which predate the
channel, are still found through their `soi.pid` file and stopped through their named event.

The log, `soi-share.log`, is truncated on each start. The exe also installs, updates and
uninstalls itself (`src/app/Lifecycle.cpp`): per-user, in
`%LOCALAPPDATA%\Programs\soi-share`, on the user PATH in `HKCU\Environment`, with the
program kept separate from the data folder so the share code survives both.

---

# Part 4 — Build

## Prerequisites

* Windows 10 2004 (build 19041) or later. Earlier works, but `WDA_EXCLUDEFROMCAPTURE` degrades to `WDA_MONITOR` (black-out instead of absent).
* Visual Studio 2019 16.11 or 2022, C++ desktop workload. **Verified on VS2019 BuildTools, MSVC 19.29, Windows SDK 10.0.19041.**
* CMake ≥ 3.20 (the version VS2019 bundles), Git.
* OpenSSL via vcpkg — needed by libdatachannel.

`libdatachannel`, `miniz` and (where needed) `{fmt}` are fetched automatically.

```powershell
vcpkg install openssl:x64-windows-static

cmake -B build -S . -G "Visual Studio 16 2019" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="<vcpkg>/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release --parallel
```

Output: `build\Release\soi-share.exe` plus `viewer.html`. Add `-DSOI_VERSION=1.2.3` to stamp a
version (`soi-share version`); release builds take it from the git tag. The full
maintainer walkthrough, including cutting a release, is in [SETUP.md](SETUP.md#maintainers-only).

**Runs on any laptop with no extra configuration**: the CRT is linked statically
(`/MT`), OpenSSL is static, and there is no runtime dependency beyond what ships with
Windows. `viewer.html` is also compiled into the exe, so **`soi-share.exe` on its own is
the whole install** — no installer, no redistributable, no admin rights, no GPU required
(software encoder fallback). A `viewer.html` placed next to the exe overrides the
embedded copy. `tools/check-dlls.ps1` reads the exe's import tables and fails the release
build if any DLL outside Windows' own ever appears.

Double-clicking the exe installs it and explains that it is a terminal program. It then
waits for Enter, or for S and Enter to start sharing on the spot.

### Toolchain notes found the hard way

* MSVC 19.29 ships `std::format` but **not** `std::format_string`. CMake probes the exact construct `util/Log.h` uses and fetches `{fmt}` only when that probe fails — a naive `std::format("{}",1)` probe passes and the build then fails deep inside the Windows SDK headers.
* `H264Encoder`'s constructor and destructor are defined in the `.cpp` on purpose: the COM interfaces are only forward-declared in the header, and an inline defaulted constructor would instantiate `ComPtr<ICodecAPI>::~ComPtr` in every translation unit that merely creates an encoder.

# Part 5 — Usage

```powershell
# Start sharing monitor 0. Returns immediately; the viewer opens with the offer loaded.
soi-share start --monitor 0

# Paste the answer the viewer produced
soi-share answer SOI1:eJxVkMtuwyAQRX...

# From any other terminal
soi-share status
soi-share stop

# Encrypted signalling, for a remote peer
soi-share start --monitor 0 --pass "correct-horse-battery-staple"

# Same LAN, zero external contact of any kind
soi-share start --monitor 0 --no-stun

# Every screen at once, as one wide picture
soi-share start --desktop

# Start on monitor 0, but let the viewer switch screens from their browser.
# This is the default -- the picker appears by itself on a multi-monitor PC.
soi-share start --monitor 0

# Monitor 1 and nothing else, whatever the viewer asks for
soi-share start --monitor 1 --lock-target

# A single window, with the cursor. Always locked: the viewer cannot widen
# this to a screen, and is not told what screens exist.
soi-share start --window "Visual Studio Code" --cursor

# Foreground with a live log, for debugging (Ctrl+C stops it)
soi-share start --foreground --verbose

soi-share list-monitors
soi-share list-windows
```

### Options

| Flag | Default | Meaning |
|---|---|---|
| `--monitor N` / `--desktop` / `--window <hwnd\|substr>` | `--monitor 0` | Capture target |
| `--fps N` | 30 | Capture rate, **hard-capped at 30** |
| `--bitrate K` | 6000 | Max/target kbps |
| `--min-bitrate K` | 600 | AIMD floor |
| `--max-width N` | 1920 | Downscale above this |
| `--gop S` | 10 | Keyframe interval, seconds |
| `--cursor` | off | Composite the mouse cursor |
| `--layered` | off | Include layered windows (CAPTUREBLT) |
| `--gpu` / `--cpu` / `--pipeline <mode>` | auto | Keep frames on the graphics card, or force the SSE2 path. See §3.2a |
| `--lock-target` | off | Pin the share to the named target. No screen picker, and the screen list is not sent. Implied by `--window` |
| `--protect` / `--no-protect` | on | `WDA_EXCLUDEFROMCAPTURE` + watchdog |
| `--pass <s>` | none | Encrypt signalling blobs |
| `--stun <url>` | Cloudflare + Google | STUN; repeatable. First use replaces defaults |
| `--no-stun` | — | Host candidates only |
| `--turn <url>` / `--turn-user` / `--turn-pass` | none | Optional relay |
| `--no-viewer` | — | Do not auto-launch the browser viewer |
| `--verbose` | off | Trace logging |

---

# Part 6 — Test results

`build\Release\soi-selftest.exe` runs the suite. Measured on this machine (Intel Quick Sync,
1920×1200 display, 8 pool threads):

| Area | Result |
|---|---|
| base64url | round-trips every length 0–64, URL-safe, rejects bad input |
| Signalling blob | byte-exact round-trip; 821 B SDP → 699 char blob; fresh salt/IV per encryption; **wrong passphrase, missing passphrase and a single flipped byte all rejected** |
| Colour conversion | primaries within 1.5 of the BT.709 reference; black→16, white→235, grey chroma exactly 128; SIMD tail correct at width 66; **1080p 1:1 in 0.51 ms** |
| Downscaling | flat fields stay flat; plateaus preserved; only the seam blends; **1920×1200→1280×800 in 2.01 ms** (was ~16.7 ms via HALFTONE) |
| Thread pool | every index visited exactly once across 7 sizes; 5000 back-to-back dispatches exact; **~2 µs per dispatch** |
| H.264 levels | 720p30→3.1, 1080p30→**4.0**, 1080p60→4.2, 4K30→5.1 |
| BitBlt capture | enumerates monitors/windows; frames non-uniform; **16.63 ms/frame at 1920×1200, refresh-rate-bound** (16.58 ms with CAPTUREBLT — free, within noise) |
| Capture backends | all three (DXGI, WGC, GDI) start, produce non-black frames and honour `--max-width`; **DXGI ~2300× cheaper per idle grab than GDI**; the factory prefers DXGI for monitors and refuses to route a window to it; WGC captures a DirectComposition window GDI renders black |
| Overlays | a layered, topmost overlay window is captured by DXGI, WGC, GDI+CAPTUREBLT and the automatic path — proven by pixel count, no `--layered` flag needed on the default path |
| Switching target | a retarget to a monitor that does not exist **reports failure and leaves a live capture at the previous size**; retargets to every screen at once and gets real content from it |
| Capture protection | **117600 probe pixels → 0 via BitBlt, PrintWindow, DXGI Desktop Duplication AND Windows.Graphics.Capture** — all four user-mode paths; reappears when cleared; watchdog re-protects new windows |
| H.264 encoder | Intel Quick Sync hardware MFT; 60/60 frames at paced 30fps in 2.00 s; Annex-B; SPS on every keyframe; CBR ~4241 kbps vs 3000 target on synthetic noise; 200-frame burst correctly dropped |

| GPU colour conversion | shader output read back and checked against the BT.709 reference: **black→16, white→235, grey→126, red→63**; grey is exactly neutral chroma; U and V are not swapped |
| GPU pipeline | capture→shader→hardware encoder end to end: the frame is a texture with no CPU copy taken, the MFT really accepts the D3D11 device, and the bitstream carries real content |

**171 assertions pass, 0 fail.**

### Interop: C++ sender ↔ browser viewer

The signalling blob is the one surface where two independent implementations must
agree byte for byte — Windows CNG + miniz on one side, WebCrypto +
`DecompressionStream` on the other. `node tests/interop.js` round-trips real
blobs through the actual `soi-selftest` binary in both directions:

```
PASS  C++ encodes -> browser decodes (unencrypted)
PASS  browser encodes -> C++ decodes (unencrypted)
PASS  C++ encrypts -> browser decrypts (AES-256-GCM + PBKDF2)
PASS  browser encrypts -> C++ decrypts (AES-256-GCM + PBKDF2)
PASS  C++ rejects a browser blob under the wrong passphrase
PASS  browser rejects a C++ blob under the wrong passphrase
PASS  browser detects a tampered C++ blob
PASS  browser accepts the blob from a viewer.html#... URL fragment

8 passed
```

### Detached lifecycle

`start` / `status` / `offer` / `stop` verified end to end: the daemon reaches
`awaiting-answer` in 0.276 s with an 865-character encrypted offer, reports
`MainWindowHandle 0` (no window), logs `console window: no console window is
attached to this process`, and is controllable and killable from a different
terminal than the one that started it.

### Bugs the testing found

Five bugs the suite found, all now fixed:

1. **`pumpFeed` called `ProcessInput` outside the lock.** It runs on both the caller's thread and the encoder's event thread, so two concurrent `ProcessInput` calls into an MFT were possible.
2. **HALFTONE StretchBlt cost 16.7 ms/frame**, capping the app at 20 fps. Found by benchmarking each GDI path separately — after the first hypothesis (CAPTUREBLT) was *disproved* by measurement. Capture went 50.85 ms → 16.63 ms.
3. **The daemon log only ever contained a BOM.** `_wfopen(..., "ccs=UTF-8")` puts the stream into wide orientation, after which every narrow `fprintf` silently writes nothing.
4. **`start` raced its own child.** The parent polled for daemon state before the child had written its pid file, read "not running", and reported failure while the daemon was in fact running fine.
5. **Thread-pool counter underflow** (caught in review): a lingering worker from a previous job could decrement the completion counter below zero and hang the next `parallelFor`.

The CAPTUREBLT episode is worth calling out: the obvious explanation for a
50 ms frame time was the flag everyone warns about, and it was wrong. Measuring
each GDI path in isolation found the real cause in one run.

# Part 7 — Known limits

1. **Symmetric NAT on both ends needs a relay.** Supply `--turn`; otherwise the app reports the failure rather than hanging.
2. **A target nothing can read is genuinely black.** The three backends between them cover GPU-composited windows, fullscreen games and overlay video (§1.2a). What remains black is what *no* user-mode API can read: a window another app protected with `SetWindowDisplayAffinity`, or the secure desktop (UAC / lock screen). `soi-share capture-check` says which case you are in. This is a Windows boundary, not a gap SOI can close.
3. **~60 fps is the GDI ceiling**, and `--fps` is capped at 30 by design. The DXGI and WGC backends are not refresh-rate-bound, so the cap is the limit there, not the backend.
4. **Audio is not implemented.** The transport supports a second track; WASAPI loopback into Opus is the natural addition.
5. **One viewer per process.** `Streamer` holds a single `PeerConnection`. Multiple viewers means one encoder fanned out to N packetizers — a contained change.
6. **AIMD is not GCC.** It reacts to loss, so it underperforms Google's delay-gradient estimator on bufferbloated links.
7. **`WDA_EXCLUDEFROMCAPTURE` protects SOI's own windows from capture, but it is not a security boundary**, and the terminal itself usually cannot be protected. See §1.2. It is now verified against all four user-mode capture APIs, WGC included.
8. **Sender is Windows-only.** The `FrameSource`/`H264Encoder` seams are portable; PipeWire + VAAPI would be the Linux pairing.
