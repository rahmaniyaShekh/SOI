# Installing soi-share from a GitHub release

**Short answer: download `soi-share.exe` and run it. There is no setup.**

The release exe is self-contained. You do **not** need an installer, admin rights, the
Visual C++ redistributable, OpenSSL, Node, a Cloudflare account, or anything from
[SETUP.md](SETUP.md). That guide is for building from source.

What makes that true:

- The C runtime, OpenSSL and the WebRTC stack are linked statically. The only DLLs the
  exe loads are ones that ship with Windows (Media Foundation, Direct3D 11, WinHTTP, CNG
  and similar).
- The browser viewer is compiled into the exe, so the exe does not need `viewer.html`
  next to it.
- The exe already points at a hosted rendezvous (`https://share.mdarif.online`), so
  short codes work out of the box.
- It writes its state to `%LOCALAPPDATA%\soi-share\` and nowhere else. It installs no
  service and writes nothing to the registry.

The person watching needs nothing but a modern browser.

---

## Requirements

| What | Notes |
|---|---|
| Windows 10 version 2004 (build 19041) or newer, 64-bit | Windows 11 is fine. Builds 1803–1909 run, but capture protection is weaker. |
| Media Foundation | Present on normal Windows. **"N" editions** (e.g. *Windows 11 Pro N*) lack it. Install the **Media Feature Pack** first: Settings → Apps → Optional features → Add a feature → *Media Feature Pack*. |
| Internet access | For the short-code route. On the same Wi-Fi, `--no-code` works without it. |

Not sure which Windows you have? Run `winver`.

---

## 1. Download

The [Releases page](https://github.com/rahmaniyaShekh/SOI/releases/latest) has two files:

| File | Use it when |
|---|---|
| **`soi-share.exe`** | Almost always. This single file is the whole install. |
| `soi-share-vX.Y.Z-windows-x64.zip` | You also want a standalone `viewer.html` to email someone for the offline `--no-code` flow. The exe inside the zip is the same file. |

### Option A: browser

Download `soi-share.exe` and put it in a folder you'll keep, e.g.
`C:\Users\<you>\Apps\soi-share\`.

Pick the folder now, before the first run. The Windows Firewall permission you grant on
first run applies to that exact path. If you move the exe later, Windows asks again.

### Option B: PowerShell (installs it and puts it on PATH)

After this you can type `soi-share` in any new terminal. It needs no admin rights,
because it installs for your user only.

```powershell
$dir = "$env:LOCALAPPDATA\Programs\soi-share"
New-Item -ItemType Directory -Force $dir | Out-Null
curl.exe -L --fail -o "$dir\soi-share.exe" "https://github.com/rahmaniyaShekh/SOI/releases/latest/download/soi-share.exe"
Unblock-File "$dir\soi-share.exe"

# Add the folder to your user PATH (once)
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (($userPath -split ";") -notcontains $dir) {
    [Environment]::SetEnvironmentVariable("Path", (($userPath -split ";" | Where-Object { $_ }) + $dir) -join ";", "User")
}
```

Close and reopen PowerShell, then check it works:

```powershell
soi-share --help
```

> **The repository is private.** While it stays private, the download links above only
> work for accounts with access to it. `curl` gets a 404, and a browser that isn't
> signed in sees "Page not found". Anyone with access can use the GitHub CLI instead:
>
> ```powershell
> gh auth login
> gh release download --repo rahmaniyaShekh/SOI --pattern soi-share.exe --dir "$env:LOCALAPPDATA\Programs\soi-share" --clobber
> ```
>
> The alternative is to copy `soi-share.exe` to the machine any way you like: USB stick,
> file share, chat. The exe is the whole install.

---

## 2. First run

**Double-click `soi-share.exe`.** A window opens, starts sharing, and shows your code:

```
  Your friend opens   share.mdarif.online
  and enters

      4JX-Q2N
```

Closing that window does **not** stop sharing. The share keeps running in the background.

On the first run only, Windows may show two prompts:

1. **"Windows protected your PC"** (SmartScreen). The exe is not code-signed, so Windows
   doesn't recognise it yet. Click **More info → Run anyway**. Downloading with Option B
   and running `Unblock-File` usually avoids this prompt.
2. **Windows Defender Firewall: "Allow soi-share to communicate on these networks?"** Tick
   **Private networks** and click **Allow**. This lets people on the same Wi-Fi reach the
   local handover page on port 8000. If you cancel, the short-code share usually still
   works, because it only needs outbound connections.

That's all. Nothing else needs configuring.

---

## 3. Your friend joins

They open **https://share.mdarif.online** in Chrome, Edge, Firefox or Safari, on any
network. Then they type the code and give it a name. Your screen appears within a
second or two. They can drop out and rejoin as often as they like while you're sharing.

The code belongs to your PC, not to the session. It stays the same after reboots and
updates, so a friend who has joined once can reconnect later by tapping the saved name.

---

## Everyday use

From a terminal (open one in the exe's folder, or anywhere if you used Option B):

```powershell
soi-share start              # start sharing in the background; prints the code
soi-share status             # what it's doing: resolution, fps, bitrate, the code
soi-share stop               # stop sharing
```

If you didn't add it to PATH, run it from its folder as `.\soi-share.exe start` and so on.

Not a terminal person? You can stop sharing without one:

- Open Task Manager → *Details* → end `soi-share.exe`, or
- Make a desktop shortcut to `soi-share.exe` and set its *Target* to
  `"C:\...\soi-share.exe" stop`.

Common options:

```powershell
soi-share start --quality 1080p          # start at 1080p (the viewer can still change it)
soi-share start --low                    # slow link (~1 Mbps): sharp 480p, lower frame rate
soi-share list-monitors                  # see your screens
soi-share start --monitor 1              # share the second screen
soi-share list-windows
soi-share start --window "Excel"         # share one window only
soi-share start --new-code               # issue a new code and revoke the old one
soi-share --help                         # everything else
```

---

## Updating

Your share code and settings live in `%LOCALAPPDATA%\soi-share\`, not next to the exe,
so replacing the exe keeps the code.

```powershell
soi-share stop
# then download the new soi-share.exe over the old one (same folder), e.g. re-run Option B
soi-share start
```

If you stop the old version before replacing the file, you avoid a "file in use" error.
Keeping the same folder avoids a fresh firewall prompt.

---

## Uninstalling

```powershell
soi-share stop
soi-share purge                                          # deletes the code, log and state
Remove-Item "$env:LOCALAPPDATA\Programs\soi-share" -Recurse -Force   # or wherever you put the exe
```

If you used Option B, remove the folder from your user PATH:
Settings → System → About → *Advanced system settings* → *Environment Variables* →
*Path* (under "User variables") → delete the `soi-share` entry.

Optional: the firewall rule Windows created stays behind but is harmless. To remove it,
run this in an **admin** PowerShell:

```powershell
Get-NetFirewallRule -DisplayName "soi-share*" | Remove-NetFirewallRule
```

---

## Troubleshooting

**Windows says the app "can't run on your PC", or Smart App Control blocked it.**
Smart App Control (on some fresh Windows 11 installs) blocks every unsigned app and has no
"Run anyway" button. It can be turned off in Windows Security → App & browser control →
Smart App Control. Note that Windows does not let you turn it back on without
reinstalling.

**The exe vanished after downloading, or Defender flagged it.**
A tool that captures the screen and runs in the background can trip heuristic
antivirus. Check Windows Security → Protection history. Only restore it if you
downloaded it from the official Releases page.

**Nothing happens, or the log says `MFStartup failed`.**
You're on an "N" edition without Media Foundation. Install the Media Feature Pack (see
[Requirements](#requirements)).

**`soi-share is already running`**
Only one share runs at a time. `soi-share status` shows its code; `soi-share stop` ends it.

**Your friend sees "Nobody is sharing with that code".**
The code was mistyped, or sharing isn't running. Check with `soi-share status`.

**It connects but the picture never appears.**
Both networks are probably behind strict (symmetric) NAT, which needs a relay. Look at
the log:

```powershell
Get-Content "$env:LOCALAPPDATA\soi-share\soi.log" -Tail 40
```

If the log says `ICE failed`, add a TURN server. The relay can't see your screen:

```powershell
soi-share start --turn turn:host:3478 --turn-user NAME --turn-pass SECRET
```

**The code page (`share.mdarif.online`) is down, or you'd rather not depend on it.**
Either share on the same Wi-Fi with `soi-share start --no-code`, or deploy your own
rendezvous (free Cloudflare plan) using [SETUP.md, Part B](SETUP.md#part-b--deploy-your-own-rendezvous)
and start with `soi-share start --service https://your-worker-url`. You don't need to
build anything for that.

**Check which capture method works on this PC:**

```powershell
soi-share capture-check
soi-share gpu-check
```
