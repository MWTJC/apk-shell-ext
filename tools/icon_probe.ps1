# Extract what Explorer would actually draw for the DefaultIcon of each package type.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\icon_probe.ps1 -Dir <folder> -Out <folder>
param(
    [string]$Dir = ".",
    [string]$Out = "."
)

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class ShellIcon {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SHFILEINFO {
        public IntPtr hIcon;
        public int iIcon;
        public uint dwAttributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szDisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string szTypeName;
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SHGetFileInfo(string path, uint attrs, ref SHFILEINFO info,
        uint cbSize, uint flags);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyIcon(IntPtr hIcon);
}
"@

$SHGFI_ICON = 0x000000100
$SHGFI_LARGEICON = 0x000000000
$SHGFI_SMALLICON = 0x000000001

function Save-ShellIcon([string]$file, [uint32]$flags, [string]$target) {
    $info = New-Object ShellIcon+SHFILEINFO
    $res = [ShellIcon]::SHGetFileInfo($file, 0, [ref]$info, [uint32][Runtime.InteropServices.Marshal]::SizeOf($info),
                                       [uint32]($SHGFI_ICON -bor $flags))
    if ($res -eq [IntPtr]::Zero -or $info.hIcon -eq [IntPtr]::Zero) {
        Write-Output "FAIL $file"
        return
    }
    $icon = [System.Drawing.Icon]::FromHandle($info.hIcon)
    $bmp = $icon.ToBitmap()
    $bmp.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("{0} -> {1} ({2}x{3})" -f (Split-Path $file -Leaf), (Split-Path $target -Leaf), $bmp.Width, $bmp.Height)
    $bmp.Dispose(); $icon.Dispose()
    [void][ShellIcon]::DestroyIcon($info.hIcon)
}

foreach ($f in Get-ChildItem -Path $Dir -File | Where-Object { $_.Extension -in '.apk', '.apks', '.xapk', '.apkm', '.hap' } | Select-Object -First 6) {
    $stem = ($f.BaseName + $f.Extension) -replace '[^A-Za-z0-9._-]', '_'
    Save-ShellIcon $f.FullName $SHGFI_SMALLICON (Join-Path $Out "$stem-small.png")
    Save-ShellIcon $f.FullName $SHGFI_LARGEICON (Join-Path $Out "$stem-large.png")
}
