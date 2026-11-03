param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [int]$Seconds = 14,
    [ValidateSet('Details', 'Large')][string]$View = 'Details',
    [string]$WorkDir = 'C:\Users\TJC\AppData\Local\hermes\cache\scratch\v1'
)
# V1 measurement (docs/verification.md section 1).
#
# Design: Procmon must already be capturing while the folder is rendered, otherwise the
# measurement is a null test (an empty capture always shows zero). So the sequence is:
#   1) open Explorer at the PARENT folder and let it settle
#   2) start Procmon (one UAC consent) -- it captures for $Seconds
#   3) AFTER capture starts: navigate the window to $Folder, switch the view via UI
#      Automation, refresh -- this is the "user browsed the folder" moment
#   4) stop/wait for the CSV, then filter it (tools/parse-procmon-csv.py)
#
# Procmon's CLI has no /Filter switch (only /LoadConfig with a binary .pmc), so the capture
# is unfiltered and the filtering happens on the exported CSV.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
if (-not ('Win32.ProcApi' -as [type])) {
    Add-Type -Namespace Win32 -Name ProcApi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
'@
}

$procmon = 'C:\Program Files\WindowsApps\Microsoft.SysinternalsSuite_2026.10.0.0_x64__8wekyb3d8bbwe\Tools\Procmon.exe'
if (-not (Test-Path -LiteralPath $procmon)) { throw "Procmon.exe not found at $procmon" }
if (-not (Test-Path -LiteralPath $Folder)) { throw "folder not found: $Folder" }
if (-not (Test-Path -LiteralPath $WorkDir)) { New-Item -Path $WorkDir -ItemType Directory -Force | Out-Null }
$pml = Join-Path $WorkDir 'v1.pml'
$csv = Join-Path $WorkDir 'v1.csv'
Remove-Item -LiteralPath $pml, $csv -ErrorAction SilentlyContinue
Write-Host "[v1] work=$WorkDir view=$View seconds=$Seconds"

# ---------------------------------------------------------------- labels (points, not utf8)
$u = { param([int[]]$cp) -join ($cp | ForEach-Object { [char]$_ }) }
$labelView = & $u @(0x89C6, 0x56FE)
$labelView2 = & $u @(0x67E5, 0x770B)
$labelDetails = & $u @(0x8BE6, 0x7EC6, 0x4FE1, 0x606F)
$labelLarge = & $u @(0x5927, 0x56FE, 0x6807)
$wanted = if ($View -eq 'Details') { @($labelDetails, 'Details') } else { @($labelLarge, 'Large icons') }

# ---------------------------------------------------------------- 1) parent window
$parent = Split-Path -Parent $Folder
$leaf = Split-Path -Leaf $Folder
Start-Process -FilePath explorer.exe -ArgumentList "`"$parent`"" | Out-Null
$proc = $null
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Milliseconds 400
    $proc = Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$(Split-Path -Leaf $parent)*" } | Select-Object -First 1
    if ($null -ne $proc) { break }
}
if ($null -eq $proc) {
    $proc = Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -ne '' } | Select-Object -First 1
}
if ($null -eq $proc) { throw "no Explorer window found" }
$hwnd = $proc.MainWindowHandle
[void][Win32.ProcApi]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 600
Write-Host "[v1] window ready: pid=$($proc.Id)"

# ---------------------------------------------------------------- 2) start capture
$script = Join-Path $WorkDir 'v1-capture.cmd'
Set-Content -LiteralPath $script -Encoding ASCII -Value @(
    '@echo off',
    "`"$procmon`" /AcceptEula /Quiet /Minimized /Runtime $Seconds /BackingFile `"$pml`"",
    "`"$procmon`" /OpenLog `"$pml`" /SaveAs `"$csv`""
)
Write-Host "[v1] starting Procmon (UAC prompt expected) ..."
Start-Process -FilePath 'cmd.exe' -ArgumentList "/c `"$script`"" -Verb RunAs | Out-Null
Start-Sleep -Seconds 5   # driver may need to load on the very first run

# ---------------------------------------------------------------- 3) browse DURING capture
$shell = New-Object -ComObject Shell.Application
$window = $shell.Windows() | Where-Object { $_.HWND -eq [int64]$hwnd } | Select-Object -First 1
if ($null -ne $window) {
    $window.Navigate2($Folder)
} else {
    # COM window handle mismatch: fall back to shell activation (still a real navigation)
    Start-Process -FilePath explorer.exe -ArgumentList "`"$Folder`""
}
Start-Sleep -Milliseconds 1200

# switch view through UI Automation (forces a re-render of every item)
$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
$TS = [System.Windows.Automation.TreeScope]::Descendants
$buttonType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)
$radioType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::RadioButton)
$menuType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::MenuItem)

function Find-ByName([string[]]$names, $condition) {
    foreach ($el in $root.FindAll($TS, $condition)) {
        if ($names -contains $el.Current.Name) { return $el }
    }
    return $null
}

$viewBtn = Find-ByName @($labelView, $labelView2, 'View') $buttonType
if ($null -ne $viewBtn) {
    $viewBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 600
    $item = Find-ByName $wanted $radioType
    if ($null -eq $item) { $item = Find-ByName $wanted $menuType }
    if ($null -ne $item) {
        try { $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
        catch { $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
        Write-Host "[v1] view switched to $View during capture"
    } else {
        Write-Host "[v1] WARNING: view item not found; folder was only navigated to"
    }
} else {
    Write-Host "[v1] WARNING: view button not found; folder was only navigated to"
}
Start-Sleep -Milliseconds 1500

# refresh + hover a few rows (hover is what an infotip/property handler would react to)
if ($null -ne $window) {
    $window.Refresh()
    Start-Sleep -Milliseconds 800
}
$itemType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::ListItem)
$hovered = 0
foreach ($it in @($root.FindAll($TS, $itemType) | Select-Object -First 10)) {
    $r = $it.Current.BoundingRectangle
    if ($r.Width -le 0) { continue }
    [void][Win32.ProcApi]::SetCursorPos([int]($r.Left + $r.Width / 2), [int]($r.Top + $r.Height / 2))
    Start-Sleep -Milliseconds 250
    $hovered++
}
Write-Host "[v1] hovered $hovered list items"

# ---------------------------------------------------------------- 4) wait for the CSV
for ($i = 0; $i -lt 90; $i++) {
    if (Test-Path -LiteralPath $csv) {
        try {
            $fh = [System.IO.File]::Open($csv, 'Open', 'Read', 'ReadWrite')
            $fh.Close()
            Write-Host ("[v1] csv ready ({0:N1} MB): $csv" -f ((Get-Item -LiteralPath $csv).Length / 1MB))
            exit 0
        } catch { }
    }
    Start-Sleep -Milliseconds 500
}
throw "csv never became readable: $csv"
