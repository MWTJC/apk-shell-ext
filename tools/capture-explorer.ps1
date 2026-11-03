param(
    [string]$OutPng = 'C:\Users\TJC\AppData\Local\hermes\cache\scratch\explorer_thumbnails.png',
    [string]$TitleFilter = '',
    [int]$ViewWaitMs = 4500
)
# Switches the focused Explorer window to "Large icons" via UI Automation,
# waits for thumbnails to render, then captures that window only.
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI);
# the Chinese UI labels are built from code points.

Add-Type -AssemblyName System.Windows.Forms, System.Drawing, UIAutomationClient, UIAutomationTypes
if (-not ('Win32.CaptureApi' -as [type])) {
    Add-Type -Namespace Win32 -Name CaptureApi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
}

$u = { param([int[]]$cp) -join ($cp | ForEach-Object { [char]$_ }) }
$labelView = & $u @(0x89C6, 0x56FE)          # shi tu   = "view"
$labelView2 = & $u @(0x67E5, 0x770B)         # cha kan  = "view" (Win10 wording)
$labelLarge = & $u @(0x5927, 0x56FE, 0x6807) # da tu biao = "large icons"

$proc = Get-Process explorer -ErrorAction SilentlyContinue | Where-Object {
    $_.MainWindowTitle -ne '' -and ($TitleFilter -eq '' -or $_.MainWindowTitle -like "*$TitleFilter*")
} | Select-Object -First 1
if ($null -eq $proc) { throw "no Explorer window found" }
$hwnd = $proc.MainWindowHandle
[void][Win32.CaptureApi]::ShowWindow($hwnd, 3)
[void][Win32.CaptureApi]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 700

$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
$buttonType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)

function Find-ByName([string[]]$names, $condition) {
    $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    foreach ($el in $all) {
        if ($names -contains $el.Current.Name) { return $el }
    }
    return $null
}

$viewBtn = Find-ByName @($labelView, $labelView2, 'View') $buttonType
if ($null -eq $viewBtn) {
    $names = @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonType) |
        ForEach-Object { $_.Current.Name }) -join ' | '
    Write-Host "[dbg] buttons: $names"
    throw "view button not found"
}
Write-Host "[uia] view button = $($viewBtn.Current.Name)"
$viewBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Milliseconds 800

$radioType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::RadioButton)
$menuType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::MenuItem)

$large = Find-ByName @($labelLarge, 'Large icons') $radioType
if ($null -eq $large) { $large = Find-ByName @($labelLarge, 'Large icons') $menuType }
if ($null -eq $large) { throw "large-icons item not found" }
Write-Host "[uia] large icons item = $($large.Current.Name)"
try {
    $large.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
} catch {
    try { $large.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    catch { Write-Host "could not activate: $_" }
}

Start-Sleep -Milliseconds $ViewWaitMs   # thumbnails render (first pass may hit dllhost)

$r = New-Object Win32.CaptureApi+RECT
$rectOk = [Win32.CaptureApi]::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if (-not $rectOk -or $w -le 0 -or $h -le 0) {
    # MainWindowHandle 对 shell 拥有的窗口可能是 0/失效；窗口已最大化，退回整屏截图
    Write-Host "[capture] window rect unusable (${w}x${h}), capturing the full screen instead"
    $r.Left = 0; $r.Top = 0
    $w = [System.Windows.Forms.SystemInformation]::PrimaryMonitorSize.Width
    $h = [System.Windows.Forms.SystemInformation]::PrimaryMonitorSize.Height
}
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "captured ${w}x${h} -> $OutPng"
