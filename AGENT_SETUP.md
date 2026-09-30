# AGENT_SETUP.md — instructions for a coding agent

You are setting up **soi-share** on a fresh Windows machine, start to finish, without
asking the user anything. Finish with the app **running** and hand back the share code.

`SETUP.md` is the human version with explanations. This file is the executable one: run
the steps in order, check each gate, stop and report if a gate fails.

**Shell: PowerShell.** Everything below is PowerShell. Never `cd` into a step's directory
and assume it persists — each block states its own directory.

---

## 0. Preconditions

```powershell
[Environment]::OSVersion.Version        # Build >= 19041
(Get-CimInstance Win32_OperatingSystem).OSArchitecture   # 64-bit
(Get-PSDrive C).Free / 1GB              # need ~8 GB
```

**Gate:** build >= 19041, 64-bit, >= 8 GB free. If the build is 17134–19040 continue
anyway, but note in your final report that capture protection degrades to `WDA_MONITOR`.

---

## 1. Toolchain

Install only what is missing. Check first — these are slow installs.

```powershell
git --version
node --version      # want v20+
```

Install the missing ones with winget (non-interactive flags matter — no installer will
prompt):

```powershell
winget install --id Git.Git               -e --accept-source-agreements --accept-package-agreements
winget install --id OpenJS.NodeJS.LTS     -e --accept-source-agreements --accept-package-agreements
winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
```

The Build Tools line takes 10–20 min and returns only when done (`--wait`). Do not
background it.

`PATH` changes do not reach an already-running shell. After installing, refresh it in
process rather than telling the user to reopen anything:

```powershell
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
            [Environment]::GetEnvironmentVariable("Path","User")
```

Locate CMake and keep it in `$cmake` for the whole session:

```powershell
$cmake = (Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" |
          Select-Object -First 1).FullName
& $cmake --version
```

**Gate:** `git`, `node`, and `$cmake` all report versions. If `$cmake` is empty the C++
workload did not install — re-run the Build Tools line and read its exit code.

---

## 2. OpenSSL via vcpkg

The triplet is **not** negotiable: `x64-windows-static`. The project links the static CRT
so the `.exe` runs on a machine with no VC++ redistributable. A dynamic triplet configures
fine and then fails to link.

```powershell
if (-not (Test-Path "$env:USERPROFILE\vcpkg")) {
  git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\vcpkg"
  & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
}
& "$env:USERPROFILE\vcpkg\vcpkg.exe" install openssl:x64-windows-static
```

10–25 min, compiling from source.

**Gate:**

```powershell
& "$env:USERPROFILE\vcpkg\vcpkg.exe" list | Select-String "openssl:x64-windows-static"
```

must print a line.

---

## 3. Build

From the repo root (the directory holding `CMakeLists.txt`):

```powershell
& $cmake -B build -S . -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
```

Omitting `-G` lets CMake pick whichever Visual Studio generation is installed — do not
hardcode `"Visual Studio 16 2019"`. Configure needs the network: it clones `miniz` and
`libdatachannel`.

**Gate:** configure output ends with `app=ON`. **`app=OFF` means OpenSSL was not found** —
the triplet in step 2 and `-DCMAKE_TOOLCHAIN_FILE` here disagree. Fix that; do not build.

```powershell
& $cmake --build build --config Release --parallel
```

First build: 5–15 min.

**Gate:** `build\Release\` contains `soi-share.exe`, `soi-selftest.exe`, `viewer.html`.

> If linking fails with `LNK1104: cannot open file 'soi-share.exe'`, a daemon from an
> earlier run holds it. `.\build\Release\soi-share.exe stop`, then build again.

---

## 4. Verify before deploying

```powershell
.\build\Release\soi-selftest.exe        # expect: 154 passed
node tests\interop.js                   # expect: 8 passed
```

One `decode failed: authentication failed...` line inside the interop run is **expected** —
it is the negative test proving a wrong passphrase is rejected.

**Gate:** both counts exact. A lower count is a real failure — report it with the failing
test names and stop. Do not deploy a broken build.

---

## 5. Rendezvous (Cloudflare Worker)

This step needs a browser login and a Cloudflare account, so it is the one place you may
have to involve the user. Try the non-interactive path first: if `CLOUDFLARE_API_TOKEN` is
set in the environment, `wrangler` uses it and no browser opens.

```powershell
cd cloud
npx wrangler whoami        # already logged in?
```

If that fails and no API token is present, **pause and ask the user to run
`npx wrangler login`** — you cannot complete an OAuth flow for them. While waiting, do not
skip ahead; step 6 needs the URL this produces.

Two edits are required before deploying, because `cloud\wrangler.jsonc` is committed with
the original author's settings:

1. `"name"` — change from `soi-share` to something unique to this account.
2. `"routes"` — it points at `share.mdarif.online`, a domain this account does not own.
   **Delete or comment out the whole `routes` block** and keep `"workers_dev": true`. The
   URL becomes `https://<name>.<subdomain>.workers.dev`. Only keep a `routes` entry if the
   user has their own zone on Cloudflare and tells you the hostname.

```powershell
npx wrangler deploy
```

**Gate:** output lists `env.ROOMS (Room) Durable Object` and `env.ASSETS Assets`, and
prints the deployed URL. Capture that URL — call it `$svc`.

```powershell
node test-rendezvous.js $svc        # expect: 20 passed
cd ..
```

**Gate:** 20 passed.

> The room is a Durable Object, not KV, deliberately: on KV the sender's poll for the
> viewer's answer read a cached miss for up to ~50 s and reconnection failed. One of those
> 20 tests fails if that regresses.

---

## 6. Run it

The compiled default service URL is `https://share.mdarif.online`, which is **not** the
Worker you just deployed. Pass `$svc` explicitly:

```powershell
.\build\Release\soi-share.exe start --service $svc
```

`start` returns immediately — the streamer is detached and survives the terminal closing.
Do not use `run` or `start --foreground`: they stay in the terminal, and the terminal
window belongs to WindowsTerminal.exe, not to this process, so capture exclusion cannot
cover it.

**Gate:**

```powershell
.\build\Release\soi-share.exe status
```

reports a live session (resolution, bitrate, fps). If it does not:

```powershell
Get-Content "$env:LOCALAPPDATA\soi-share\soi-share.log" -Tail 40
```

`ICE failed` means both peers are behind symmetric NAT and a TURN server is needed
(`--turn turn:host:3478 --turn-user NAME --turn-pass SECRET`).

To make `$svc` the permanent default instead of typing it each time, set it in
`src\main.cpp` (`std::string serviceUrl = ...`, ~line 110) and rebuild step 3. Do this
only if the user asks — it is a source edit.

---

## 7. Report back

Hand the user exactly this, filled in:

- **The 6-character share code** printed by `start` (it is also on their clipboard).
- **The viewer URL** (`$svc`) their friend opens to enter the code.
- Test results: `154 passed`, `8 passed`, `20 passed`.
- Anything you changed in `cloud\wrangler.jsonc`.
- Any gate you had to waive, and why.

The code belongs to the machine, not the session: it survives reboots and restarts.
Rotate it with `start --new-code`, which also revokes it for anyone holding the old one.

---

## Cleanup / start over

```powershell
.\build\Release\soi-share.exe stop
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\soi-share"   # new code next start
Remove-Item -Recurse -Force .\build                          # then redo step 3
```

`npx wrangler delete` from `cloud\` removes the Worker.
