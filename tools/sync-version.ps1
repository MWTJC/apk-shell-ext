# Keep the Inno Setup script's AppVersion in sync with the single source of truth (src/version.h).
#
#   powershell -ExecutionPolicy Bypass -File tools\sync-version.ps1          # write the iss
#   powershell -ExecutionPolicy Bypass -File tools\sync-version.ps1 -Check   # verify only, exit 1 on drift
#
# The version lives in exactly one place: src/version.h (APKSX_VERSION_MAJOR/MINOR/PATCH/BUILD).
# Everything else is derived: VERSIONINFO in the three .rc files (compile time), the installer's
# AppVersion (this script), and the setup file name (installer\apk-shell-ext.iss uses {#AppVersion}).
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI).
[CmdletBinding()]
param(
    [string]$VersionHeader = '',
    [string]$Iss = '',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($VersionHeader)) { $VersionHeader = Join-Path $root 'src\version.h' }
if ([string]::IsNullOrEmpty($Iss)) { $Iss = Join-Path $root 'installer\apk-shell-ext.iss' }

foreach ($path in @($VersionHeader, $Iss)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing file: $path" }
}

$header = [System.IO.File]::ReadAllText($VersionHeader)

function Get-Number([string]$name) {
    $m = [regex]::Match($header, "#define\s+$name\s+(\d+)")
    if (-not $m.Success) { throw "cannot find $name in $VersionHeader" }
    return [int]$m.Groups[1].Value
}

function Get-Quoted([string]$name) {
    $m = [regex]::Match($header, "#define\s+$name\s+`"([^`"]*)`"")
    if (-not $m.Success) { throw "cannot find $name in $VersionHeader" }
    return $m.Groups[1].Value
}

$major = Get-Number 'APKSX_VERSION_MAJOR'
$minor = Get-Number 'APKSX_VERSION_MINOR'
$patch = Get-Number 'APKSX_VERSION_PATCH'
$build = Get-Number 'APKSX_VERSION_BUILD'
$triple = '{0}.{1}.{2}' -f $major, $minor, $patch
$quad = '{0}.{1}.{2}.{3}' -f $major, $minor, $patch, $build

# version.h must agree with itself before we copy anything out of it
$declaredTriple = Get-Quoted 'APKSX_VERSION_STRING'
$declaredQuad = Get-Quoted 'APKSX_VERSION_FULL'
if ($declaredTriple -ne $triple) {
    throw "src/version.h is inconsistent: MAJOR/MINOR/PATCH say $triple but APKSX_VERSION_STRING says $declaredTriple"
}
if ($declaredQuad -ne $quad) {
    throw "src/version.h is inconsistent: APKSX_VERSION_FULL says $declaredQuad, expected $quad"
}

$issText = [System.IO.File]::ReadAllText($Iss)
$pattern = '#define\s+AppVersion\s+"[^"]*"'
$match = [regex]::Match($issText, $pattern)
if (-not $match.Success) { throw "cannot find the AppVersion define in $Iss" }
$current = [regex]::Match($match.Value, '"([^"]*)"').Groups[1].Value
$replacement = '#define AppVersion "' + $triple + '"'

if ($Check) {
    if ($current -ne $triple) {
        Write-Host ("[version] DRIFT: src/version.h = {0}, installer\apk-shell-ext.iss = {1}" -f $triple, $current)
        Write-Host '[version] run: powershell -ExecutionPolicy Bypass -File tools\sync-version.ps1'
        exit 1
    }
    Write-Host ("[version] ok: {0}" -f $triple)
    exit 0
}

if ($current -eq $triple) {
    Write-Host ("[version] {0} (unchanged)" -f $triple)
    exit 0
}

$updated = $issText.Substring(0, $match.Index) + $replacement + $issText.Substring($match.Index + $match.Length)
$utf8 = New-Object System.Text.UTF8Encoding($false)   # no BOM: Inno reads the script as UTF-8
[System.IO.File]::WriteAllText($Iss, $updated, $utf8)
Write-Host ("[version] {0} -> {1} (installer\apk-shell-ext.iss updated)" -f $current, $triple)
exit 0
