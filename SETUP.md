# Setting up soi-share on a Windows machine

Start to finish: install the toolchain, build the sender, deploy your own rendezvous,
share your screen. Every step is spelled out, including the ones that are easy to skip.

> **Only want to run it?** You don't need any of this. Download `soi-share.exe` from the
> GitHub Releases page and run it — see [INSTALL.md](INSTALL.md). This guide is for
> building from source and hosting your own rendezvous.

You need to do **Part A** (build) and **Part B** (rendezvous) once per machine. After
that, sharing is a single command.

Total time: about 30–45 minutes, most of it waiting for downloads and the first build.

---

## Before you start

| What | Why | Notes |
|---|---|---|
| Windows 10 version 1803 or newer, 64-bit | High-resolution timers, Media Foundation, per-monitor DPI | Windows 11 is fine |
| Windows 10 build 19041 (version 2004) for full capture protection | `WDA_EXCLUDEFROMCAPTURE` needs it; older builds degrade to `WDA_MONITOR` and log a warning | Only matters if a window of ours is ever on screen, which is rare for a detached daemon |
| ~8 GB free disk | vcpkg + OpenSSL + libdatachannel sources and build trees are large | |
| An internet connection | The build downloads dependencies from GitHub | |
| A Cloudflare account | Hosts the rendezvous. The free plan is enough | Sign up at dash.cloudflare.com |

You do **not** need Visual Studio (the full IDE), a GPU, or admin rights beyond what the
installers ask for.

> **Which terminal?** Use **PowerShell**. Every command below is PowerShell. Open it with
> `Win` → type `powershell` → Enter.

---

# Part A — Build the sender

## A1. Install Git

1. Download from <https://git-scm.com/download/win> and run the installer.
2. Accept every default.
3. Close and reopen PowerShell, then check it worked:

```powershell
git --version
```

You should see something like `git version 2.47.1.windows.2`. If you get
`git : The term 'git' is not recognized`, reopen PowerShell — the installer only updates
`PATH` for new terminals.

## A2. Install the Visual Studio Build Tools (the C++ compiler)

This is the compiler without the IDE.

1. Download **Build Tools for Visual Studio** from
   <https://visualstudio.microsoft.com/downloads/> (scroll to *Tools for Visual Studio* →
   *Build Tools for Visual Studio*).
2. Run it. In the installer, select the **Desktop development with C++** workload.
3. In the right-hand *Installation details* panel, make sure these are ticked (they are by
   default with that workload):
   - **MSVC v142 – VS 2019 C++ x64/x86 build tools** (or v143 for 2022 — either works)
   - **Windows 10 SDK** or **Windows 11 SDK**
   - **C++ CMake tools for Windows**
4. Click **Install**. This takes 10–20 minutes and several GB.

> **Why the CMake tools box matters:** it installs the `cmake.exe` this guide uses. If you
> skip it you will need to install CMake 3.20+ separately.

Find your CMake and remember the path — you will use it in every build command:

```powershell
Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" | Select-Object -ExpandProperty FullName
```

On a VS 2019 Build Tools install that prints:

```
C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

Save it into a variable for this session:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --version
```

## A3. Install Node.js

Needed for the rendezvous deploy tool and the test suites.

1. Download the **LTS** installer from <https://nodejs.org/> and run it with defaults.
2. Reopen PowerShell and check:

```powershell
node --version   # v20 or newer, e.g. v22.14.0
npm --version
```

## A4. Install vcpkg and OpenSSL

vcpkg supplies OpenSSL, which the WebRTC stack needs for DTLS.

```powershell
cd $env:USERPROFILE
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
```

Now build OpenSSL. **The triplet matters** — it must be `x64-windows-static`, because
soi-share links the static CRT so the resulting `.exe` runs on a machine with no Visual
C++ redistributable installed:

```powershell
.\vcpkg\vcpkg.exe install openssl:x64-windows-static
```

This compiles OpenSSL from source and takes 10–25 minutes. Leave it running.

Confirm it landed:

```powershell
.\vcpkg\vcpkg.exe list | Select-String openssl
```

You should see `openssl:x64-windows-static`.

> If you cloned vcpkg somewhere else, substitute your path everywhere below —
> the toolchain file is always `<your-vcpkg>\scripts\buildsystems\vcpkg.cmake`.

## A5. Get the source

Put it anywhere without unusual characters in the path:

```powershell
cd C:\
mkdir dev -Force
cd dev
# If you have the repo URL:
#   git clone <repo-url> SOI
# Otherwise copy the SOI folder here, then:
cd SOI
```

Check you are in the right place — this must list `CMakeLists.txt`, `src`, `cloud`:

```powershell
Get-ChildItem
```

## A6. Configure the build

```powershell
& $cmake -B build -S . -G "Visual Studio 16 2019" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
```

Using the **VS 2022** Build Tools instead? Change the generator:

```powershell
-G "Visual Studio 17 2022"
```

This step clones `miniz` and `libdatachannel` from GitHub, so it needs the network. The
last line of output should read:

```
-- soi: std::format=...  app=ON  selftest=ON
```

**`app=OFF` means OpenSSL was not found.** Recheck the triplet in A4 and the toolchain
path above; they must match.

## A7. Build

```powershell
& $cmake --build build --config Release --parallel
```

The first build compiles libdatachannel and takes 5–15 minutes. Later builds take
seconds. When it finishes you have:

```powershell
Get-ChildItem .\build\Release\
```

- `soi-share.exe` — the sender
- `soi-selftest.exe` — the test binary
- `viewer.html` — the offline viewer used by the LAN handover route

## A8. Check the build

```powershell
.\build\Release\soi-selftest.exe
```

Expect `154 passed`. This exercises all three capture backends (DXGI Desktop Duplication,
Windows.Graphics.Capture and GDI BitBlt) and the automatic choice between them, colour
conversion, the H.264 encoder, the signalling blob format, and verifies capture
protection against all four user-mode capture paths — BitBlt, PrintWindow, DXGI Desktop
Duplication and Windows.Graphics.Capture.

```powershell
node tests\interop.js
```

Expect `8 passed` — this proves the C++ and browser blob codecs agree byte for byte. One
line reading `decode failed: authentication failed...` is **expected**: it is a negative
test proving a wrong passphrase is rejected.

---

# Part B — Deploy your own rendezvous

The rendezvous is the only server involved. It turns a 6-character code into a lookup for
an encrypted connection blob. It cannot read your screen, your IP addresses, or the code
itself — see the comments at the top of `cloud/src/index.js`.

You can skip Part B entirely if you only ever share on the same Wi-Fi: use
`soi-share start --no-code` and the manual blob route. For a code that works from any
network, continue.

## B1. Log in to Cloudflare

```powershell
cd cloud
npx wrangler login
```

A browser opens; approve the request. Then confirm:

```powershell
npx wrangler whoami
```

## B2. Name your Worker

Open `cloud\wrangler.jsonc` and change the `name` to something unique to you — Worker
names are per-account, and the default `soi-share` may collide with an existing one:

```jsonc
"name": "my-screen-share",
```

## B3. Choose your URL

**Option 1 — use the free `workers.dev` subdomain (simplest).**
Delete or comment out the `routes` block in `cloud\wrangler.jsonc`:

```jsonc
// "routes": [
//   { "pattern": "share.example.com", "custom_domain": true }
// ],
```

Leave `"workers_dev": true`. Your URL will be
`https://<worker-name>.<your-subdomain>.workers.dev`.

**Option 2 — use your own domain.** The domain must already be on your Cloudflare
account. Set the pattern to the hostname you want; Wrangler creates the DNS record and
the certificate for you:

```jsonc
"routes": [
  { "pattern": "share.yourdomain.com", "custom_domain": true }
],
```

## B4. Deploy

```powershell
npx wrangler deploy
```

The output lists your URL and the bindings. You should see:

```
env.ROOMS (Room)     Durable Object
env.ASSETS           Assets
```

> **About the Durable Object:** the room lives in a Durable Object, not in Workers KV.
> This is not a stylistic choice. On KV the sender's poll for the viewer's answer hit a
> cached miss and did not see an answer that had already been written for up to ~50
> seconds — long enough that both peers gave up first, which is what made reconnection
> fail. A Durable Object is strongly consistent, so a write is visible to the very next
> read. `cloud/test-rendezvous.js` has a test that fails if this ever regresses.
>
> The first deploy creates the Durable Object class automatically via the `migrations`
> block. You do not need to create anything by hand.

## B5. Check the rendezvous

```powershell
node test-rendezvous.js https://your-worker-url
```

Expect `20 passed`. Among other things this proves the server stores only ciphertext,
never your code or SDP.

## B6. Point the sender at it

Either pass it each time:

```powershell
.\build\Release\soi-share.exe start --service https://your-worker-url
```

…or edit the default once, in `src/main.cpp`:

```cpp
std::string serviceUrl = "https://your-worker-url";
```

then rebuild with A7. Editing the default is worth it — you will type `start` a lot.

---

# Part C — Share your screen

## C1. Start sharing

```powershell
cd C:\dev\SOI
.\build\Release\soi-share.exe start
```

It prints your code and returns immediately — the streaming process runs detached, so you
can close the terminal:

```
  Your friend opens   your-worker-url
  and enters

      4JX-Q2N

  The screen appears as soon as they type it. Any network --
  they do not have to be on your Wi-Fi. The code belongs to this
  PC and stays the same next time. (copied to your clipboard)

  status: soi-share status      stop: soi-share stop
```

## C2. Your friend joins

They open your URL in any modern browser, type the code, and give it a name so they can
recognise it next time. The screen appears in a second or two.

The name is stored only in their browser and is never sent anywhere.

## C3. While it runs

```powershell
.\build\Release\soi-share.exe status    # resolution, bitrate, fps, loss, RTT
.\build\Release\soi-share.exe stop      # works from any terminal
```

**Your friend can join, leave, and rejoin as often as they like for as long as the sender
is running.** The code stays live, and reconnection is automatic:

- Their Wi-Fi drops and comes back → the picture returns by itself, no reload, no typing.
- You restart the sender → they reconnect by themselves within a few seconds.
- They close the tab and come back an hour later → they tap the saved name and are in.

The code belongs to the PC, not the session. It survives reboots. Rotate it with
`--new-code`, which also revokes it for anyone who already has it.

## C4. Picking quality

Your friend chooses the quality from the strip under the video: **360p, 480p,
720p (default), 1080p, source**. The change applies within a few seconds and is
remembered per screen, so it survives a reconnect.

**What happens on a slow link is the important part.** Most video tools drop the
bitrate and let the picture go soft. That is right for a face and wrong for a
screen: softening 9-pixel-tall text makes it unreadable, and unreadable text at
30fps is worth less than sharp text at 4fps.

So this holds the resolution and the per-frame bit budget fixed, and lets the
**frame rate** fall instead — as far as `--min-fps` (default 2). Scrolling gets
choppy on a bad connection; the text stays crisp. Screen content makes that
cheap, because identical frames are skipped entirely and a static screen costs
almost nothing either way.

> **Why "source" used to give you less than 720p.** Browsers answer the codec
> negotiation with `profile-level-id=4d001f` — H.264 level 3.1, whose ceiling is
> 3600 macroblocks, which is *exactly* 1280x720 — regardless of what the sender
> offered. The sender honours what the viewer says it can decode, so asking for
> 1920x1200 quietly produced 1202x752, and anything sent above the negotiated
> level made Chrome freeze (measured: four freezes totalling 2.1s in a
> five-second sample). The viewer page now states the level it can really decode
> (5.1), and the sender clamps to whatever the viewer claims. Measured after the
> fix: every level from 480p to 1920x1200 decodes with zero freezes and zero
> packet loss.

## C5. Switching screens

If your PC has more than one monitor, a **Screen** menu appears in the strip next
to Quality, listing each one as `1: 1920×1200 (main)`. Your friend picks; the
picture switches in a couple of seconds. The share code, the connection and
everything else stay exactly as they are — only the pixels change.

With a single monitor the menu is hidden entirely rather than shown with one
entry in it. Unplugging a screen mid-session is safe: the list is re-enumerated
every time it is sent, and a request for a monitor that has gone away is ignored
and the menu corrected, rather than ending the share.

The choice is deliberately **not** remembered between sessions — which screen is
worth watching changes from one conversation to the next.

## C6. What is protected from capture

Nothing belonging to soi-share appears in the stream, or in anyone else's screen
recording. Verified on this machine: the running daemon owns **zero** top-level
windows — there is nothing on screen to capture in the first place.

As a safety net for anything that ever does appear, every window this process
owns is marked `WDA_EXCLUDEFROMCAPTURE`, which DWM enforces at composition time.
That covers every user-mode capture path uniformly — BitBlt, PrintWindow, DXGI
Desktop Duplication and Windows.Graphics.Capture — and the test suite proves it
against all of them by probing the captured pixels.

Protection is established **before** the capture pipeline starts and stays up for
the entire life of the process, including the gaps between reconnect attempts,
and is re-swept every 200 ms.

> **What this is not.** Capture exclusion is a compositor feature, not a security
> boundary. It does not stop kernel-mode capture, mirror drivers, a DLL injected
> into this process, hardware HDMI capture, or a phone pointed at the screen.
>
> **One real exposure:** if you use `soi-share run` in the foreground instead of
> `start`, the terminal window belongs to WindowsTerminal.exe, not to this
> process, and `SetWindowDisplayAffinity` only works on your own windows — so the
> terminal IS visible to capture. The log says so explicitly when it happens. Use
> `start`, which is detached and has no console at all.

## C7. Useful options

```powershell
# Start at a particular quality (the viewer can still change it)
.\build\Release\soi-share.exe start --quality 1080p

# Slow connection (~1 Mbps): 480p held sharp, frame rate free to fall
.\build\Release\soi-share.exe start --low

# Pick a monitor
.\build\Release\soi-share.exe list-monitors
.\build\Release\soi-share.exe start --monitor 1

# Share one window instead of the whole screen
.\build\Release\soi-share.exe list-windows
.\build\Release\soi-share.exe start --window "Excel"

# Issue a new code and revoke the old one
.\build\Release\soi-share.exe start --new-code

# See every option
.\build\Release\soi-share.exe --help
```

---

# Troubleshooting

**`cmake : The term 'cmake' is not recognized`**
You did not set `$cmake`, or PowerShell was reopened since you did. Re-run the assignment
in A2 — `$cmake` only lasts for the current terminal session.

**Configure ends with `app=OFF` / `OpenSSL not found`**
The triplet and the toolchain file must agree. Re-run A4 with
`openssl:x64-windows-static` exactly, and check the `-DCMAKE_TOOLCHAIN_FILE` path in A6
points at a file that exists.

**`LINK : fatal error LNK1104: cannot open file 'soi-share.exe'`**
The daemon is running and holding the file. Stop it, then build again:

```powershell
.\build\Release\soi-share.exe stop
```

**`soi-share is already running`**
One daemon at a time by design. `soi-share stop` first, or `soi-share status` to see what
it is doing.

**Your friend sees "Nobody is sharing with that code"**
The code was mistyped, or the sender is not running. Check with `soi-share status`. The
page waits indefinitely for a code that has worked on their device before, and only
reports this for a code it has never connected with.

**It connects but the picture never appears**
Both of you are probably behind symmetric NAT, which needs a relay. Check the log:

```powershell
Get-Content "$env:LOCALAPPDATA\soi-share\soi.log" -Tail 40
```

`ICE failed` confirms it. Supply a TURN server:

```powershell
.\build\Release\soi-share.exe start --turn turn:host:3478 --turn-user NAME --turn-pass SECRET
```

The relay only ever sees DTLS-SRTP ciphertext; it cannot see your screen.

**Where is the log?**

```powershell
Get-Content "$env:LOCALAPPDATA\soi-share\soi.log" -Tail 50
```

That folder also holds `machine.code`, the PC's persistent share code.

**Nothing works and you want a clean slate**

```powershell
.\build\Release\soi-share.exe stop
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\soi-share"
Remove-Item -Recurse -Force .\build
```

Then redo A6 and A7. Deleting the state folder issues a new code on the next start.

---

# What gets installed where

| Thing | Location | Safe to delete? |
|---|---|---|
| Build output | `SOI\build\` | Yes — rebuild with A6, A7 |
| Share code, log, pid | `%LOCALAPPDATA%\soi-share\` | Yes — a new code is issued next start |
| vcpkg + OpenSSL | `%USERPROFILE%\vcpkg\` | Only if nothing else uses it |
| Rendezvous | Your Cloudflare account | `npx wrangler delete` from `cloud\` |

The sender writes nothing to the registry, installs no service, and creates no windows.
