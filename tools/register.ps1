<#
.SYNOPSIS
    Register / unregister the APK thumbnail provider (M1: thumbnails only).

.DESCRIPTION
    Writes exactly the keys described in docs/architecture.md section 4 and nothing else:
      HKCR\.<ext>\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96} = <ThumbCLSID>
      HKCR\CLSID\{ThumbCLSID}\InprocServer32                     = apkthumb.dll, Apartment
    Extensions default to apk + the multi-APK containers (xapk/apks/apkm, M4a).
    Pass -Extensions 'apk' to register only the plain extension.
    A JSON backup of every value we touch is written to
    %LOCALAPPDATA%\apk-shell-ext\install-backup.json so -Uninstall can restore the
    previous state exactly (V6: registry diff must be empty).

    Scope CurrentUser needs no admin rights (writes HKCU\Software\Classes, which Explorer
    honours). Scope LocalMachine needs an elevated shell (writes HKLM\Software\Classes).

    NOTE: keep this file ASCII-only. Windows PowerShell 5.1 decodes BOM-less files as ANSI,
    and non-ASCII comments can corrupt the following lines (seen in practice).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install
    powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Uninstall
#>
[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(ParameterSetName = 'Install')][switch]$Install,
    [Parameter(ParameterSetName = 'Uninstall')][switch]$Uninstall,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [ValidateSet('All', 'Thumb', 'Propsheet')][string]$Feature = 'All',
    [string[]]$Extensions = @('apk', 'xapk', 'apks', 'apkm'),
    [string]$DllDir = ''
)

$ErrorActionPreference = 'Stop'

$ThumbClsid = '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}'
$ThumbHandlerId = '{E357FCCD-A995-4576-B01F-234630154E96}'
$PropClsid = '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}'
$PropHandlerName = 'ApkInfo'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Install -and -not $Uninstall) { $Install = $true }
if ([string]::IsNullOrEmpty($DllDir)) { $DllDir = Join-Path $RepoRoot 'out\x64' }
$DllPath = Join-Path $DllDir 'apkthumb.dll'
$PropDllPath = Join-Path $DllDir 'apkprop.dll'

$ClassesRoot = if ($Scope -eq 'CurrentUser') { 'HKCU:\Software\Classes' } else { 'HKLM:\Software\Classes' }
$BackupDir = Join-Path $env:LOCALAPPDATA 'apk-shell-ext'
$BackupFile = Join-Path $BackupDir 'install-backup.json'

function Ensure-Key([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
}

function Get-ValueOrDefault([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path
    if ($null -eq $item) { return $null }
    return $item.GetValue($Name, $null)
}

function Read-BackupRecords {
    $records = @()
    if (-not (Test-Path -LiteralPath $BackupFile)) { return $records }
    $raw = Get-Content -LiteralPath $BackupFile -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($raw)) { return $records }
    $parsed = $raw | ConvertFrom-Json
    foreach ($p in @($parsed)) {
        foreach ($q in @($p)) {
            if ($null -ne $q -and $q.path -is [string] -and -not [string]::IsNullOrEmpty($q.path)) {
                $records += $q
            }
        }
    }
    return $records
}

function Save-Backup([hashtable]$Record) {
    if (-not (Test-Path -LiteralPath $BackupDir)) {
        New-Item -Path $BackupDir -ItemType Directory -Force | Out-Null
    }
    $records = @()
    foreach ($r in (Read-BackupRecords)) { $records += $r }
    $records += [pscustomobject]@{
        scope      = $Scope
        keyExisted = [bool]$Record.keyExisted
        path       = [string]$Record.path
        name       = [string]$Record.name
        oldValue   = $Record.oldValue
        newValue   = $Record.newValue
        stamp      = (Get-Date).ToString('o')
    }
    ConvertTo-Json -InputObject @($records) -Depth 6 | Set-Content -LiteralPath $BackupFile -Encoding UTF8
}

function Install-ThumbHandler {
    if (-not (Test-Path -LiteralPath $DllPath)) {
        throw "apkthumb.dll not found at $DllPath (run tools\build.cmd first, or pass -DllDir)"
    }
    $dllFull = (Resolve-Path -LiteralPath $DllPath).Path

    # 1) shell extension registration, per extension
    foreach ($ext in $Extensions) {
        $handlerKey = Join-Path $ClassesRoot ('.' + $ext + '\ShellEx\' + $ThumbHandlerId)

        $keyExisted = Test-Path -LiteralPath $handlerKey
        $oldValue = Get-ValueOrDefault $handlerKey ''
        Save-Backup @{ keyExisted = $keyExisted; path = $handlerKey; name = ''; oldValue = $oldValue; newValue = $ThumbClsid }

        Ensure-Key $handlerKey
        Set-ItemProperty -LiteralPath $handlerKey -Name '(default)' -Value $ThumbClsid
    }

    # 2) COM class registration
    $clsidKey = Join-Path $ClassesRoot ('CLSID\' + $ThumbClsid)
    $inprocKey = Join-Path $clsidKey 'InprocServer32'
    $keyExisted = Test-Path -LiteralPath $inprocKey
    $oldValue = Get-ValueOrDefault $inprocKey ''
    Save-Backup @{ keyExisted = $keyExisted; path = $inprocKey; name = ''; oldValue = $oldValue; newValue = $dllFull }

    Ensure-Key $clsidKey
    Set-ItemProperty -LiteralPath $clsidKey -Name '(default)' -Value 'APK Thumbnail Provider (apk-shell-ext)'
    Ensure-Key $inprocKey
    Set-ItemProperty -LiteralPath $inprocKey -Name '(default)' -Value $dllFull
    Set-ItemProperty -LiteralPath $inprocKey -Name 'ThreadingModel' -Value 'Apartment'

    Write-Host "[install] scope=$Scope"
    Write-Host "[install] $clsidKey -> $dllFull"
    foreach ($ext in $Extensions) {
        Write-Host "[install] .$ext ShellEx\$ThumbHandlerId -> $ThumbClsid"
    }
    Write-Host "[install] thumbnails registered. Restart Explorer (or clear the thumbnail cache) to see changes."
}

function Uninstall-Handler {
    # Remove only what we wrote; restore previous values recorded in the backup.
    foreach ($rec in (Read-BackupRecords)) {
        if ($rec.scope -ne $Scope) { continue }
        if ($rec.keyExisted -eq $true -and $null -ne $rec.oldValue) {
            if (Test-Path -LiteralPath $rec.path) {
                Set-ItemProperty -LiteralPath $rec.path -Name $rec.name -Value $rec.oldValue
                Write-Host "[uninstall] restored $($rec.path) -> $($rec.oldValue)"
            }
        } else {
            if (Test-Path -LiteralPath $rec.path) {
                Remove-Item -LiteralPath $rec.path -Recurse -Force
                Write-Host "[uninstall] removed $($rec.path)"
            }
        }
    }

    # Belt and braces: remove our own keys even when no backup exists.
    foreach ($ext in $Extensions) {
        $handlerKey = Join-Path $ClassesRoot ('.' + $ext + '\ShellEx\' + $ThumbHandlerId)
        if (Test-Path -LiteralPath $handlerKey) {
            Remove-Item -LiteralPath $handlerKey -Recurse -Force
            Write-Host "[uninstall] removed $handlerKey"
        }
        # Drop the parent only when it is an empty shell we created (V6: zero residue).
        $shellExKey = Join-Path $ClassesRoot ('.' + $ext + '\ShellEx')
        if (Test-Path -LiteralPath $shellExKey) {
            $kids = @(Get-ChildItem -LiteralPath $shellExKey -ErrorAction SilentlyContinue)
            if ($kids.Count -eq 0) {
                Remove-Item -LiteralPath $shellExKey -Force
                Write-Host "[uninstall] removed empty $shellExKey"
            }
        }
    }

    $clsidKey = Join-Path $ClassesRoot ('CLSID\' + $ThumbClsid)
    if (Test-Path -LiteralPath $clsidKey) {
        Remove-Item -LiteralPath $clsidKey -Recurse -Force
        Write-Host "[uninstall] removed $clsidKey"
    }

    # Property sheet keys (belt and braces; the backup restore above usually did it already)
    if ($Feature -ne 'Thumb') {
        foreach ($ext in $Extensions) {
            $propKey = Join-Path $ClassesRoot ('.' + $ext + '\shellex\PropertySheetHandlers\' + $PropHandlerName)
            if (Test-Path -LiteralPath $propKey) {
                Remove-Item -LiteralPath $propKey -Recurse -Force
                Write-Host "[uninstall] removed $propKey"
            }
            $handlersKey = Join-Path $ClassesRoot ('.' + $ext + '\shellex\PropertySheetHandlers')
            if (Test-Path -LiteralPath $handlersKey) {
                $left = @(Get-ChildItem -LiteralPath $handlersKey -ErrorAction SilentlyContinue)
                if ($left.Count -eq 0) { Remove-Item -LiteralPath $handlersKey -Force }
            }
            $shellExKey = Join-Path $ClassesRoot ('.' + $ext + '\shellex')
            if (Test-Path -LiteralPath $shellExKey) {
                $left = @(Get-ChildItem -LiteralPath $shellExKey -ErrorAction SilentlyContinue)
                if ($left.Count -eq 0) { Remove-Item -LiteralPath $shellExKey -Force }
            }
        }
        $propClsidKey = Join-Path $ClassesRoot ('CLSID\' + $PropClsid)
        if (Test-Path -LiteralPath $propClsidKey) {
            Remove-Item -LiteralPath $propClsidKey -Recurse -Force
            Write-Host "[uninstall] removed $propClsidKey"
        }
    }

    if (Test-Path -LiteralPath $BackupFile) { Remove-Item -LiteralPath $BackupFile -Force }
    Write-Host "[uninstall] done. Restart Explorer to drop the handler."
}

function Install-PropsheetHandler {
    if (-not (Test-Path -LiteralPath $PropDllPath)) {
        throw "apkprop.dll not found at $PropDllPath (run tools\build.cmd first, or pass -DllDir)"
    }
    $dllFull = (Resolve-Path -LiteralPath $PropDllPath).Path

    foreach ($ext in $Extensions) {
        $handlerKey = Join-Path $ClassesRoot ('.' + $ext + '\shellex\PropertySheetHandlers\' + $PropHandlerName)
        $keyExisted = Test-Path -LiteralPath $handlerKey
        $oldValue = Get-ValueOrDefault $handlerKey ''
        Save-Backup @{ keyExisted = $keyExisted; path = $handlerKey; name = ''; oldValue = $oldValue; newValue = $PropClsid }
        Ensure-Key $handlerKey
        Set-ItemProperty -LiteralPath $handlerKey -Name '(default)' -Value $PropClsid
    }

    $clsidKey = Join-Path $ClassesRoot ('CLSID\' + $PropClsid)
    $inprocKey = Join-Path $clsidKey 'InprocServer32'
    $keyExisted = Test-Path -LiteralPath $inprocKey
    $oldValue = Get-ValueOrDefault $inprocKey ''
    Save-Backup @{ keyExisted = $keyExisted; path = $inprocKey; name = ''; oldValue = $oldValue; newValue = $dllFull }

    Ensure-Key $clsidKey
    Set-ItemProperty -LiteralPath $clsidKey -Name '(default)' -Value 'APK Package Info Page (apk-shell-ext)'
    Ensure-Key $inprocKey
    Set-ItemProperty -LiteralPath $inprocKey -Name '(default)' -Value $dllFull
    Set-ItemProperty -LiteralPath $inprocKey -Name 'ThreadingModel' -Value 'Apartment'

    Write-Host "[install] scope=$Scope"
    Write-Host "[install] $clsidKey -> $dllFull"
    foreach ($ext in $Extensions) {
        Write-Host "[install] .$ext shellex\PropertySheetHandlers\$PropHandlerName -> $PropClsid"
    }
}

if ($Uninstall) {
    Uninstall-Handler
} else {
    if ($Feature -ne 'Propsheet') { Install-ThumbHandler }
    if ($Feature -ne 'Thumb') { Install-PropsheetHandler }
}
