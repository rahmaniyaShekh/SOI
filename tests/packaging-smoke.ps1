<#
.SYNOPSIS
  End-to-end check of soi-share's self-install: install, PATH, reinstall,
  uninstall -- run for real against a built exe.

.DESCRIPTION
  -Isolated points LOCALAPPDATA at a throwaway folder first, so the program
  and data folders are created there. The user PATH in HKCU is real either way
  (that is the thing being tested); the entry this adds is removed again by the
  uninstall step, and the script restores the original value if anything fails.

  CI runs this with -Isolated. Run without it only when you mean to install for
  real.

.EXAMPLE
  powershell -File tests\packaging-smoke.ps1 -Exe build\Release\soi-share.exe -Isolated
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$ExpectVersion = '',
    [switch]$Isolated
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$failures = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "  PASS  $What" -ForegroundColor Green }
    else     { Write-Host "  FAIL  $What" -ForegroundColor Red; $script:failures++ }
}
function UserPath { [Environment]::GetEnvironmentVariable('Path', 'User') }
function Count-Entries([string]$PathValue, [string]$Dir) {
    @($PathValue -split ';' | Where-Object { $_.Trim().Trim('"').TrimEnd('\') -ieq $Dir.TrimEnd('\') }).Count
}

$originalPath = UserPath
$originalKind = (Get-Item 'HKCU:\Environment').GetValueKind('Path')
if ($Isolated) {
    $fake = Join-Path ([IO.Path]::GetTempPath()) ('soi-smoke-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $fake | Out-Null
    $env:LOCALAPPDATA = $fake
    Write-Host "LOCALAPPDATA -> $fake"
}
$dir       = Join-Path $env:LOCALAPPDATA 'Programs\soi-share'
$installed = Join-Path $dir 'soi-share.exe'
$data      = Join-Path $env:LOCALAPPDATA 'soi-share'

try {
    Write-Host "== version =="
    $v = (& $Exe version --short | Out-String).Trim()
    Check ($LASTEXITCODE -eq 0 -and $v -match '^\d+\.\d+\.\d+') "version --short prints a version ($v)"
    if ($ExpectVersion) { Check ($v -eq $ExpectVersion) "version is $ExpectVersion" }
    $help = & $Exe help | Out-String
    Check ($help -match 'QUICK START' -and $help -match 'uninstall') 'help lists QUICK START and the install commands'
    & $Exe definitely-not-a-command | Out-Null
    Check ($LASTEXITCODE -eq 2) 'an unknown command exits 2'

    Write-Host "== install =="
    # A copy carrying the internet zone mark, as a browser download would.
    $staged = Join-Path ([IO.Path]::GetTempPath()) ('soi-share-dl-' + [Guid]::NewGuid().ToString('N') + '.exe')
    Copy-Item $Exe $staged
    Set-Content -Path $staged -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3"
    $out = & $staged install | Out-String
    Write-Host $out
    Check ($LASTEXITCODE -eq 0) 'install exits 0'
    Check (Test-Path $installed) "installed to $installed"
    Check (-not (Get-Item $installed -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'Zone.Identifier removed from the installed copy'
    Check (Test-Path (Join-Path $dir 'THIRD_PARTY_NOTICES.md')) 'THIRD_PARTY_NOTICES.md placed beside it'
    Check ((Count-Entries (UserPath) $dir) -eq 1) 'user PATH has exactly one entry'
    Check ((Get-Item 'HKCU:\Environment').GetValueKind('Path') -eq 'ExpandString') 'PATH is written as REG_EXPAND_SZ'
    Remove-Item $staged -Force -ErrorAction SilentlyContinue

    & $installed install | Out-Null
    Check ($LASTEXITCODE -eq 0 -and (Count-Entries (UserPath) $dir) -eq 1) 'reinstalling from the installed copy keeps one entry'
    & $Exe install | Out-Null
    Check ($LASTEXITCODE -eq 0 -and (Count-Entries (UserPath) $dir) -eq 1) 'installing again from elsewhere keeps one entry'

    $ver = & $installed version | Out-String
    Check ($ver -match 'the installed copy') 'version reports it is the installed copy'
    & $installed status | Out-Null
    Check ($LASTEXITCODE -eq 1) 'status says not running (exit 1)'

    # A stale instance record from a "crashed" run is detected and removed.
    New-Item -ItemType Directory -Force $data | Out-Null
    Set-Content -Path (Join-Path $data 'instance.txt') -Value "pid=4294967000`nport=1`nsecret=00`nexe=C:\nope.exe" -NoNewline
    $st = & $installed status | Out-String
    Check ($st -match 'stale' -and -not (Test-Path (Join-Path $data 'instance.txt'))) 'a stale instance record is cleaned up'

    Write-Host "== uninstall =="
    Set-Content -Path (Join-Path $data 'keep-me.txt') -Value 'user data'
    $out = & $installed uninstall | Out-String
    Write-Host $out
    Check ($LASTEXITCODE -eq 0) 'uninstall exits 0'
    Check ((Count-Entries (UserPath) $dir) -eq 0) 'PATH entry removed'
    $gone = $false
    for ($i = 0; $i -lt 40 -and -not $gone; $i++) { Start-Sleep -Milliseconds 500; $gone = -not (Test-Path $dir) }
    Check $gone 'program folder deleted after the exe exited'
    Check (Test-Path (Join-Path $data 'keep-me.txt')) 'user data kept'
    $other = ($originalPath -split ';' | Where-Object { $_ }) -join ';'
    $now   = ((UserPath) -split ';' | Where-Object { $_ }) -join ';'
    Check ($now -eq $other) 'every other PATH entry is unchanged'
}
finally {
    # Whatever happened above, leave the user's PATH as it was.
    $key = Get-Item 'HKCU:\Environment'
    if ((UserPath) -ne $originalPath) {
        Set-ItemProperty -Path 'HKCU:\Environment' -Name Path -Value $originalPath -Type $originalKind
        Write-Host 'restored the original user PATH'
    }
    if ($Isolated) { Remove-Item -Recurse -Force $fake -ErrorAction SilentlyContinue }
}

if ($failures) { Write-Host "$failures check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'packaging smoke test passed' -ForegroundColor Green
exit 0
