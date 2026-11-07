<#
.SYNOPSIS
    Snapshot / diff / check the registry and files that the installer owns,
    so "install then uninstall leaves zero residue" can be proven (V6 / M5).

.DESCRIPTION
    Snapshot mode dumps every key and value under the paths apk-shell-ext writes,
    plus the install directory listing, into a sorted text file.
    Diff mode compares two snapshots and exits 1 when they differ.
    Check mode asserts the expected state after install (or after uninstall):

      powershell -File tools\verify-install.ps1 -Phase snapshot -Scope user -Path a.txt
      powershell -File tools\verify-install.ps1 -Phase diff     -A a.txt -B b.txt
      powershell -File tools\verify-install.ps1 -Phase check    -State installed
      powershell -File tools\verify-install.ps1 -Phase check    -State removed

    Keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less files as ANSI).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\verify-install.ps1 -Phase check -State installed
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('snapshot', 'diff', 'check')][string]$Phase,
    [ValidateSet('user', 'machine')][string]$Scope = 'user',
    [string]$Path = '',
    [string]$A = '',
    [string]$B = '',
    [ValidateSet('installed', 'removed')][string]$State = 'installed',
    # Which features this installation is supposed to have switched on (ADR-0008).
    # Default = the normal, everything-on install. Pass e.g. -Features prop to assert a install
    # where thumbnails and file icons were deliberately left out.
    [ValidateSet('thumb', 'icon', 'prop')][string[]]$Features = @('thumb', 'icon', 'prop')
)

$ErrorActionPreference = 'Stop'

$ThumbClsid = '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}'
$PropClsid = '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}'
$ThumbHandlerId = '{E357FCCD-A995-4576-B01F-234630154E96}'
$Exts = @('apk', 'xapk', 'apks', 'apkm', 'hap')
$IconFor = @{ apk = 'apk.ico'; xapk = 'apks.ico'; apks = 'apks.ico'; apkm = 'apks.ico'; hap = 'hap.ico' }

$Classes = if ($Scope -eq 'user') { 'HKCU:\Software\Classes' } else { 'HKLM:\Software\Classes' }
$UninstallRoot = if ($Scope -eq 'user') {
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
} else {
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
}
# Inno Setup 的卸载条目名：{AppId}_is1，AppId 见 installer\apk-shell-ext.iss
$UninstallKeyName = '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}_is1'
$InstallDir = if ($Scope -eq 'user') {
    Join-Path $env:LOCALAPPDATA 'Programs\apk-shell-ext'
} else {
    Join-Path $env:ProgramFiles 'apk-shell-ext'
}

function OwnedRegistryPaths {
    $paths = @()
    foreach ($ext in $Exts) {
        $paths += Join-Path $Classes ('.' + $ext + '\ShellEx\' + $ThumbHandlerId)
        $paths += Join-Path $Classes ('.' + $ext + '\shellex\PropertySheetHandlers\ApkInfo')
        $paths += Join-Path $Classes ('.' + $ext + '\DefaultIcon')
        $paths += Join-Path $Classes ('CLSID\' + $ThumbClsid)
        $paths += Join-Path $Classes ('CLSID\' + $PropClsid)
    }
    $paths += Join-Path $UninstallRoot $UninstallKeyName
    return $paths | Sort-Object -Unique
}

function GlobalRegistrations {
    # Anything of ours hung under HKCR\* or AllFilesystemObjects would attach the property page to
    # every file type (only per-extension registration is allowed: ADR-0002). Report, never repair
    # here - this script only asserts.
    $hits = @()
    foreach ($target in @('*', 'AllFilesystemObjects')) {
        foreach ($group in @('shellex\PropertySheetHandlers', ('ShellEx\' + $ThumbHandlerId))) {
            $root = Join-Path $Classes ($target + '\' + $group)
            if (-not (Test-Path -LiteralPath $root)) { continue }
            foreach ($child in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
                $value = $child.GetValue('', $null)
                if ($value -eq $PropClsid -or $value -eq $ThumbClsid) {
                    $hits += (Join-Path $root $child.PSChildName)
                }
            }
        }
    }
    return $hits
}

function Read-Tree([string]$root) {
    $lines = @()
    if (-not (Test-Path -LiteralPath $root)) {
        $lines += "$root  ABSENT"
        return $lines
    }
    $key = Get-Item -LiteralPath $root
    $values = $key.GetValueNames() | Sort-Object
    foreach ($name in $values) {
        $label = if ([string]::IsNullOrEmpty($name)) { '(default)' } else { $name }
        $lines += ("{0}  [{1}] = {2}" -f $root, $label, $key.GetValue($name, '<unreadable>', 'DoNotExpandEnvironmentNames'))
    }
    foreach ($child in @(Get-ChildItem -LiteralPath $root -Recurse -ErrorAction SilentlyContinue)) {
        $sub = $child.GetValueNames() | Sort-Object
        if ($sub.Count -eq 0 -and $child.PSChildName -eq '') { continue }
        foreach ($name in $sub) {
            $label = if ([string]::IsNullOrEmpty($name)) { '(default)' } else { $name }
            $lines += ("{0}\{1}  [{2}] = {3}" -f $root, $child.PSChildName, $label,
                       $child.GetValue($name, '<unreadable>', 'DoNotExpandEnvironmentNames'))
        }
    }
    return $lines
}

function Take-Snapshot {
    $lines = @()
    foreach ($path in (OwnedRegistryPaths)) {
        $lines += Read-Tree $path
    }
    if (Test-Path -LiteralPath $InstallDir) {
        foreach ($file in @(Get-ChildItem -LiteralPath $InstallDir -Recurse -File | Sort-Object FullName)) {
            $lines += ('FILE  ' + $file.FullName + '  ' + $file.Length)
        }
    } else {
        $lines += "DIR  $InstallDir ABSENT"
    }
    return $lines | Sort-Object
}

switch ($Phase) {
    'snapshot' {
        if ([string]::IsNullOrEmpty($Path)) { $Path = Join-Path $env:TEMP 'apksx-snapshot.txt' }
        $lines = Take-Snapshot
        $lines | Set-Content -LiteralPath $Path -Encoding UTF8
        Write-Host "[snapshot] $($lines.Count) lines -> $Path"
    }
    'diff' {
        if ([string]::IsNullOrEmpty($A) -or [string]::IsNullOrEmpty($B)) { throw 'diff needs -A and -B' }
        $left = Get-Content -LiteralPath $A -Encoding UTF8
        $right = Get-Content -LiteralPath $B -Encoding UTF8
        $diff = Compare-Object -ReferenceObject $left -DifferenceObject $right
        if ($null -eq $diff) {
            Write-Host '[diff] identical (zero residue)'
            exit 0
        }
        Write-Host "[diff] $($diff.Count) differing lines:"
        foreach ($row in $diff) {
            $sign = if ($row.SideIndicator -eq '<=') { '-' } else { '+' }
            Write-Host ("  {0} {1}" -f $sign, $row.InputObject)
        }
        exit 1
    }
    'check' {
        $problems = @()
        $thumb = Join-Path $Classes ('CLSID\' + $ThumbClsid + '\InprocServer32')
        $prop = Join-Path $Classes ('CLSID\' + $PropClsid + '\InprocServer32')

        if ($State -eq 'installed') {
            foreach ($file in @('apkthumb.dll', 'apkprop.dll', 'apkinfo.exe', 'unins000.exe')) {
                if (-not (Test-Path -LiteralPath (Join-Path $InstallDir $file))) {
                    $problems += "missing file: $(Join-Path $InstallDir $file)"
                }
            }
            foreach ($ext in $Exts) {
                $key = Join-Path $Classes ('.' + $ext + '\ShellEx\' + $ThumbHandlerId)
                $value = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).'(default)'
                if ($Features -contains 'thumb') {
                    if ($value -ne $ThumbClsid) { $problems += ".$ext thumbnail handler missing" }
                } elseif ($value -eq $ThumbClsid) {
                    $problems += ".$ext thumbnail handler registered although thumb was not requested"
                }
                $propKey = Join-Path $Classes ('.' + $ext + '\shellex\PropertySheetHandlers\ApkInfo')
                $value = (Get-ItemProperty -LiteralPath $propKey -ErrorAction SilentlyContinue).'(default)'
                if ($Features -contains 'prop') {
                    if ($value -ne $PropClsid) { $problems += ".$ext property page handler missing" }
                } elseif ($value -eq $PropClsid) {
                    $problems += ".$ext property page handler registered although prop was not requested"
                }
                $iconKey = Join-Path $Classes ('.' + $ext + '\DefaultIcon')
                $icon = (Get-ItemProperty -LiteralPath $iconKey -ErrorAction SilentlyContinue).'(default)'
                $want = '"' + (Join-Path (Join-Path $InstallDir 'icons') $IconFor[$ext]) + '",0'
                if ($Features -contains 'icon') {
                    if ($icon -ne $want) { $problems += ".$ext DefaultIcon = '$icon' (want '$want')" }
                } elseif ($icon -eq $want) {
                    $problems += ".$ext DefaultIcon still points at us although icon was not requested"
                }
            }
            $server = (Get-ItemProperty -LiteralPath $thumb -ErrorAction SilentlyContinue).'(default)'
            if ($server -ne (Join-Path $InstallDir 'apkthumb.dll')) { $problems += "thumb InprocServer32 = '$server'" }
            $server = (Get-ItemProperty -LiteralPath $prop -ErrorAction SilentlyContinue).'(default)'
            if ($server -ne (Join-Path $InstallDir 'apkprop.dll')) { $problems += "prop InprocServer32 = '$server'" }
            if (-not (Test-Path -LiteralPath (Join-Path $UninstallRoot $UninstallKeyName))) {
                $problems += 'uninstall entry missing (Apps & Features)'
            }
        } else {
            foreach ($path in (OwnedRegistryPaths)) {
                if (Test-Path -LiteralPath $path) { $problems += "residue key: $path" }
            }
            if (Test-Path -LiteralPath $InstallDir) {
                $left = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue)
                if ($left.Count -gt 0) { $problems += "residue files: $($left.Count) in $InstallDir" }
            }
        }

        # Applies to both states: we never own a wildcard / AllFilesystemObjects registration.
        foreach ($hit in (GlobalRegistrations)) {
            $problems += "global registration: $hit (would put the property page on every file)"
        }

        if ($problems.Count -eq 0) {
            Write-Host "[check] $State : ok"
            exit 0
        }
        Write-Host "[check] $State : $($problems.Count) problem(s)"
        foreach ($problem in $problems) { Write-Host "  - $problem" }
        exit 1
    }
}
