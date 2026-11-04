param(
    [Parameter(Mandatory = $false)][string]$ApkPath = '',
    # Capture an already-open dialog with this exact title instead of invoking the
    # shell verb (used with proptest.exe, which builds the page itself).
    [string]$WindowTitle = '',
    [string]$OutPng = 'C:\Users\TJC\AppData\Local\hermes\cache\scratch\propsheet.png',
    [int]$WaitMs = 2500,
    [int]$CtrlTabPresses = 1,
    # Exact caption of the custom property page to select (e.g. 'Package info').
    # Defaults to the Chinese caption, which is what this machine's UI shows.
    [string]$TabName = '',
    # Click before capturing: "x,y" offsets from the dialog's top-left corner (real
    # mouse input, so child-window geometry does not matter).
    [string]$ClickPoint = '',
    # Scroll the page down by N wheel notches before capture.
    [int]$WheelClicks = 0,
    # Skip the UI Automation attempt (it can hang when the shell is busy).
    [switch]$SkipUia,
    # Move the cursor down the page in N steps before capture (reproduce fast hovering).
    [int]$HoverSweep = 0,
    # Wait this long after the -ClickPoint click before capturing (e.g. let SHA-256 finish).
    [int]$AfterClickWaitMs = 400,
    # Invoke a button by its caption through UI Automation (works even when the dialog
    # cannot take foreground focus, where synthetic mouse clicks land on another window).
    [string]$InvokeButton = '',
    [int]$AfterInvokeWaitMs = 1500,
    [switch]$KeepOpen
)
# Opens the "Properties" dialog for an APK via the shell verb, waits, then captures
# only that dialog window and closes it again (unless -KeepOpen).
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI);
# the localized dialog title is built from code points.

Add-Type -AssemblyName System.Windows.Forms, System.Drawing, UIAutomationClient, UIAutomationTypes
if (-not ('Win32.PropWindowApi' -as [type])) {
    Add-Type -Namespace Win32 -Name PropWindowApi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int id);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wp, IntPtr lp);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
[DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr parent, POINT pt, uint flags);
[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr hWnd, ref POINT pt);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT pt);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rc);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
[DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostMessage(IntPtr hWnd, int msg, IntPtr wp, IntPtr lp);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

public delegate bool EnumProc(IntPtr hWnd, IntPtr param);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
}

$u = { param([int[]]$cp) -join ($cp | ForEach-Object { [char]$_ }) }
$labelProps = & $u @(0x5C5E, 0x6027)  # shu xing = "properties"

if ([string]::IsNullOrEmpty($WindowTitle)) {
    if ([string]::IsNullOrEmpty($ApkPath)) { throw "either -ApkPath or -WindowTitle is required" }
    $full = (Resolve-Path -LiteralPath $ApkPath).Path
    $dir = [System.IO.Path]::GetDirectoryName($full)
    $leaf = [System.IO.Path]::GetFileName($full)

    $shell = New-Object -ComObject Shell.Application
    $item = $shell.Namespace($dir).ParseName($leaf)
    if ($null -eq $item) { throw "cannot parse $full" }
    $item.InvokeVerb('properties')
}

# Wait for the dialog (class #32770, owned by explorer, title contains the localized word)
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddMilliseconds($WaitMs + 3000)
while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 150
    $script:found = [IntPtr]::Zero
    $callback = [Win32.PropWindowApi+EnumProc]{
        param($hwnd, $param)
        if (-not [Win32.PropWindowApi]::IsWindowVisible($hwnd)) { return $true }
        $cls = New-Object System.Text.StringBuilder 64
        [void][Win32.PropWindowApi]::GetClassName($hwnd, $cls, 64)
        if ($cls.ToString() -ne '#32770') { return $true }
        $len = [Win32.PropWindowApi]::GetWindowTextLength($hwnd)
        if ($len -le 0) { return $true }
        $text = New-Object System.Text.StringBuilder ($len + 1)
        [void][Win32.PropWindowApi]::GetWindowText($hwnd, $text, $text.Capacity)
        $title = $text.ToString()
        $matches = if ([string]::IsNullOrEmpty($WindowTitle)) {
            $title.Contains($labelProps) -or $title.Contains('Properties')
        } else {
            $title -eq $WindowTitle
        }
        if ($matches) {
            $script:found = $hwnd
            return $false
        }
        return $true
    }
    [void][Win32.PropWindowApi]::EnumWindows($callback, [IntPtr]::Zero)
    if ($script:found -ne [IntPtr]::Zero) { $dialog = $script:found }
}
if ($dialog -eq [IntPtr]::Zero) { throw "properties dialog did not appear within $($WaitMs + 3000) ms" }

function Set-DialogTop([IntPtr]$h) {
    # HWND_TOPMOST(-1) | SWP_NOSIZE | SWP_NOACTIVATE: keep mouse/wheel input on the dialog
    [void][Win32.PropWindowApi]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
    [void][Win32.PropWindowApi]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 150
}

[void][Win32.PropWindowApi]::ShowWindow($dialog, 5)
Set-DialogTop $dialog
Start-Sleep -Milliseconds 250

# Select the custom tab through UI Automation (keyboard tab switching is unreliable
# because the dialog is opened by another process and may not take focus).
$labelTab = & $u @(0x5B89, 0x88C5, 0x5305, 0x4FE1, 0x606F)  # an zhuang bao xin xi
if (-not [string]::IsNullOrEmpty($TabName)) { $labelTab = $TabName }
$selected = -not [string]::IsNullOrEmpty($WindowTitle)
if ($SkipUia) { $selected = $false }
try {
    if ($selected) { throw "skip" }
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($dialog)
    $tabItemType = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::TabItem)
    $items = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $tabItemType)
    foreach ($item in $items) {
        if ($item.Current.Name -eq $labelTab) {
            $pattern = $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
            $pattern.Select()
            $selected = $true
            break
        }
    }
} catch {
    Write-Host "UIA tab selection failed: $_"
}
if (-not $selected) {
    # Fallback: drive the tab control directly (TCM_SETCURSEL + synthesized TCN_SELCHANGE).
    $tab = [Win32.PropWindowApi]::FindWindowEx($dialog, [IntPtr]::Zero, 'SysTabControl32', $null)
    if ($tab -ne [IntPtr]::Zero) {
        $count = [int][Win32.PropWindowApi]::SendMessage($tab, 0x1304, [IntPtr]::Zero, [IntPtr]::Zero)
        $target = -1
        for ($i = 0; $i -lt $count; $i++) {
            # TCITEMW on x64: mask @0, pszText @16, cchTextMax @24
            $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(1024)
            $item = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(64)
            [System.Runtime.InteropServices.Marshal]::WriteInt32($item, 0, 1)
            [System.Runtime.InteropServices.Marshal]::WriteIntPtr($item, 16, $buffer)
            [System.Runtime.InteropServices.Marshal]::WriteInt32($item, 24, 256)
            [void][Win32.PropWindowApi]::SendMessage($tab, 0x133C, [IntPtr]$i, $item)
            $text = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buffer)
            [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
            [System.Runtime.InteropServices.Marshal]::FreeHGlobal($item)
            if ($text -eq $labelTab) { $target = $i; break }
        }
        if ($target -ge 0) {
            [void][Win32.PropWindowApi]::SendMessage($tab, 0x130C, [IntPtr]$target, [IntPtr]::Zero)
            $nm = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(32)
            [System.Runtime.InteropServices.Marshal]::WriteIntPtr($nm, [IntPtr]::Zero, $tab)
            [System.Runtime.InteropServices.Marshal]::WriteIntPtr($nm, [IntPtr][IntPtr]::Size, [IntPtr][Win32.PropWindowApi]::GetDlgCtrlID($tab))
            [System.Runtime.InteropServices.Marshal]::WriteInt32($nm, ([IntPtr]::Size * 2), -551)  # TCN_SELCHANGE
            [void][Win32.PropWindowApi]::SendMessage($dialog, 0x004E, [IntPtr][Win32.PropWindowApi]::GetDlgCtrlID($tab), $nm)
            [System.Runtime.InteropServices.Marshal]::FreeHGlobal($nm)
            $selected = $true
            Write-Host "selected tab '$labelTab' via tab-control messages"
        } else {
            Write-Host "warning: tab '$labelTab' not found in tab control"
        }
    }
}
if (-not $selected) { Write-Host "warning: custom tab not found, falling back to Ctrl+Tab" }

for ($i = 0; (-not $selected) -and ($i -lt $CtrlTabPresses); $i++) {
    [Win32.PropWindowApi]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)      # Ctrl down
    [Win32.PropWindowApi]::keybd_event(0x09, 0, 0, [UIntPtr]::Zero)      # Tab down
    [Win32.PropWindowApi]::keybd_event(0x09, 0, 2, [UIntPtr]::Zero)      # Tab up
    [Win32.PropWindowApi]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)      # Ctrl up
    Start-Sleep -Milliseconds 350
}
# Let the page finish its first parse before any interaction (the old order clicked first).
# settle before interaction
Start-Sleep -Milliseconds $WaitMs

if ($WheelClicks -gt 0) {
    $windowRect2 = New-Object Win32.PropWindowApi+RECT
    [void][Win32.PropWindowApi]::GetWindowRect($dialog, [ref]$windowRect2)
    $centerX = $windowRect2.Left + [int](($windowRect2.Right - $windowRect2.Left) / 2)
    $centerY = $windowRect2.Top + [int](($windowRect2.Bottom - $windowRect2.Top) / 2)
    Set-DialogTop $dialog
    [void][Win32.PropWindowApi]::SetCursorPos($centerX, $centerY)
    Start-Sleep -Milliseconds 100
    for ($i = 0; $i -lt $WheelClicks; $i++) {
        [Win32.PropWindowApi]::mouse_event(0x0800, 0, 0, [uint32]4294967176, [UIntPtr]::Zero)  # WHEEL -120
        Start-Sleep -Milliseconds 120
    }
    Write-Host ("scrolled {0} notches" -f $WheelClicks)
    Start-Sleep -Milliseconds 300
}if (-not [string]::IsNullOrEmpty($InvokeButton)) {
    # Cross-process BM_CLICK on the page's button: UIA cannot see our page's child buttons
    # from the sheet root on this build, and synthetic mouse events land on whichever window
    # actually owns the foreground. Control ids: 1002 hash / 1003 copy / 1004 retry.
    $buttonId = switch ($InvokeButton) {
        'hash' { 1002 }
        'copy' { 1003 }
        'retry' { 1004 }
        default { throw "unknown -InvokeButton (use hash|copy|retry)" }
    }

    $script:pageDlg = [IntPtr]::Zero
    $childCallback = [Win32.PropWindowApi+EnumProc]{
        param($hwnd, $param)
        if ($script:pageDlg -ne [IntPtr]::Zero) { return $false }
        $cls = New-Object System.Text.StringBuilder 64
        [void][Win32.PropWindowApi]::GetClassName($hwnd, $cls, 64)
        if ($cls.ToString() -eq '#32770') { $script:pageDlg = $hwnd; return $false }
        return $true
    }
    [void][Win32.PropWindowApi]::EnumChildWindows($dialog, $childCallback, [IntPtr]::Zero)
    if ($script:pageDlg -eq [IntPtr]::Zero) { throw "page dialog (#32770) not found inside the property sheet" }

    $button = [Win32.PropWindowApi]::GetDlgItem($script:pageDlg, [int]$buttonId)
    if ($button -eq [IntPtr]::Zero) { throw ("button id {0} not found" -f $buttonId) }
    # WM_COMMAND straight to the page dialog: BM_CLICK on a cross-process button is not
    # delivered on this build, and the code path we want to exercise is the dialog handler.
    [void][Win32.PropWindowApi]::SendMessage($script:pageDlg, [uint32]0x0111, [IntPtr]$buttonId, [IntPtr]::Zero)
    Write-Host ("clicked button id {0}" -f $buttonId)
    Start-Sleep -Milliseconds $AfterInvokeWaitMs
}

if (-not [string]::IsNullOrEmpty($ClickPoint)) {
    $parts = $ClickPoint.Split(',')
    $windowRect = New-Object Win32.PropWindowApi+RECT
    [void][Win32.PropWindowApi]::GetWindowRect($dialog, [ref]$windowRect)
    $screenX = $windowRect.Left + [int]$parts[0]
    $screenY = $windowRect.Top + [int]$parts[1]
    Set-DialogTop $dialog
    [void][Win32.PropWindowApi]::SetCursorPos($screenX, $screenY)
    Start-Sleep -Milliseconds 120
    [Win32.PropWindowApi]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
    [Win32.PropWindowApi]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Write-Host ("clicked screen ({0},{1})" -f $screenX, $screenY)
    Start-Sleep -Milliseconds $AfterClickWaitMs
}

if ($HoverSweep -gt 0) {
    $windowRect3 = New-Object Win32.PropWindowApi+RECT
    [void][Win32.PropWindowApi]::GetWindowRect($dialog, [ref]$windowRect3)
    $left = $windowRect3.Left + 40
    $top = $windowRect3.Top + 120
    $bottom = $windowRect3.Bottom - 90
    Set-DialogTop $dialog
    for ($i = 0; $i -lt $HoverSweep; $i++) {
        $y = $top + [int](($bottom - $top) * ($i % 20) / 20)
        [void][Win32.PropWindowApi]::SetCursorPos($left, $y)
        Start-Sleep -Milliseconds 12
    }
    Write-Host ("hover sweep: {0} steps" -f $HoverSweep)
}



Start-Sleep -Milliseconds 150

$rect = New-Object Win32.PropWindowApi+RECT
[void][Win32.PropWindowApi]::GetWindowRect($dialog, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
$bitmap.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
Write-Host "captured $OutPng ($width x $height)"

if (-not $KeepOpen) {
    [void][Win32.PropWindowApi]::SendMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    Start-Sleep -Milliseconds 300
}
