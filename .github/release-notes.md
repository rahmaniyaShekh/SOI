## soi-share {{TAG}}: serverless P2P screen sharing from the terminal

One self-contained `soi-share.exe` for 64-bit Windows 10 (1803+) and 11. You don't
need to install anything else: no runtime, no admin rights, no DLLs.

### Install (PowerShell, one line)

The repository is private, so you need a read-only GitHub token. SETUP.md explains how to
create one (Contents: Read-only, this repository only). The token is read without
echoing it to the screen:

```powershell
$env:SOI_SHARE_GITHUB_TOKEN = [Net.NetworkCredential]::new('', (Read-Host 'GitHub token' -AsSecureString)).Password; irm https://api.github.com/repos/{{REPO}}/contents/install.ps1 -Headers @{ Authorization = "Bearer $env:SOI_SHARE_GITHUB_TOKEN"; Accept = 'application/vnd.github.raw' } | iex
```

Or download `soi-share.exe` below and double-click it. It installs itself and
explains what to do next.

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
| `soi-share.exe` | The whole program. |
| `install.ps1` | The installer the one-liner runs. |
| `SHA256SUMS.txt` | Checksums. `install.ps1` and `soi-share update` refuse a file that doesn't match. |
| `viewer.html` | Standalone viewer, only for the offline `--no-code` flow (the exe already contains it). |
| `THIRD_PARTY_NOTICES.md` | Licenses of the libraries compiled into the exe. |

The exe is not code-signed. SmartScreen may say "Windows protected your PC". If it
does, click **More info → Run anyway**.

[SETUP.md](https://github.com/{{REPO}}/blob/{{TAG}}/SETUP.md) covers install options,
tokens, updating, uninstalling and troubleshooting.
