<#
.SYNOPSIS
  Fails if an exe imports any DLL that does not ship with Windows.

.DESCRIPTION
  soi-share.exe must run on a clean Windows install with nothing added: no
  Visual C++ redistributable, no OpenSSL, no MinGW runtime. This reads the PE
  import table and the delay-load import table directly (no dumpbin or objdump
  needed) and compares every DLL name against an allow-list of Windows' own
  system DLLs.

  A new dependency that is genuinely part of Windows goes in $Allowed below,
  deliberately. Anything else -- vcruntime140.dll, libcrypto-3-x64.dll,
  libstdc++-6.dll -- means something stopped being linked statically.

.EXAMPLE
  powershell -File tools\check-dlls.ps1 build\Release\soi-share.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Path
)
$ErrorActionPreference = 'Stop'

# Every one of these is part of Windows 10/11 itself. MFPlat is absent on "N"
# editions until the Media Feature Pack is added; the app says so at run time.
$Allowed = @(
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll',
    'ole32.dll', 'oleaut32.dll', 'ws2_32.dll', 'iphlpapi.dll', 'winhttp.dll',
    'crypt32.dll', 'bcrypt.dll', 'dwmapi.dll', 'shcore.dll', 'd3d11.dll',
    'dxgi.dll', 'd3dcompiler_47.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mf.dll',
    'ntdll.dll', 'secur32.dll', 'shlwapi.dll', 'version.dll',
    'msvcrt.dll', 'ucrtbase.dll',
    'api-ms-win-core-winrt-l1-1-0.dll', 'api-ms-win-core-winrt-string-l1-1-0.dll'
)

$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Path))
function U16([int]$o) { [BitConverter]::ToUInt16($bytes, $o) }
function U32([int]$o) { [BitConverter]::ToUInt32($bytes, $o) }

if ((U16 0) -ne 0x5A4D) { throw "$Path is not an executable (no MZ header)" }
$pe = [int](U32 0x3C)
if ((U32 $pe) -ne 0x00004550) { throw "$Path has no PE signature" }
$coff     = $pe + 4
$sections = [int](U16 ($coff + 2))
$optSize  = [int](U16 ($coff + 16))
$opt      = $coff + 20
$magic    = U16 $opt
$dirs     = if ($magic -eq 0x20B) { $opt + 112 } elseif ($magic -eq 0x10B) { $opt + 96 } else { throw "unknown optional header magic $magic" }
if ($magic -ne 0x20B) { Write-Host "note: $Path is 32-bit" }

$secTable = $opt + $optSize
function RvaToOffset([uint32]$rva) {
    for ($i = 0; $i -lt $sections; $i++) {
        $s = $secTable + 40 * $i
        $va = U32 ($s + 12); $vsize = U32 ($s + 8); $raw = U32 ($s + 20); $rawSize = U32 ($s + 16)
        $span = [Math]::Max($vsize, $rawSize)
        if ($rva -ge $va -and $rva -lt $va + $span) { return [int]($rva - $va + $raw) }
    }
    throw ("RVA 0x{0:X} is outside every section" -f $rva)
}
function AsciiAt([int]$off) {
    $end = $off
    while ($bytes[$end] -ne 0) { $end++ }
    [Text.Encoding]::ASCII.GetString($bytes, $off, $end - $off)
}

$names = New-Object System.Collections.Generic.List[string]

# IMAGE_DIRECTORY_ENTRY_IMPORT: 20-byte descriptors, name RVA at +12.
$importRva = U32 ($dirs + 8 * 1)
if ($importRva -ne 0) {
    $d = RvaToOffset $importRva
    while ($true) {
        $nameRva = U32 ($d + 12)
        if ($nameRva -eq 0 -and (U32 $d) -eq 0) { break }
        $names.Add((AsciiAt (RvaToOffset $nameRva)))
        $d += 20
    }
}

# IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT: 32-byte descriptors, name RVA at +4.
$delayRva = U32 ($dirs + 8 * 13)
if ($delayRva -ne 0) {
    $d = RvaToOffset $delayRva
    while ($true) {
        $nameRva = U32 ($d + 4)
        if ($nameRva -eq 0) { break }
        $names.Add((AsciiAt (RvaToOffset $nameRva)) + ' (delay-loaded)')
        $d += 32
    }
}

$bad = @()
foreach ($n in $names) {
    $dll = ($n -replace ' \(delay-loaded\)$', '').ToLowerInvariant()
    $ok = $Allowed -contains $dll
    if (-not $ok) { $bad += $n }
    Write-Host ("  {0,-4} {1}" -f $(if ($ok) { 'ok' } else { 'BAD' }), $n)
}
if ($names.Count -eq 0) { throw "found no imports at all in $Path; the parser is broken" }
if ($bad.Count -gt 0) {
    Write-Host ""
    Write-Host "FAIL: $Path depends on DLLs that do not ship with Windows:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host "Link them statically, or -- only if they really are part of Windows -- add them to `$Allowed in $PSCommandPath."
    exit 1
}
Write-Host "OK: $($names.Count) imports, all part of Windows."
exit 0
