## soi-share {{TAG}}: serverless P2P screen sharing from the terminal

One self-contained `soi-share.exe` for 64-bit Windows 10 (1803+) and 11, and one universal
`soi-share` binary for macOS 10.15+ on Apple silicon and Intel. You don't need to install
anything else: no runtime, no admin rights, no DLLs, no Homebrew.

### Install (PowerShell, one line)

The repository is private, so you need a read-only GitHub token. SETUP.md explains how to
create one (Contents: Read-only, this repository only). The token is read without
echoing it to the screen:

```powershell
$env:SOI_SHARE_GITHUB_TOKEN = [Net.NetworkCredential]::new('', (Read-Host 'GitHub token' -AsSecureString)).Password; irm https://api.github.com/repos/{{REPO}}/contents/install.ps1 -Headers @{ Authorization = "Bearer $env:SOI_SHARE_GITHUB_TOKEN"; Accept = 'application/vnd.github.raw' } | iex
```

Or download `soi-share.exe` below and double-click it. It installs itself and
explains what to do next.

### macOS: run it from its folder

Download `soi-share-macos.zip` below (Safari unzips it), then in Terminal:

```bash
cd ~/Downloads/soi-share
xattr -d com.apple.quarantine soi-share    # once: the binary is not notarized
./soi-share start
```

The first start asks for Screen Recording permission for your terminal app: allow it in
System Settings → Privacy & Security, quit and reopen the terminal, and start again. The
share runs in the background; `./soi-share status` and `./soi-share stop` control it.
`./soi-share install` is optional and puts it on your PATH.

### Use

```powershell
soi-share start      # share in the background; prints a 6-character code
soi-share status     # pid, uptime, the code, live stats
soi-share stop
soi-share update     # verified in-place update to the newest release
```

Your friend opens https://share.mdarif.online and types the code.

### Files

| File | What it is |
|---|---|
| `soi-share.exe` | The whole program, for Windows. |
| `soi-share-macos.zip` | The whole program, for macOS (universal: Apple silicon and Intel), with a README. |
| `install.ps1` | The installer the one-liner runs. |
| `SHA256SUMS.txt` | Checksums. `install.ps1` and `soi-share update` refuse a file that doesn't match (`shasum -a 256 -c SHA256SUMS.txt --ignore-missing` on a Mac). |
| `viewer.html` | Standalone viewer, only for the offline `--no-code` flow (the exe already contains it). |
| `THIRD_PARTY_NOTICES.md` | Licenses of the libraries compiled into the exe. |

The exe is not code-signed. SmartScreen may say "Windows protected your PC". If it
does, click **More info → Run anyway**. The macOS binary is ad-hoc signed, not notarized:
clear the download mark with `xattr` as above, or use **Allow Anyway** in System Settings →
Privacy & Security.

[SETUP.md](https://github.com/{{REPO}}/blob/{{TAG}}/SETUP.md) covers install options,
tokens, updating, uninstalling and troubleshooting.
