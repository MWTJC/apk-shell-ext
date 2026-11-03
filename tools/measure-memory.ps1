param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [string]$DllName = 'apkthumb.dll',
    [int]$SettleMs = 6000
)
# V4 measurement (docs/verification.md): private-commit delta caused by browsing a folder
# of APKs in the Large-icons view. Thumbnail handlers live in dllhost, so that process is
# measured separately (its module list tells us whether our DLL is the cause).

$ErrorActionPreference = 'Stop'

function Measure-State([string]$Tag) {
    $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue)
    $explorerMB = 0
    if ($explorer.Count -gt 0) {
        $explorerMB = [math]::Round((($explorer | Measure-Object PrivateMemorySize64 -Sum).Sum / 1MB), 2)
    }
    $hosts = @()
    foreach ($p in @(Get-Process dllhost -ErrorAction SilentlyContinue)) {
        $hasDll = $false
        try {
            foreach ($m in $p.Modules) { if ($m.ModuleName -eq $DllName) { $hasDll = $true } }
        } catch { }
        $hosts += [pscustomobject]@{
            pid       = $p.Id
            privateMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
            hasDll    = $hasDll
        }
    }
    $ours = @($hosts | Where-Object { $_.hasDll })
    $oursMB = 0
    if ($ours.Count -gt 0) { $oursMB = [math]::Round((($ours | Measure-Object privateMB -Sum).Sum), 2) }
    return [pscustomobject]@{
        tag              = $Tag
        explorerPrivateMB = $explorerMB
        dllhostCount     = $hosts.Count
        dllhostWithDll   = $ours.Count
        dllhostPrivateMB = $oursMB
        dllhostDetail    = ($ours | ForEach-Object { "pid=$($_.pid) ${($_.privateMB)}MB" }) -join '; '
    }
}

$before = Measure-State 'before'
Write-Host ("[v4] before  explorer={0} MB  dllhost={1} (ours={2}, {3} MB)" -f `
    $before.explorerPrivateMB, $before.dllhostCount, $before.dllhostWithDll, $before.dllhostPrivateMB)

$viewScript = Join-Path $PSScriptRoot 'explorer-view.ps1'
& $viewScript -Folder $Folder -View Large -WaitMs $SettleMs

$after = Measure-State 'after'
Write-Host ("[v4] after   explorer={0} MB  dllhost={1} (ours={2}, {3} MB)" -f `
    $after.explorerPrivateMB, $after.dllhostCount, $after.dllhostWithDll, $after.dllhostPrivateMB)

$delta = [math]::Round($after.explorerPrivateMB - $before.explorerPrivateMB, 2)
Write-Host ("[v4] delta   explorer={0} MB   dllhostWithDll={1} files(={2})" -f `
    $delta, $after.dllhostWithDll, (Get-ChildItem -LiteralPath $Folder -Filter *.apk).Count)
