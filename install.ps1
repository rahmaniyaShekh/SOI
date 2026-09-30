# soi-share installer for Windows 10/11 (x64). Per-user, no admin rights.
#
# Downloads the latest release of soi-share.exe from GitHub, verifies it against
# the release's SHA256SUMS.txt, and runs `soi-share.exe install`, which copies it
# to %LOCALAPPDATA%\Programs\soi-share and adds that folder to your user PATH.
# It also puts that folder on the PATH of THIS PowerShell window, so
# `soi-share` works straight away.
#
# The repository is private, so a read-only GitHub token is needed. Run:
#
#   $env:SOI_SHARE_GITHUB_TOKEN = [Net.NetworkCredential]::new('', (Read-Host 'GitHub token' -AsSecureString)).Password
#   irm https://api.github.com/repos/rahmaniyaShekh/SOI/contents/install.ps1 -Headers @{ Authorization = "Bearer $env:SOI_SHARE_GITHUB_TOKEN"; Accept = 'application/vnd.github.raw' } | iex
#
# Optional: $env:SOI_SHARE_VERSION = 'v1.1.0' installs that release instead of
# the latest.
#
# Everything is inside a script block so that, run through `iex`, nothing leaks
# into your session and a failure returns to the prompt instead of closing it.

& {
    $ErrorActionPreference = 'Stop'
    $Repo  = 'rahmaniyaShekh/SOI'
    $Exe   = 'soi-share.exe'
    $Sums  = 'SHA256SUMS.txt'
    $Token = $env:SOI_SHARE_GITHUB_TOKEN
    $Tag   = $env:SOI_SHARE_VERSION

    function Say([string]$Text) { Write-Host $Text }
    function Fail([string]$Text) {
        Write-Host ''
        Write-Host "soi-share was NOT installed: $Text" -ForegroundColor Red
    }

    # ---- this machine --------------------------------------------------------
    $build = [Environment]::OSVersion.Version.Build
    if ([Environment]::OSVersion.Platform -ne 'Win32NT' -or [Environment]::OSVersion.Version.Major -lt 10 -or $build -lt 17134) {
        Fail "soi-share needs Windows 10 version 1803 or newer (this is $([Environment]::OSVersion.VersionString)). Windows 11 is fine."
        return
    }
    if (-not [Environment]::Is64BitOperatingSystem) {
        Fail 'soi-share needs 64-bit Windows; this is 32-bit Windows.'
        return
    }
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
    if ($arch -eq 'ARM64' -and $build -lt 22000) {
        Fail 'this is Windows 10 on ARM, which cannot run x64 programs. Windows 11 on ARM can.'
        return
    }
    if (-not $env:LOCALAPPDATA) { Fail 'LOCALAPPDATA is not set, so there is no per-user folder to install into.'; return }
    if ($build -lt 19041) {
        Say 'Note: Windows 10 before version 2004 works, but capture protection is weaker there.'
    }

    # The curl.exe that ships with Windows, not PowerShell's `curl` alias. It is
    # used for every request because Windows PowerShell 5.1 mishandles the
    # authenticated download (see "Download" below) and reports a private repo's
    # 404 as a vague "connection closed".
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (-not (Test-Path $curl)) { Fail "$curl is missing (it ships with Windows 10 1803 and later)."; return }

    $work = Join-Path ([IO.Path]::GetTempPath()) ('soi-share-install-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null

    # The token never appears on a command line, where any process could read
    # it. curl reads it from a config file in $work: a fresh folder inside the
    # per-user %TEMP% (only this user can open it), deleted in `finally`.
    #
    # Not stdin: Windows PowerShell 5.1 can prefix what it pipes to a native
    # program with a UTF-8 byte-order mark, whatever $OutputEncoding says, and
    # curl then rejects the first line of its config.
    $curlConfig = Join-Path $work 'curl.cfg'
    function Get-Url([string]$Url, [string]$Accept, [string]$OutFile, [bool]$WithToken) {
        # curl's exit status is checked explicitly below.
        $ErrorActionPreference = 'Continue'
        $config = "# soi-share installer`n"
        if ($WithToken -and $Token) { $config += 'header = "Authorization: Bearer ' + $Token + "`"`n" }
        [IO.File]::WriteAllText($curlConfig, $config, [Text.Encoding]::ASCII)
        $errFile = Join-Path $work 'curl.err'
        Remove-Item $errFile -ErrorAction SilentlyContinue
        # No -L: redirects are followed by hand below, so the token can be
        # dropped when GitHub redirects to its storage host. --stderr rather
        # than 2>, which Windows PowerShell would wrap in its own error text.
        $out = & $curl --stderr $errFile --config $curlConfig --silent --show-error --proto '=https' --tlsv1.2 `
            -H "Accept: $Accept" -H 'X-GitHub-Api-Version: 2022-11-28' -H 'User-Agent: soi-share-installer' `
            -o $OutFile -w '%{http_code} %{redirect_url}' $Url
        Remove-Item $curlConfig -ErrorAction SilentlyContinue
        $parts = "$out".Trim() -split ' ', 2
        $result = [pscustomobject]@{ Status = [int]$parts[0]; Redirect = ''; Error = '' }
        if ($parts.Count -gt 1) { $result.Redirect = $parts[1] }
        if ($LASTEXITCODE -ne 0) { $result.Error = "$(Get-Content $errFile -Raw -ErrorAction SilentlyContinue)".Trim() }
        return $result
    }

    try {
        # ---- which release ---------------------------------------------------
        $what = 'the latest release'
        $api = "https://api.github.com/repos/$Repo/releases/latest"
        if ($Tag) { $what = "release $Tag"; $api = "https://api.github.com/repos/$Repo/releases/tags/$Tag" }
        Say "soi-share installer: looking up $what of $Repo..."

        $meta = Join-Path $work 'release.json'
        $r = Get-Url $api 'application/vnd.github+json' $meta $true
        if ($r.Status -eq 0) { Fail "could not reach api.github.com: $($r.Error) Check the internet connection (or proxy)."; return }
        if ($r.Status -eq 401) { Fail 'GitHub rejected the token (expired, revoked or mistyped). Create a new one; see SETUP.md.'; return }
        if ($r.Status -eq 404) {
            if ($Token) { Fail "GitHub returned 404 with your token: it cannot see $Repo (it needs access to that repository with Contents: Read-only), or $what does not exist." }
            else { Fail "GitHub returned 404. The repository $Repo is private: a token is needed. Set `$env:SOI_SHARE_GITHUB_TOKEN first; see SETUP.md." }
            return
        }
        if ($r.Status -ne 200) { Fail "GitHub answered HTTP $($r.Status) for $api."; return }

        $release = Get-Content $meta -Raw | ConvertFrom-Json
        $exeAsset  = $release.assets | Where-Object { $_.name -eq $Exe }  | Select-Object -First 1
        $sumsAsset = $release.assets | Where-Object { $_.name -eq $Sums } | Select-Object -First 1
        if (-not $exeAsset -or -not $sumsAsset) { Fail "release $($release.tag_name) has no $Exe or no $Sums; refusing to install an unverifiable binary."; return }

        # ---- download ----------------------------------------------------------
        # Through the API asset URL, which works for private repositories. GitHub
        # redirects it to a pre-signed storage URL that must be fetched WITHOUT
        # the Authorization header -- that host rejects a second credential.
        function Get-Asset($Asset, [string]$OutFile) {
            $r = Get-Url $Asset.url 'application/octet-stream' $OutFile $true
            for ($hop = 0; $hop -lt 5 -and $r.Status -ge 300 -and $r.Status -lt 400 -and $r.Redirect; $hop++) {
                $r = Get-Url $r.Redirect 'application/octet-stream' $OutFile $false
            }
            if ($r.Status -ne 200) { throw "downloading $($Asset.name) failed (HTTP $($r.Status)) $($r.Error)" }
        }

        Say ("downloading {0} {1} ({2:N1} MB)..." -f $Exe, $release.tag_name, ($exeAsset.size / 1MB))
        $exePath  = Join-Path $work $Exe
        $sumsPath = Join-Path $work $Sums
        Get-Asset $sumsAsset $sumsPath
        Get-Asset $exeAsset $exePath

        # ---- verify ------------------------------------------------------------
        $expected = $null
        foreach ($line in (Get-Content $sumsPath)) {
            if ($line -match '^\s*([0-9a-fA-F]{64})\s+\*?(\S.*?)\s*$' -and $Matches[2] -eq $Exe) {
                $expected = $Matches[1].ToLowerInvariant()
            }
        }
        if (-not $expected) { Fail "$Sums has no entry for $Exe; refusing to install."; return }
        $actual = (Get-FileHash -Algorithm SHA256 $exePath).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            Fail "CHECKSUM MISMATCH for $Exe (expected $expected, got $actual). The download is corrupt or was tampered with."
            return
        }
        Say "sha256 verified: $actual"

        # ---- install -----------------------------------------------------------
        # The exe does the real work: copies itself, edits the user PATH,
        # broadcasts the change, and saves the token (DPAPI) if it is a
        # fine-grained one -- all read from the environment, never argv.
        $env:SOI_SHARE_INSTALLER = '1'
        & $exePath install
        $code = $LASTEXITCODE
        Remove-Item Env:SOI_SHARE_INSTALLER -ErrorAction SilentlyContinue
        if ($code -ne 0) { Fail "soi-share.exe install exited with code $code (see above)."; return }

        # PATH changes only reach terminals opened from now on. Fix up this one.
        $dir = Join-Path $env:LOCALAPPDATA 'Programs\soi-share'
        $inSession = ($env:Path -split ';' | ForEach-Object { $_.Trim().Trim('"').TrimEnd('\') }) -contains $dir
        if (-not $inSession) { $env:Path = ($env:Path.TrimEnd(';') + ';' + $dir) }

        Say ''
        Write-Host 'Installed. In this window and any new one:' -ForegroundColor Green
        Say '    soi-share start      start sharing; prints a code for your friend'
        Say '    soi-share status     see what it is doing'
        Say '    soi-share stop       stop sharing'
        Say '    soi-share help       everything else'
    }
    catch {
        Fail $_.Exception.Message
    }
    finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
        # Saved (encrypted) by the exe if it was worth keeping; no need to leave
        # it in the session.
        Remove-Item Env:SOI_SHARE_GITHUB_TOKEN -ErrorAction SilentlyContinue
    }
}
