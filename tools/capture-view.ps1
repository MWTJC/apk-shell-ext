param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [string]$OutPng = '',
    [ValidateSet('List', 'Small', 'Details', 'Large')][string]$View = 'List',
    [int]$WaitMs = 3000
)
# Open Explorer on $Folder in the requested view and capture just that window.
# View switching goes through Shell.Application's Document.CurrentViewMode (stable across
# Win10/Win11 layouts; SendKeys and UIA menus are both fragile).
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI).
#
# Used by the M5 acceptance record: proves the three DefaultIcon files are what Explorer
# draws in list / small-icon views (no code of ours runs there).

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
if (-not ('Win32.Capture' -as [type])) {
    Add-Type -Namespace Win32 -Name Capture -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
public static bool BringToFront(IntPtr hWnd) {
    var top = new IntPtr(-1); var notop = new IntPtr(-2);
    SetWindowPos(hWnd, top, 0, 0, 0, 0, 0x0001 | 0x0002);
    SetWindowPos(hWnd, notop, 0, 0, 0, 0, 0x0001 | 0x0002);
    SetForegroundWindow(hWnd);
    return BringWindowToTop(hWnd);
}
'@
}

# CurrentViewMode: 2 = small icons, 3 = list, 4 = details, 5 = icons/thumbnails (large)
$mode = switch ($View) {
    'Large'   { 5 }
    'List'    { 3 }
    'Details' { 4 }
    'Small'   { 2 }
}

Start-Process -FilePath explorer.exe -ArgumentList "`"$Folder`""
$leaf = Split-Path -Leaf $Folder
$proc = $null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 400
    $proc = Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$leaf*" } | Select-Object -First 1
    if ($null -ne $proc) { break }
}
if ($null -eq $proc) { throw "explorer window for '$leaf' not found" }

$hwnd = $proc.MainWindowHandle
[void][Win32.Capture]::ShowWindow($hwnd, 3)
[void][Win32.Capture]::SetForegroundWindow($hwnd)
# 置顶再取消：保证截图时没有被"主文件夹"之类的新窗口盖住
[void][Win32.Capture]::BringToFront($hwnd)
Start-Sleep -Milliseconds 900

$resolved = (Resolve-Path -LiteralPath $Folder).Path
$shell = New-Object -ComObject Shell.Application
$window = $null
foreach ($candidate in $shell.Windows()) {
    if ($null -eq $candidate.Document) { continue }
    if ($candidate.Document.Folder.Self.Path -eq $resolved) { $window = $candidate; break }
}
if ($null -eq $window) { throw "no shell window for $resolved" }
$window.Document.CurrentViewMode = $mode
Start-Sleep -Milliseconds $WaitMs
Write-Host "[view] CurrentViewMode -> $($window.Document.CurrentViewMode) ($View)"

if ([string]::IsNullOrEmpty($OutPng)) {
    $OutPng = Join-Path $env:TEMP 'apksx-explorer.png'
}
$rect = New-Object Win32.Capture+RECT
[void][Win32.Capture]::GetWindowRect($hwnd, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap $width, $height
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose()
$bmp.Dispose()
Write-Host "[capture] $OutPng ($width x $height)"
