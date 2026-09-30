# Setting up soi-share

**Using soi-share needs nothing installed first.** You don't need a runtime, compiler,
package manager, admin rights, Visual C++ redistributable or DLLs. The release is one
self-contained `soi-share.exe` that installs, updates and uninstalls itself. Part 1 is all
most people need.

[Part 2](#maintainers-only) is only for whoever builds the exe, publishes releases, or
hosts their own rendezvous.

---

# Part 1 — Using soi-share

## Requirements

| What | Notes |
|---|---|
| Windows 10 version 1803 or newer, 64-bit, or Windows 11 | Version 2004 (build 19041) or newer for full capture protection. Windows 11 on ARM works (x64 emulation). |
| Media Foundation | Present on normal Windows. **"N" editions** (e.g. *Windows 11 Pro N*) need the **Media Feature Pack**: Settings → Apps → Optional features → Add a feature → *Media Feature Pack*. `install`, `update` and `help` work without it; sharing does not. |
| Internet access | For the share code. On the same Wi-Fi, `--no-code` works without it. |

The person watching needs only a modern browser.

## Install

Every option below installs **for your Windows user only** (no admin rights) into:

| | Location |
|---|---|
| Program | `%LOCALAPPDATA%\Programs\soi-share\soi-share.exe` (this folder is added to your user PATH) |
| Your data: share code, log, saved token | `%LOCALAPPDATA%\soi-share\` |

Program and data are separate on purpose. Updating or uninstalling replaces the program
and never touches your share code, so a friend who saved your code can still use it.

### Option A — one line in PowerShell (recommended)

Open PowerShell (right-click Start → *Terminal*). The repository is private, so you need a
read-only token first; see [the next section](#private-repository-the-access-token). Paste
this line. It asks for the token without showing it and keeps it out of your command
history:

```powershell
$env:SOI_SHARE_GITHUB_TOKEN = [Net.NetworkCredential]::new('', (Read-Host 'GitHub token' -AsSecureString)).Password; irm https://api.github.com/repos/rahmaniyaShekh/SOI/contents/install.ps1 -Headers @{ Authorization = "Bearer $env:SOI_SHARE_GITHUB_TOKEN"; Accept = 'application/vnd.github.raw' } | iex
```

The installer then:

1. Checks you're on 64-bit Windows 10 1803+ or Windows 11.
2. Downloads the latest release's `soi-share.exe` and `SHA256SUMS.txt`, using the
   `curl.exe` that ships with Windows.
3. Refuses to continue unless the exe's SHA-256 matches.
4. Runs `soi-share.exe install`. This copies the exe into place, adds the folder to your
   user PATH, and saves the token encrypted for your Windows user so `soi-share update`
   works later.
5. Adds the folder to the PATH of **this** window too, so `soi-share` works straight away.

To install a particular version, first run `$env:SOI_SHARE_VERSION = 'v1.1.0'`.

If the repository ever becomes public, you won't need a token and the one-liner shrinks to:

```powershell
irm https://github.com/rahmaniyaShekh/SOI/releases/latest/download/install.ps1 | iex
```

### Option B — double-click

Download `soi-share.exe` from the
[latest release](https://github.com/rahmaniyaShekh/SOI/releases/latest) (signed in to
GitHub, since the repository is private) and double-click it. A window opens, installs
it as above, explains that soi-share is a terminal program and what to type, and waits
for you to press Enter. Type **S** and Enter instead to start sharing right away.

### Option C — you already have the exe

If you got `soi-share.exe` some other way (USB stick, file share, chat), run it once with
`install`:

```powershell
.\soi-share.exe install
```

Then open a new terminal. You can also skip installing entirely and run the exe from any
folder as `.\soi-share.exe start`. It works the same, just without the PATH entry.

### Private repository: the access token

A private repository's releases return **404 Not Found** to anyone who isn't signed in, and
that includes scripts. Windows PowerShell sometimes reports this misleadingly as "the
underlying connection was closed". To download, the installer and `soi-share update` send
a GitHub token that can do exactly one thing: read this repository.

**Who creates it.** GitHub's fine-grained tokens can only reach repositories owned by the
account or organisation that creates them. So **the repository owner creates the token**
and gives it to whoever should be able to install. Anyone else gets it from the owner.

1. Open <https://github.com/settings/personal-access-tokens/new> (Settings → Developer
   settings → Personal access tokens → *Fine-grained tokens* → *Generate new token*).
2. **Token name**: `soi-share install`. **Expiration**: whatever you're comfortable
   with; a year is reasonable. When it expires, updates stop until you make a new one.
3. **Resource owner**: the owner of `rahmaniyaShekh/SOI`.
4. **Repository access**: *Only select repositories* → `SOI`.
5. **Permissions** → *Repository permissions* → **Contents: Read-only**. GitHub adds
   *Metadata: Read-only* by itself. Leave everything else at *No access*.
6. **Generate token** and copy the `github_pat_…` value. GitHub shows it only once.

What soi-share does with it:

- The token reaches `soi-share.exe` only through an environment variable, never on a
  command line where other programs could read it.
- It's saved **encrypted with Windows DPAPI** for your Windows user, with an
  application-specific key, in `%LOCALAPPDATA%\soi-share\github-token.dpapi`. Other users
  of the PC, and copies of the file on another PC, can't decrypt it.
- **Only fine-grained tokens (`github_pat_…`) are saved.** A classic (`ghp_…`) or OAuth
  token, like the GitHub CLI's login, can reach every repository your account can. Such a
  token is used for the one run and then forgotten.
- `soi-share update --forget-token` deletes the saved token. Revoking it on GitHub
  (Settings → Developer settings → Fine-grained tokens) takes effect immediately.
- Where `update` looks for a token, in order: `SOI_SHARE_GITHUB_TOKEN`, then `GH_TOKEN`
  or `GITHUB_TOKEN` (used, never saved), then the saved token. If it finds none, it asks
  once, without echoing what you type.

## First run

The exe isn't code-signed, so Windows may show up to two prompts the first time:

1. **"Windows protected your PC"** (SmartScreen): click **More info → Run anyway**. The
   one-line installer usually avoids this, and `soi-share install` removes the
   "downloaded from the internet" mark from the installed copy.
2. **Windows Defender Firewall: "Allow soi-share to communicate…"**: tick **Private
   networks** and click **Allow**. This lets people on your Wi-Fi reach the local page on
   port 8000. If you cancel, sharing by code still works, because it only needs
   outbound connections.

## Share your screen

```powershell
soi-share start
```

It returns to the prompt straight away. The share runs in the background, so closing the
terminal doesn't stop it. It prints:

```
soi-share started in the background (pid 11732)
getting a share code...

  Your friend opens   share.mdarif.online
  and enters

      FV7-8NU

  The screen appears as soon as they type it. Any network --
  they do not have to be on your Wi-Fi. The code belongs to this
  PC and stays the same next time. (copied to your clipboard)

  log: C:\Users\you\AppData\Local\soi-share\soi-share.log

  next:  soi-share status     soi-share stop     soi-share help
```

Your friend opens **https://share.mdarif.online** in any modern browser, types the code and
gives it a name. The screen appears in a second or two. They can leave and rejoin as often
as they like while you're sharing, and reconnection after a network drop is automatic.

```powershell
soi-share status              # pid, uptime, state, the code, live stream stats
soi-share stop                # stop sharing (from any terminal)
soi-share start --foreground  # stay attached and watch the live log; Ctrl+C stops
```

Running `soi-share start` while a share is already running doesn't start a second one. It
shows the running share's code instead.

The code belongs to this PC, not to the session. It survives reboots, updates, and even
uninstalling and reinstalling. `soi-share start --new-code` issues a new one and revokes
the old one.

### Picking quality

Your friend picks the quality from the strip under the video: **360p, 480p, 720p
(default), 1080p, source**. On a slow link the resolution is held and the **frame rate**
falls instead, as low as `--min-fps` (default 2). Text stays sharp and scrolling gets
choppy, which is the right trade for a screen.

### Switching screens

With more than one monitor, a **Screen** menu appears next to Quality so your friend can
switch between them from their side. `--lock-target` pins the share to one screen, and a
`--window` share is always pinned.

### What is protected from capture

soi-share never appears in the stream or in anyone else's screen recording. The
background process owns no windows at all, and anything it ever owns is marked
`WDA_EXCLUDEFROMCAPTURE`. This is a compositor feature, **not a security boundary**; see
README §1.2. One exposure: with `start --foreground`, the terminal window belongs to your
terminal app, not to soi-share, so the terminal **is** visible to capture. Plain
`start` has no window at all.

### Useful options

```powershell
soi-share start --quality 1080p          # start at 1080p (your friend can still change it)
soi-share start --low                    # slow link (~1 Mbps): sharp 480p, lower frame rate
soi-share list-monitors                  # see your screens
soi-share start --monitor 1              # share the second screen
soi-share list-windows
soi-share start --window "Excel"         # share one window only
soi-share start --new-code               # new code; the old one stops working
soi-share help                           # everything else
```

## Updating

```powershell
soi-share update --check     # is there a newer release?
soi-share update             # install it
```

`update` downloads the latest release and refuses to install it unless its SHA-256
matches the release's `SHA256SUMS.txt`. It then runs the new exe once to prove it works,
stops a running share, and swaps the exe in place. Windows won't delete a running exe but
will rename one, so the old copy becomes `soi-share.exe.old` and is cleaned up
automatically later. If a share was running, it restarts on the new version with the same
options and the same code.

`update --force` reinstalls the latest release even if you already have it. "Already up to
date" means there is nothing newer.

## Uninstalling

```powershell
soi-share uninstall            # keeps your share code and settings
soi-share uninstall --purge    # removes those too
```

`uninstall` stops a running share, removes the folder from your user PATH, and deletes
`%LOCALAPPDATA%\Programs\soi-share`. The folder disappears a few seconds after the command
finishes, because a running exe can't delete itself. Without `--purge`, your data folder
stays, so reinstalling brings back the **same** share code.

The Windows Firewall rule created on first run stays behind. It's harmless. To remove
it, run this in an **admin** PowerShell:
`Get-NetFirewallRule -DisplayName "soi-share*" | Remove-NetFirewallRule`.

## Troubleshooting

**`soi-share : The term 'soi-share' is not recognized`**
Open a **new** terminal. PATH changes only reach terminals opened after the install,
except the window the one-line installer ran in. If a new terminal still doesn't find
it, run `& "$env:LOCALAPPDATA\Programs\soi-share\soi-share.exe" install` to put it back
on PATH.

**"Windows protected your PC"**
SmartScreen shows this because the exe isn't code-signed. Click **More info → Run
anyway**.

**"This app can't run on your PC" / blocked by Smart App Control**
Smart App Control, on some fresh Windows 11 installs, blocks every unsigned app and has no
"Run anyway". You can turn it off in Windows Security → App & browser control → Smart App
Control. Windows won't let you turn it back on without reinstalling.

**The exe vanished, or Defender flagged it**
A tool that captures the screen and runs in the background can trip heuristic
antivirus. Check Windows Security → Protection history. Restore it only if it came from
this repository's Releases page. The installer and `update` verify the SHA-256.

**Installer or `update`: "404" / "the repository is private: a token is needed"**
You didn't provide a token, or the token can't see this repository. It needs access to
`rahmaniyaShekh/SOI` with **Contents: Read-only**; see
[the token section](#private-repository-the-access-token).

**`update`: "GitHub rejected the token"**
The token has expired or been revoked. Run `soi-share update --forget-token`, then
`soi-share update`, and paste a new one.

**`start`: "soi-share is already running"**
Only one share runs at a time. It shows you that share's code. `soi-share stop` ends it.

**`start` or `status`: "running but not answering its control channel"**
The background process is stuck. Run `soi-share stop --force`, then start again.

**"An older version of soi-share is running"**
A share started by version 1.0.x is still running. `soi-share stop` stops it too.

**`start` takes 20–30 seconds before the code appears**
On some networks, the first direct connection to the code service times out before
soi-share falls back to the system proxy settings. The log says `rendezvous reachable
only through the system proxy`. The code still works; only the first publish is slow.

**Your friend sees "Nobody is sharing with that code"**
The code was mistyped, or sharing isn't running. Check with `soi-share status`.

**It connects but the picture never appears**
You're probably both behind strict (symmetric) NAT, which needs a relay. If the log
says `ICE failed`, add a TURN server. The relay only sees encrypted media:
`soi-share start --turn turn:host:3478 --turn-user NAME --turn-pass SECRET`.

**Where is the log?**
`Get-Content "$env:LOCALAPPDATA\soi-share\soi-share.log" -Tail 50`. It's rewritten on
each `start`. `soi-share status` shows its path.

**Nothing works and you want a clean slate**
`soi-share uninstall --purge`, then install again. You get a new share code.

**`MFStartup failed` / "Media Foundation is not available"**
You're on an "N" edition of Windows. Install the Media Feature Pack (see
[Requirements](#requirements)).

**Which capture method works on this PC?**
Run `soi-share capture-check` and `soi-share gpu-check`.

## What lives where

| Thing | Location | Removed by |
|---|---|---|
| The program | `%LOCALAPPDATA%\Programs\soi-share\` (+ its PATH entry) | `soi-share uninstall` |
| Share code, log, saved token | `%LOCALAPPDATA%\soi-share\` | `soi-share uninstall --purge` or `soi-share purge` |
| Firewall rule | Windows Defender Firewall | admin PowerShell, see [Uninstalling](#uninstalling) |

soi-share installs no service, and the only registry value it touches is your user
`Path` in `HKCU\Environment`.

---

# Maintainers only

**Nothing below is needed to use soi-share.** This part covers building `soi-share.exe`,
publishing a release, and hosting your own rendezvous.

## M1. Build from source

### Toolchain

1. **Git**: <https://git-scm.com/download/win>, with the defaults.
2. **Build Tools for Visual Studio** (2019 or 2022) from
   <https://visualstudio.microsoft.com/downloads/> → *Tools for Visual Studio*. Select the
   **Desktop development with C++** workload, and make sure **MSVC v142/v143**, a
   **Windows 10/11 SDK** and **C++ CMake tools for Windows** are ticked.
3. **Node.js LTS** from <https://nodejs.org/> (for the interop tests and the rendezvous).
4. **vcpkg + static OpenSSL**, the only dependency CMake doesn't fetch itself:

```powershell
cd $env:USERPROFILE
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg.exe install openssl:x64-windows-static     # 10–25 minutes
```

The triplet must be `x64-windows-static`. That, the static CRT (`/MT`), and CMake
fetching `libdatachannel`, `miniz` and (if needed) `{fmt}` as static libraries are what
make the exe need nothing beyond Windows.

Find the CMake that came with the Build Tools:

```powershell
$cmake = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" | Select-Object -First 1 -ExpandProperty FullName
& $cmake --version
```

### Configure, build, test

```powershell
git clone https://github.com/rahmaniyaShekh/SOI.git; cd SOI
& $cmake -B build -S . -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DSOI_VERSION=1.1.0
& $cmake --build build --config Release --parallel
```

The last configure line should read `soi: std::format=...  app=ON  selftest=ON`.
**`app=OFF` means OpenSSL wasn't found**; check that the triplet and the toolchain path
match. `SOI_VERSION` is what `soi-share version` prints and what `update` compares
against. Releases take it from the git tag.

```powershell
.\build\Release\soi-selftest.exe unit      # pure logic: PATH editing, checksums, DPAPI, JSON, ...
.\build\Release\soi-selftest.exe           # everything, incl. capture/encoder/GPU (needs a desktop)
node tests\interop.js                      # C++ and browser blob codecs agree byte for byte
powershell -File tools\check-dlls.ps1 build\Release\soi-share.exe
powershell -File tests\packaging-smoke.ps1 -Exe build\Release\soi-share.exe -Isolated
```

`check-dlls.ps1` reads the exe's import and delay-import tables and fails if any DLL is
not part of Windows. That catches a dynamic CRT or OpenSSL creeping back in.
`packaging-smoke.ps1` installs, reinstalls and uninstalls for real (with `-Isolated`, into
a throwaway LOCALAPPDATA), checking the PATH entry and the Zone.Identifier removal. It
puts your user PATH back afterwards.

`interop.js` prints one `decode failed: authentication failed...` line. That's expected:
it's the negative test for a wrong passphrase.

**`LNK1104: cannot open file 'soi-share.exe'`** means a share is running from the build
folder. Run `.\build\Release\soi-share.exe stop` first.

## M2. Publish a release

Users never build anything. They get the exe that GitHub Actions builds
(`.github/workflows/release.yml`) on a `windows-latest` runner.

```powershell
git tag v1.2.0
git push origin main v1.2.0
gh run watch        # or the Actions tab
```

The workflow:

1. Builds with `-DSOI_VERSION` taken from the tag, so `v1.2.0` becomes `1.2.0`.
2. Runs the unit tests, the DLL allow-list check, the version-stamp check and the
   install/uninstall smoke test. Any failure stops the release.
3. Stages `soi-share.exe`, `install.ps1`, `viewer.html` and `THIRD_PARTY_NOTICES.md`, and
   writes `SHA256SUMS.txt` in `sha256sum` format.
4. Creates the release, or updates it with `--clobber` if the tag was published before.
5. Runs the documented one-liner in Windows PowerShell 5.1 against what it just published,
   and checks that the installed `soi-share version` matches.

Running the workflow by hand (*Actions → Build and Release → Run workflow*) builds and
tests the current commit without publishing. Give it a tag to publish or re-publish that
release. A tag with a suffix (`v1.2.0-rc.1`) is published as a prerelease, which
`releases/latest` and `soi-share update` ignore.

`install.ps1` is fetched by the one-liner from the **default branch**, so changes to it
reach users as soon as they're merged, not at the next tag.

**Third-party notices.** The exe statically links the libraries listed in
`THIRD_PARTY_NOTICES.md`, whose license texts are in `third_party/licenses/`. The file is
compiled into the exe (`soi-share licenses`) and copied beside the installed copy.
Update both when a dependency changes.

## M3. Host your own rendezvous (optional)

The rendezvous turns a 6-character code into a lookup for an encrypted connection blob.
It can't read your screen, your IP addresses, or the code itself; see
`cloud/src/index.js`. The default, `https://share.mdarif.online`, already works. Deploy
your own only if you'd rather not depend on it:

```powershell
cd cloud
npx wrangler login
# set a unique "name" in wrangler.jsonc; for a workers.dev URL, remove the "routes" block,
# or set routes[0].pattern to a hostname on your own Cloudflare zone
npx wrangler deploy
node test-rendezvous.js https://your-worker-url     # expect 20 passed
```

The room lives in a Durable Object, not in Workers KV, and that isn't a stylistic choice.
KV served cached misses for up to ~50 s, which broke reconnection.
`cloud/test-rendezvous.js` fails if that ever regresses.

Point the sender at it with `soi-share start --service https://your-worker-url`, or change
the `serviceUrl` default in `src/main.cpp` and rebuild.
