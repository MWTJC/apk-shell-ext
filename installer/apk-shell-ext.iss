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

; 换 DLL 时 Explorer / dllhost 可能正拿着旧文件：交给 Inno 走 Restart Manager。
; 必须是 force：缩略图一取过，apkthumb.dll 就被 COM Surrogate（dllhost.exe）占着，
; RestartManager 关不掉它 → 默认 yes 时静默升级会以"某些应用程序无法关闭"收场（退出码 5，实测）。
; 被强关的只有 explorer / dllhost 这两个宿主进程，都是随取随起、不承载用户数据。
CloseApplications=force
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

[Tasks]
; 两个功能可以单独关掉（ADR-0008）：属性页是核心功能，不设开关。
; UsePreviousTasks 默认 yes → 升级时沿用上一次的选择。
Name: "thumb"; Description: "Thumbnails for .apk/.xapk/.apks/.apkm/.hap in Large and Extra large views"; GroupDescription: "Explorer integration:"
Name: "icon";  Description: "Replace the generic file icon in Small / List / Details views"; GroupDescription: "Explorer integration:"

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
; Tasks: thumb  = 安装界面里可以关掉这一项（ADR-0008）
; Check: AllowThumb = 关掉或探测到"别人已经占了缩略图"时整套跳过（跳过就不写、卸载也不碰）
Root: HKA; Subkey: "Software\Classes\.apk\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: thumb; Check: AllowThumb
Root: HKA; Subkey: "Software\Classes\.xapk\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: thumb; Check: AllowThumb
Root: HKA; Subkey: "Software\Classes\.apks\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: thumb; Check: AllowThumb
Root: HKA; Subkey: "Software\Classes\.apkm\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: thumb; Check: AllowThumb
Root: HKA; Subkey: "Software\Classes\.hap\ShellEx\{{E357FCCD-A995-4576-B01F-234630154E96}"; ValueType: string; \
    ValueName: ""; ValueData: "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}"; \
    Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: thumb; Check: AllowThumb

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
; Tasks: icon / Check: AllowIcon —— 与缩略图同规则（ADR-0008）
Root: HKA; Subkey: "Software\Classes\.apk\DefaultIcon";  ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apk.ico"",0";  Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: icon; Check: AllowIcon
Root: HKA; Subkey: "Software\Classes\.xapk\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: icon; Check: AllowIcon
Root: HKA; Subkey: "Software\Classes\.apks\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: icon; Check: AllowIcon
Root: HKA; Subkey: "Software\Classes\.apkm\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\apks.ico"",0"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: icon; Check: AllowIcon
Root: HKA; Subkey: "Software\Classes\.hap\DefaultIcon";  ValueType: string; ValueName: ""; \
    ValueData: """{app}\icons\hap.ico"",0";  Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: icon; Check: AllowIcon

; ---- 界面语言（apkinfo --lang 写；默认 auto = 跟随系统）
; createvalueifdoesntexist：升级时别把用户自己选的 zh/en 顶回 auto（实测过：不加这个标志，
; 每次升级都会重置成 auto）
Root: HKCU; Subkey: "Software\apk-shell-ext"; ValueType: string; ValueName: "UiLanguage"; \
    ValueData: "auto"; Flags: createvalueifdoesntexist uninsdeletevalue uninsdeletekeyifempty

[UninstallRun]
; 卸载前先把"功能开关"关掉：装的时候关掉、后来用 apkinfo --features 又打开的那些值，
; 声明式条目并不认识（它们只在安装时被写过的才会回滚），所以这里补一刀。
; 只删"指向我们自己"的值，别人的值不动（与 --features 同一份实现）。
Filename: "{app}\apkinfo.exe"; Parameters: "--features all=off"; Flags: runhidden; \
    RunOnceId: "releaseFeatures"

[UninstallDelete]
; 自建缩略图缓存与诊断日志：纯派生数据，删掉不丢任何用户内容
Type: filesandordirs; Name: "{localappdata}\apk-shell-ext\thumbs"
Type: files; Name: "{localappdata}\apk-shell-ext\apkthumb.log"

[Code]
// ---- 功能开关 + "撞车探测"（ADR-0008）-----------------------------------------------
// * 安装界面勾 thumb / icon 决定要不要接管这两块；属性页没有开关（核心功能）。
// * 真正开始写文件/写注册表之前（ssInstall）先看一眼目标机器：别的程序是不是已经占了
//   缩略图（.ext\ShellEx\{E357FCCD…} 不是我们的 CLSID）或文件图标（.ext\DefaultIcon 不是我们的 ico）。
//   占了就按功能各问一次，默认按钮 = "保持原样"。
// * 静默安装弹不了窗：默认同样"保持原样"，要接管得显式 /OVERWRITE=thumb,icon 或 /OVERWRITE=all。
// * Check: AllowThumb / AllowIcon 为假 → 这套条目压根不写；实测 Inno 对"没写过的条目"卸载时也不碰，
//   所以"保持原样"= 别人的值从安装到卸载都没人动。
var
  TakeOverThumb, TakeOverIcon: Boolean;
  ConflictsAsked: Boolean;
  Exts: array[0..4] of String;

function OurThumbClsid: String;
begin
  Result := '{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}';
end;

function ThumbHandlerKey(Ext: String): String;
begin
  Result := 'Software\Classes\.' + Ext + '\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}';
end;

function DefaultIconKey(Ext: String): String;
begin
  Result := 'Software\Classes\.' + Ext + '\DefaultIcon';
end;

function IconFileFor(Ext: String): String;
begin
  if CompareText(Ext, 'apk') = 0 then
    Result := 'apk.ico'
  else if CompareText(Ext, 'hap') = 0 then
    Result := 'hap.ico'
  else
    Result := 'apks.ico';
end;

function IsOurIconValue(Ext, Value: String): Boolean;
begin
  Result := Pos(Lowercase('\icons\' + IconFileFor(Ext)), Lowercase(Value)) > 0;
end;

// DefaultIcon 的值形如 `"C:\x\a.ico",0` → 取出路径
function IconPathOf(Value: String): String;
var
  p: Integer;
begin
  Result := Value;
  p := Pos(',', Result);
  if p > 0 then
    Result := Copy(Result, 1, p - 1);
  Result := RemoveQuotes(Trim(Result));
end;

// 别的程序留下的"死注册"不该被当成撞车：CLSID 没登记 InprocServer32、或 DLL 已经不在，
// 直接当作没占（我们覆盖它反而是帮忙）。
function ThumbRegistrationLive(Clsid: String): Boolean;
var
  dll: String;
begin
  Result := False;
  if not RegQueryStringValue(HKCR, 'CLSID\' + Clsid + '\InprocServer32', '', dll) then
    Exit;
  Result := FileExists(RemoveQuotes(Trim(dll)));
end;

function ReadBothRoots(Key: String; var Value: String): Boolean;
begin
  Value := '';
  if not RegQueryStringValue(HKCU, Key, '', Value) then
    Value := '';
  if Value = '' then
    if not RegQueryStringValue(HKLM, Key, '', Value) then
      Value := '';
  Result := Value <> '';
end;

function ForeignThumbReport: String;
var
  i: Integer;
  v: String;
begin
  Result := '';
  for i := 0 to 4 do begin
    if ReadBothRoots(ThumbHandlerKey(Exts[i]), v) and (CompareText(v, OurThumbClsid) <> 0) then begin
      if ThumbRegistrationLive(v) then
        Result := Result + '    .' + Exts[i] + '   ' + v + #13#10
      else
        Log('stale thumbnail registration ignored on .' + Exts[i] + ': ' + v);
    end;
  end;
end;

function ForeignIconReport: String;
var
  i: Integer;
  v: String;
begin
  Result := '';
  for i := 0 to 4 do begin
    if ReadBothRoots(DefaultIconKey(Exts[i]), v) and (not IsOurIconValue(Exts[i], v)) then begin
      if FileExists(IconPathOf(v)) then
        Result := Result + '    .' + Exts[i] + '   ' + v + #13#10
      else
        Log('stale DefaultIcon ignored on .' + Exts[i] + ': ' + v);
    end;
  end;
end;

procedure AskAboutConflicts;
var
  thumbReport, iconReport, overwrite: String;
begin
  if ConflictsAsked then
    Exit;
  ConflictsAsked := True;
  TakeOverThumb := True;
  TakeOverIcon := True;

  // 先算出来并写进安装日志：静默安装时这也是唯一能核对"有没有撞车"的证据
  thumbReport := ForeignThumbReport;
  iconReport := ForeignIconReport;
  if thumbReport <> '' then
    Log('conflict: thumbnails already registered by another program:' + #13#10 + thumbReport)
  else
    Log('conflict: no foreign thumbnail handler found');
  if iconReport <> '' then
    Log('conflict: file icons already set by another program:' + #13#10 + iconReport)
  else
    Log('conflict: no foreign DefaultIcon found');

  if WizardSilent then begin
    overwrite := Lowercase(ExpandConstant('{param:OVERWRITE|}'));
    // 只有在真撞车时才需要 OVERWRITE：没撞车就照常接管（否则静默安装会莫名其妙什么都不注册）
    if thumbReport <> '' then
      TakeOverThumb := (Pos('all', overwrite) > 0) or (Pos('thumb', overwrite) > 0);
    if iconReport <> '' then
      TakeOverIcon := (Pos('all', overwrite) > 0) or (Pos('icon', overwrite) > 0);
    Log('silent install: OVERWRITE=' + overwrite + ' -> thumb takeover=' +
        IntToStr(Ord(TakeOverThumb)) + ' icon takeover=' + IntToStr(Ord(TakeOverIcon)));
    Exit;
  end;

  if (thumbReport <> '') and WizardIsTaskSelected('thumb') then
    if MsgBox('Another program already provides thumbnails for these file types:'
              + #13#10 + #13#10 + thumbReport + #13#10
              + 'Let apk-shell-ext take them over?'
              + #13#10 + 'No keeps the existing provider (recommended if you are happy with it).',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDNO then
      TakeOverThumb := False;

  if (iconReport <> '') and WizardIsTaskSelected('icon') then
    if MsgBox('Another program already sets the file icon for these file types:'
              + #13#10 + #13#10 + iconReport + #13#10
              + 'Let apk-shell-ext replace it?'
              + #13#10 + 'No keeps the existing icon.',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDNO then
      TakeOverIcon := False;
end;

function AllowThumb: Boolean;
begin
  Result := WizardIsTaskSelected('thumb') and TakeOverThumb;
end;

function AllowIcon: Boolean;
begin
  Result := WizardIsTaskSelected('icon') and TakeOverIcon;
end;

function InitializeSetup: Boolean;
begin
  Exts[0] := 'apk';
  Exts[1] := 'xapk';
  Exts[2] := 'apks';
  Exts[3] := 'apkm';
  Exts[4] := 'hap';
  TakeOverThumb := True;
  TakeOverIcon := True;
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  state: String;
begin
  if CurStep = ssInstall then
    AskAboutConflicts
  else if CurStep = ssPostInstall then begin
    state := 'thumb=' + IntToStr(Ord(AllowThumb)) + ' icon=' + IntToStr(Ord(AllowIcon));
    Log('apk-shell-ext: install done (' + state + '); property page + DefaultIcon/handler ' +
        'registration for apk/xapk/apks/apkm/hap');
  end;
end;
