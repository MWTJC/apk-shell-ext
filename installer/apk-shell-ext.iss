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
#define AppVersion "1.0.3"
#define AppPublisher "MWTJC"
#define AppId "{{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}"

; 按架构分包（ADR-0010）：同一个脚本编两次，靠 ISCC /DBUILD_ARM64 切换。
;   x64 包  -> 只允许真 x64 系统（x64os，不含 Arm64 的 x64 模仿）
;   arm64 包 -> 只允许 Arm64 系统
; 为什么用 x64os 而不是 x64compatible：我们的 DLL 是进程内壳扩展，Arm64 上的 explorer/dllhost
; 是原生 Arm64 进程，x64 DLL 装上去也不会生效（静默失效比"拒绝安装"更糟）。
#ifdef BUILD_ARM64
  #define ArchName "arm64"
  #define ArchIdent "arm64"
  #define PayloadDir "..\out\arm64"
#else
  #define ArchName "x64"
  #define ArchIdent "x64os"
  #define PayloadDir "..\out\x64"
#endif

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
ArchitecturesAllowed={#ArchIdent}
ArchitecturesInstallIn64BitMode={#ArchIdent}
MinVersion=10.0.17763

; 换 DLL 时 Explorer / dllhost 可能正拿着旧文件。这里**不关闭任何程序**：
; Windows 允许给"已加载的 DLL"改名（进程继续用旧映像，新进程用新文件），
; 所以 [Code] 在 ssInstall 阶段先把被占用的文件改名成 *.old，让 [Files] 正常写入新文件。
; 为什么不用 Inno 的 CloseApplications：它会用 Restart Manager 在 UI 线程关掉 Explorer，
; 实测同一台机器上这一关 0.4–30 s 不等，期间向导没有任何进度；而且 RestartApplications=no 时
; Inno 不负责把 Explorer 拉回来（实测有一次 60 s 都没回来）。改名的代价只是"已打开的窗口继续用旧代码"。
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no

; 装/卸之后让 Explorer 重新读类型图标与属性页（SHChangeNotify）
ChangesAssociations=yes

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\out\dist
OutputBaseFilename=apk-shell-ext-setup-{#AppVersion}-{#ArchName}
UninstallDisplayName={#AppName} (package icons + info page)
; 控制面板里的图标：必须给一个有 icon 资源的文件或 .ico。apkthumb.dll 里没有 ICON 资源，
; 指向它时 ExtractIconEx 返回 0 个图标 → 控制面板显示空白（实测）。
UninstallDisplayIcon={app}\icons\apk.ico
SetupIconFile=..\icons\apk.ico
LicenseFile=..\LICENSE
; 安装器界面只有英文：官方发行版不带中文 .isl，第三方翻译不入库（见 docs/m5-delivery.md）

[Tasks]
; 两个功能可以单独关掉（ADR-0008）：属性页是核心功能，不设开关。
; UsePreviousTasks 默认 yes → 升级时沿用上一次的选择。
Name: "thumb"; Description: "Thumbnails for .apk/.xapk/.apks/.apkm/.hap in Large and Extra large views"; GroupDescription: "Explorer integration:"
Name: "icon";  Description: "Replace the generic file icon in Small / List / Details views"; GroupDescription: "Explorer integration:"

[Files]
; restartreplace 只在"改名也没成功"时兜底（它需要管理员，按用户安装时无效）；
; 正常路径由 [Code] 的 MoveAsideForReplace 把旧文件改名让路，不关任何程序。
; uninsrestartdelete：卸载时 DLL 若正被 Explorer 拿着，就排队到重启删除，而不是报错卡住。
Source: "{#PayloadDir}\apkthumb.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\apkprop.dll";  DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\apkinfo.exe";  DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
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
; 升级时给"被占用、来不及删"的旧二进制留的副本（.*.old / .old1 …）
Type: files; Name: "{app}\apkthumb.dll.old*"
Type: files; Name: "{app}\apkprop.dll.old*"
Type: files; Name: "{app}\apkinfo.exe.old*"

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
  // 升级换文件时"改名让路"的记录：Live[i] 被改成了 Aside[i]（见 MoveAsideForReplace）
  MovedLive, MovedAside: TStringList;
  ReplacedInPlace: Boolean;

// Windows 允许重命名一个**已被进程加载**的 DLL：旧进程继续用旧映像，新进程用新文件。
// 所以升级根本不需要关闭 Explorer：把旧文件改名让路，[Files] 就能顺利写入新的同名文件。
// 这也解释了为什么不用 Inno 的 CloseApplications（它会用 Restart Manager 关掉 shell，
// 耗时 0.4–30 s 不可预测、期间没有进度，而且我们还得自己把它拉回来）。
procedure MoveAsideForReplace(const Dir, Name: String);
var
  Live, Aside: String;
  Suffix: Integer;
begin
  Live := AddBackslash(Dir) + Name;
  if not FileExists(Live) then
    Exit;  { 首次安装，或已被上一次升级清掉 }
  Aside := Live + '.old';
  Suffix := 1;
  while FileExists(Aside) do begin
    Aside := Live + '.old' + IntToStr(Suffix);
    Inc(Suffix);
  end;
  if RenameFile(Live, Aside) then begin
    MovedLive.Add(Live);
    MovedAside.Add(Aside);
    ReplacedInPlace := True;
    Log('replace: renamed ' + Name + ' out of the way (still loaded by Explorer or dllhost)');
  end else
    Log('replace: could not rename ' + Name + '; Setup will overwrite it (may need a restart)');
end;

// 新文件写好后，旧副本通常已经没人拿着了（宿主进程退出即解锁）；删不掉就留给下次安装。
procedure CleanupMovedAside;
var
  I: Integer;
begin
  for I := 0 to MovedAside.Count - 1 do begin
    if FileExists(MovedAside[I]) then begin
      if DeleteFile(MovedAside[I]) then
        Log('replace: removed ' + ExtractFileName(MovedAside[I]))
      else
        Log('replace: ' + ExtractFileName(MovedAside[I]) + ' still in use; next install will retry');
    end;
  end;
end;

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
  MovedLive := TStringList.Create;
  MovedAside := TStringList.Create;
  ReplacedInPlace := False;
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  state: String;
begin
  if CurStep = ssInstall then begin
    AskAboutConflicts;
    // 先给被 Explorer / dllhost 拿着的旧二进制改名让路，[Files] 才能无条件覆盖。
    MoveAsideForReplace(ExpandConstant('{app}'), 'apkthumb.dll');
    MoveAsideForReplace(ExpandConstant('{app}'), 'apkprop.dll');
    MoveAsideForReplace(ExpandConstant('{app}'), 'apkinfo.exe');
  end else if CurStep = ssPostInstall then begin
    CleanupMovedAside;
    state := 'thumb=' + IntToStr(Ord(AllowThumb)) + ' icon=' + IntToStr(Ord(AllowIcon));
    Log('apk-shell-ext: install done (' + state + '); property page + DefaultIcon/handler ' +
        'registration for apk/xapk/apks/apkm/hap');
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and ReplacedInPlace then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'Files that Explorer had loaded were replaced in place, so no window had to be closed. ' +
      'The new version takes effect the next time Explorer starts.';
end;

procedure DeinitializeSetup;
var
  I: Integer;
begin
  // 中途失败或被取消时新文件可能没写成：把改名让路的旧文件放回去，别留一个坏掉的安装目录。
  if MovedAside <> nil then
    for I := 0 to MovedAside.Count - 1 do begin
      if (not FileExists(MovedLive[I])) and FileExists(MovedAside[I]) then begin
        if RenameFile(MovedAside[I], MovedLive[I]) then
          Log('replace: restored ' + ExtractFileName(MovedLive[I]));
      end;
    end;
end;
