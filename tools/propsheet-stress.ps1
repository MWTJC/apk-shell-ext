param(
    [Parameter(Mandatory = $true)][string]$ApkPath,
    [int]$Cycles = 10,
    [int]$PageWaitMs = 120,
    [string]$OutPng = 'C:\Users\TJC\AppData\Local\hermes\cache\scratch\stress.png'
)
# Opens and closes the property page N times and reports explorer's handle count
# before/after plus any leftover apkinfo.exe processes (M3 acceptance: no monotonic
# growth, no orphan workers when the page is closed mid-parse).
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI).

$script = Join-Path $PSScriptRoot 'capture-propsheet.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw "capture-propsheet.ps1 not found next to this script" }

function ExplorerHandles {
    $proc = Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $proc) { return -1 }
    return $proc.HandleCount
}

$before = ExplorerHandles
Write-Host ("explorer handles before: {0}" -f $before)

for ($i = 1; $i -le $Cycles; $i++) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $script -ApkPath $ApkPath -OutPng $OutPng -WaitMs $PageWaitMs -SkipUia *> $null
    Start-Sleep -Milliseconds 250
    if ($i % 5 -eq 0) {
        Write-Host ("  cycle {0}: handles={1} apkinfo={2}" -f $i, (ExplorerHandles),
                    @(Get-Process apkinfo -ErrorAction SilentlyContinue).Count)
    }
}

Start-Sleep -Milliseconds 800
$after = ExplorerHandles
$leftover = @(Get-Process apkinfo -ErrorAction SilentlyContinue).Count
Write-Host ("explorer handles after {0} cycles: {1} (delta {2})" -f $Cycles, $after, ($after - $before))
Write-Host ("leftover apkinfo processes: {0}" -f $leftover)
if ($leftover -ne 0) { exit 1 }
exit 0
