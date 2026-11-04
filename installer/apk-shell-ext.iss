; apk-shell-ext —— Inno Setup 6 安装脚本（M5）
;
; 为什么用 Inno 而不是自己写安装器（ADR-0007）：
;   * 注册表/文件/卸载条目/回滚都是声明式的，卸载由 unins000.exe 精确撤销，不需要我们维护 journal；
;   * 免 UAC 的按用户安装、一次 UAC 的整机安装、静默安装全都现成；
;   * 我们只保留"装好之后还想查/改"的命令：apkinfo.exe --status / --diag / --clear-cache / --cache-info。
;
; Root 一律用 HKA（官方文档明确"不要用 HKCR，改用 HKA + Software\Classes"）：
;   整机安装 -> HKLM\Software\Classes，按用户安装 -> HKCU\Software\Classes。
;   写成 HKCR 时非管理员模式会直接写 HKLM 并被拒绝（实测 RegCreateKeyEx 代码 5）。
;
; 构建：tools\pack.cmd （先 tools\build.cmd Release，再 ISCC，产物 out\dist\apk-shell-ext-setup-<ver>.exe）
; 装：  out\dist\apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
; 卸：  <install dir>\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

#define AppName "apk-shell-ext"
#define AppVersion "1.0.0"
#define AppPublisher "apk-shell-ext"
#define AppId "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=apk-shell-ext setup (package icons + info page)
VersionInfoProductName={#AppName}
VersionInfoCompany={#AppPublisher}

; 默认按用户安装（%LOCALAPPDATA%\Programs，零 UAC）。安装界面里可以切到"为所有用户安装"
; （{autopf} = %ProgramFiles% + HKLM），只有那一档需要一次 UAC。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
DefaultDirName={autopf}\apk-shell-ext
DisableProgramGroupPage=yes
DisableWelcomePage=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763

; 换 DLL 时 Explorer / dllhost 可能正拿着旧文件：交给 Inno 走 Restart Manager
CloseApplications=yes
RestartApplications=no
RestartIfNeededByRun=no

; 装/卸之后让 Explorer 重新读类型图标与属性页（SHChangeNotify）
ChangesAssociations=yes

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\out\dist
OutputBaseFilename=apk-shell-ext-setup-{#AppVersion}
UninstallDisplayName={#AppName} (package icons + info page)
UninstallDisplayIcon={app}\apkthumb.dll
SetupIconFile=..\icons\apk.ico
LicenseFile=..\LICENSE
; 安装器界面只有英文：官方发行版不带中文 .isl，第三方翻译不入库（见 docs/m5-delivery.md）

[Files]
Source: "..\out\x64\apkthumb.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\out\x64\apkprop.dll";  DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\out\x64\apkinfo.exe";  DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\icons\*.ico";          DestDir: "{app}\icons"; Flags: ignoreversion
Source: "..\docs\install.md";      DestDir: "{app}"; Flags: ignoreversion

[Registry]
; ---- COM 类（缩略图 provider / 属性页）
Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; ValueType: string; \
    ValueName: ""; ValueData: "APK Thumbnail Provider (apk-shell-ext)"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}\InprocServer32"; ValueType: string; \
    ValueName: ""; ValueData: "{app}\apkthumb.dll"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}\InprocServer32"; ValueType: string; \
    ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletevalue

Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; ValueType: string; \
    ValueName: ""; ValueData: "APK Package Info Page (apk-shell-ext)"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}\InprocServer32"; ValueType: string; \
    ValueName: ""; ValueData: "{app}\apkprop.dll"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\CLSID\{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}\InprocServer32"; ValueType: string; \
    ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletevalue

; ---- 缩略图 handler（每个扩展名一个 ShellEx 键）
Root: HKA; Subkey: "Software\Classes\.apk\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.xapk\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apks\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apkm\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.hap\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty

; ---- 属性页 handler
Root: HKA; Subkey: "Software\Classes\.apk\shellex\PropertySheetHandlers\ApkInfo"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.xapk\shellex\PropertySheetHandlers\ApkInfo"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apks\shellex\PropertySheetHandlers\ApkInfo"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apkm\shellex\PropertySheetHandlers\ApkInfo"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.hap\shellex\PropertySheetHandlers\ApkInfo"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty

; ---- 静态格式图标（小图标 / 列表视图，零调用成本）
Root: HKA; Subkey: "Software\Classes\.apk\DefaultIcon";  ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apk.ico"",0";  Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.xapk\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apks\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.apkm\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.hap\DefaultIcon";  ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\hap.ico"",0";  Flags: uninsdeletevalue uninsdeletekeyifempty

; ---- 界面语言（apkinfo --lang 写；默认 auto = 跟随系统）
Root: HKCU; Subkey: "Software\apk-shell-ext"; ValueType: string; ValueName: "UiLanguage"; \
    ValueData: "auto"; Flags: uninsdeletevalue uninsdeletekeyifempty

[UninstallDelete]
; 自建缩略图缓存与诊断日志：纯派生数据，删掉不丢任何用户内容
Type: filesandordirs; Name: "{localappdata}\apk-shell-ext\thumbs"
Type: files; Name: "{localappdata}\apk-shell-ext\apkthumb.log"

[Code]
// 安装完成时在安装日志里留一行可核对的痕迹（验收时对照 verify-install.ps1 的输出）
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    Log('apk-shell-ext: thumbnails + property page + DefaultIcon registered for apk/xapk/apks/apkm/hap');
end;
