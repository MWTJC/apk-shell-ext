param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [ValidateSet('Details', 'Large', 'List')][string]$View = 'Details',
    [int]$WaitMs = 2500
)
# Opens Explorer on $Folder and switches the view through UI Automation.
# ASCII-only on purpose (Windows PowerShell 5.1 decodes BOM-less files as ANSI);
# Chinese UI labels are built from code points.

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
if (-not ('Win32.ViewApi' -as [type])) {
    Add-Type -Namespace Win32 -Name ViewApi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
'@
}

$u = { param([int[]]$cp) -join ($cp | ForEach-Object { [char]$_ }) }
$labelView = & $u @(0x89C6, 0x56FE)     # "view" (Win11)
$labelView2 = & $u @(0x67E5, 0x770B)    # "view" (Win10)
$labelDetails = & $u @(0x8BE6, 0x7EC6, 0x4FE1, 0x606F)   # details
$labelLarge = & $u @(0x5927, 0x56FE, 0x6807)             # large icons
$labelList = & $u @(0x5217, 0x8868)                      # list

$wanted = switch ($View) {
    'Details' { @($labelDetails, 'Details') }
    'Large'   { @($labelLarge, 'Large icons') }
    'List'    { @($labelList, 'List') }
}

Start-Process -FilePath explorer.exe -ArgumentList "`"$Folder`""

# Wait for the window: a fresh explorer.exe can take a couple of seconds and the new
# window may be owned by a different explorer process than the one that was running.
$leaf = Split-Path -Leaf $Folder
$proc = $null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 400
    $proc = Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$leaf*" } | Select-Object -First 1
    if ($null -ne $proc) { break }
}
if ($null -eq $proc) {
    $proc = Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -ne '' } | Select-Object -First 1
}
if ($null -eq $proc) { throw "no Explorer window found" }
$hwnd = $proc.MainWindowHandle
[void][Win32.ViewApi]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 300

$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
$TS = [System.Windows.Automation.TreeScope]::Descendants
$buttonType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)

function Find-ByName([string[]]$names, $condition) {
    foreach ($el in $root.FindAll($TS, $condition)) {
        if ($names -contains $el.Current.Name) { return $el }
    }
    return $null
}

$viewBtn = Find-ByName @($labelView, $labelView2, 'View') $buttonType
if ($null -eq $viewBtn) { throw "view button not found" }
$viewBtn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Milliseconds 800

$radioType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::RadioButton)
$menuType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::MenuItem)

$item = Find-ByName $wanted $radioType
if ($null -eq $item) { $item = Find-ByName $wanted $menuType }
if ($null -eq $item) { throw "view item '$View' not found" }
try { $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
catch { $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }

Start-Sleep -Milliseconds $WaitMs
Write-Host "[view] $View applied (window pid=$($proc.Id), title=$($proc.MainWindowTitle))"
