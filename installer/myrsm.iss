; MyRsm installer (Inno Setup 6).
;
; Build (CI does this, see .github/workflows/build.yml):
;   ISCC /DAppVersion=1.2.3 /DSourceDir=<deploy folder> /DIconFile=<icon.ico> ^
;        /DVCRedist=<vc_redist.x64.exe> /DVCRedistMinor=44 /DVCRedistBld=35208 installer\myrsm.iss
;
; Silent install / upgrade (all parameters optional):
;   MyRsm-Setup-<ver>.exe /VERYSILENT /SUPPRESSMSGBOXES /HTTPPORT=34567 /WSPORT=34568 /POLLMS=250 ^
;        /HOSTLABEL=<name> /PREFIX=hwinfo /COMPATSOURCE=0 /FIREWALL=1 /FWSCOPE=<prometheus-ip>
; Silent uninstall (keeps %ProgramData%\MyRsm unless /PURGEDATA):
;   "C:\Program Files\MyRsm\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES [/PURGEDATA]
;
; The service itself is managed by `myrsm.exe service install|start|stop|uninstall`
; and configured by %ProgramData%\MyRsm\myrsm.ini; this script stays thin.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\deploy"
#endif
#ifndef VCRedistMinor
  #define VCRedistMinor "0"
#endif
#ifndef VCRedistBld
  #define VCRedistBld "0"
#endif

#define AppName "MyRsm"
#define AppPublisher "DJMalachite"
#define AppURL "https://github.com/DJMalachite/myrsm"
#define FirewallRuleName "MyRsm HTTP"

[Setup]
; Stable AppId: upgrades install in place. Never change it.
AppId={{6F0D3B0E-5C1E-4C55-9C2B-8E4A1D7C9A61}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} setup
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=MyRsm-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\bin\myrsm.exe
; We stop/start the service ourselves around the file copy.
CloseApplications=no
RestartApplications=no
SetupLogging=yes
#ifdef IconFile
SetupIconFile={#IconFile}
#endif

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef VCRedist
Source: "{#VCRedist}"; Flags: dontcopy
#endif

[Code]
const
  ServiceKey = 'SYSTEM\CurrentControlSet\Services\MyRsm';
  VCRuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  IniSection = 'myrsm';
  DefaultHttpPort = '34567';
  DefaultWsPort = '34568';
  DefaultPollMs = '250';
  DefaultPrefix = 'hwinfo';
  DefaultFwScope = 'LocalSubnet';

var
  SettingsPage: TInputQueryWizardPage;
  OptionsPage: TInputOptionWizardPage;
  OldServicePath: String;   // binary of an existing MyRsm service outside {app}
  OldServiceArgs: String;   // its command-line arguments (pre-ini installs)
  HealthMessage: String;
  InstallProblems: String;

{ ---------------------------------------------------------------- helpers }

function IniPath: String;
begin
  Result := ExpandConstant('{commonappdata}\MyRsm\myrsm.ini');
end;

function AppExe: String;
begin
  Result := ExpandConstant('{app}\bin\myrsm.exe');
end;

{ /NAME=value from the setup command line; Default if absent. }
function GetParam(const Name, Default: String): String;
var
  I: Integer;
  P, Prefix: String;
begin
  Result := Default;
  Prefix := '/' + Uppercase(Name) + '=';
  for I := 1 to ParamCount do
  begin
    P := ParamStr(I);
    if Uppercase(Copy(P, 1, Length(Prefix))) = Prefix then
      Result := Copy(P, Length(Prefix) + 1, Length(P));
  end;
end;

function HasFlag(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if Uppercase(ParamStr(I)) = '/' + Uppercase(Name) then
      Result := True;
end;

function OnlyChars(const S, Allowed: String): Boolean;
var
  I: Integer;
begin
  Result := True;
  for I := 1 to Length(S) do
    if Pos(S[I], Allowed) = 0 then
    begin
      Result := False;
      Exit;
    end;
end;

function IsTrue(const S: String): Boolean;
var
  V: String;
begin
  V := Lowercase(Trim(S));
  Result := (V = '1') or (V = 'true') or (V = 'yes') or (V = 'on');
end;

function BoolStr(B: Boolean; const T, F: String): String;
begin
  if B then Result := T else Result := F;
end;

{ Splits a service ImagePath into the executable and the rest. Handles
  quoted paths and unquoted paths with spaces (up to ".exe"). }
procedure SplitImagePath(const ImagePath: String; var Exe, Args: String);
var
  S: String;
  P: Integer;
begin
  S := Trim(ImagePath);
  Exe := '';
  Args := '';
  if Copy(S, 1, 1) = '"' then
  begin
    Delete(S, 1, 1);
    P := Pos('"', S);
    if P = 0 then
    begin
      Exe := S;
      Exit;
    end;
    Exe := Copy(S, 1, P - 1);
    Args := Trim(Copy(S, P + 1, Length(S)));
  end
  else
  begin
    P := Pos('.exe', Lowercase(S));
    if P = 0 then
    begin
      Exe := S;
      Exit;
    end;
    Exe := Copy(S, 1, P + 3);
    Args := Trim(Copy(S, P + 4, Length(S)));
  end;
end;

{ Value following Flag in a space-separated argument string (quotes allowed). }
function ArgValue(const Args, Flag: String): String;
var
  S, Tok: String;
  Q: Integer;
  Found: Boolean;
begin
  Result := '';
  Found := False;
  S := Trim(Args);
  while S <> '' do
  begin
    if Copy(S, 1, 1) = '"' then
    begin
      Delete(S, 1, 1);
      Q := Pos('"', S);
      if Q = 0 then Q := Length(S) + 1;
      Tok := Copy(S, 1, Q - 1);
      Delete(S, 1, Q);
    end
    else
    begin
      Q := Pos(' ', S);
      if Q = 0 then Q := Length(S) + 1;
      Tok := Copy(S, 1, Q - 1);
      Delete(S, 1, Q - 1);
    end;
    S := TrimLeft(S);
    if Found then
    begin
      Result := Tok;
      Exit;
    end;
    if CompareText(Tok, Flag) = 0 then
      Found := True
    else if CompareText(Copy(Tok, 1, Length(Flag) + 1), Flag + '=') = 0 then
    begin
      Result := Copy(Tok, Length(Flag) + 2, Length(Tok));
      Exit;
    end;
  end;
end;

function ArgPresent(const Args, Flag: String): Boolean;
begin
  Result := Pos(' ' + Lowercase(Flag) + ' ', ' ' + Lowercase(Args) + ' ') > 0;
end;

{ Command-line parameter > existing ini > old service argument > default. }
function InitialValue(const Param, IniKey, OldFlag, Default: String): String;
begin
  Result := GetIniString(IniSection, IniKey, '', IniPath);
  if (Result = '') and (OldFlag <> '') then
    Result := ArgValue(OldServiceArgs, OldFlag);
  if Result = '' then
    Result := Default;
  Result := GetParam(Param, Result);
end;

{ Runs myrsm.exe with the given arguments; its output goes to the setup log. }
function RunMyRsm(const Exe, Args: String; var Code: Integer): Boolean;
var
  OutFile: String;
  Output: AnsiString;
begin
  OutFile := ExpandConstant('{tmp}\myrsm-' + IntToStr(Random(100000)) + '.txt');
  Result := Exec(ExpandConstant('{cmd}'),
    '/S /C ""' + Exe + '" ' + Args + ' > "' + OutFile + '" 2>&1"',
    ExtractFileDir(Exe), SW_HIDE, ewWaitUntilTerminated, Code);
  if LoadStringFromFile(OutFile, Output) then
    Log('myrsm.exe ' + Args + ' (exit ' + IntToStr(Code) + '): ' + Output)
  else
    Log('myrsm.exe ' + Args + ' (exit ' + IntToStr(Code) + ')');
  DeleteFile(OutFile);
end;

function RunNetsh(const Args: String): Integer;
var
  Code: Integer;
begin
  if not Exec(ExpandConstant('{sys}\netsh.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Log('netsh ' + Args + ' -> ' + IntToStr(Code));
  Result := Code;
end;

procedure AddProblem(const S: String);
begin
  Log('PROBLEM: ' + S);
  if InstallProblems <> '' then
    InstallProblems := InstallProblems + #13#10;
  InstallProblems := InstallProblems + S;
end;

{ ---------------------------------------------------------------- wizard }

procedure InitializeWizard;
var
  ImagePath, Exe: String;
begin
  OldServicePath := '';
  OldServiceArgs := '';
  if RegQueryStringValue(HKLM, ServiceKey, 'ImagePath', ImagePath) then
  begin
    SplitImagePath(ImagePath, Exe, OldServiceArgs);
    OldServicePath := Exe;
    Log('Existing MyRsm service: ' + ImagePath);
  end;

  SettingsPage := CreateInputQueryPage(wpSelectDir,
    'MyRsm settings',
    'These are written to ' + IniPath + '.',
    'You can change them later by editing that file and running "Restart-Service MyRsm".');
  SettingsPage.Add('HTTP port (/, /metrics, /healthz):', False);
  SettingsPage.Add('WebSocket port:', False);
  SettingsPage.Add('Shared-memory poll interval (ms, minimum 50):', False);
  SettingsPage.Add('Host label for Prometheus (empty = computer name):', False);
  SettingsPage.Add('Metric prefix:', False);
  SettingsPage.Add('Firewall rule: allowed remote addresses (e.g. LocalSubnet or the Prometheus IP):', False);

  SettingsPage.Values[0] := InitialValue('HTTPPORT', 'http_port', '--httpport', DefaultHttpPort);
  SettingsPage.Values[1] := InitialValue('WSPORT', 'ws_port', '--wsport', DefaultWsPort);
  SettingsPage.Values[2] := InitialValue('POLLMS', 'poll_ms', '--pollms', DefaultPollMs);
  SettingsPage.Values[3] := InitialValue('HOSTLABEL', 'host_label', '--host-label', ExpandConstant('{computername}'));
  SettingsPage.Values[4] := InitialValue('PREFIX', 'metric_prefix', '--metric-prefix', DefaultPrefix);
  SettingsPage.Values[5] := GetParam('FWSCOPE', GetPreviousData('FirewallScope', DefaultFwScope));

  OptionsPage := CreateInputOptionPage(SettingsPage.ID,
    'MyRsm options', '', 'Select the options to apply:', False, False);
  OptionsPage.Add('Add a Windows Firewall rule allowing inbound TCP to the HTTP port');
  OptionsPage.Add('Add source="http" to sensor series (only if your dashboards came from the post-MQTT connector)');
  OptionsPage.Values[0] := IsTrue(GetParam('FIREWALL', GetPreviousData('Firewall', '1')));
  OptionsPage.Values[1] := IsTrue(GetParam('COMPATSOURCE',
    BoolStr(IsTrue(GetIniString(IniSection, 'compat_source_label', '', IniPath))
            or ArgPresent(OldServiceArgs, '--compat-source-label'), '1', '0')));
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Firewall', BoolStr(OptionsPage.Values[0], '1', '0'));
  SetPreviousData(PreviousDataKey, 'FirewallScope', Trim(SettingsPage.Values[5]));
end;

{ Returns '' when the settings are valid, else a message. }
function ValidateSettings: String;
var
  Http, Ws, Poll: Integer;
  Host, Prefix, Scope: String;
begin
  Result := '';
  Http := StrToIntDef(Trim(SettingsPage.Values[0]), -1);
  Ws := StrToIntDef(Trim(SettingsPage.Values[1]), -1);
  Poll := StrToIntDef(Trim(SettingsPage.Values[2]), -1);
  Host := Trim(SettingsPage.Values[3]);
  Prefix := Trim(SettingsPage.Values[4]);
  Scope := Trim(SettingsPage.Values[5]);
  if (Http < 1) or (Http > 65535) then
    Result := 'The HTTP port must be a number from 1 to 65535.'
  else if (Ws < 1) or (Ws > 65535) then
    Result := 'The WebSocket port must be a number from 1 to 65535.'
  else if Http = Ws then
    Result := 'The HTTP and WebSocket ports must be different.'
  else if (Poll < 50) or (Poll > 600000) then
    Result := 'The poll interval must be a number of milliseconds from 50 to 600000.'
  else if (Length(Host) > 128) or
          not OnlyChars(Host, 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._- ') then
    Result := 'The host label may only contain letters, digits, spaces, ".", "_" and "-" (edit myrsm.ini for anything else).'
  else if (Prefix = '') or
          not OnlyChars(Prefix, 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_') or
          (Pos(Copy(Prefix, 1, 1), '0123456789') > 0) then
    Result := 'The metric prefix must start with a letter or "_" and contain only letters, digits and "_".'
  else if OptionsPage.Values[0] and
          ((Scope = '') or not OnlyChars(Scope, 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,:/-')) then
    Result := 'The firewall remote address list must be like LocalSubnet, 192.168.1.10, 10.0.0.0/24 (comma-separated, no spaces).';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Msg: String;
begin
  Result := True;
  if (CurPageID = SettingsPage.ID) or (CurPageID = OptionsPage.ID) then
  begin
    Msg := ValidateSettings;
    if Msg <> '' then
    begin
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

{ ---------------------------------------------------------------- install }

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  ImagePath, Exe, Args: String;
begin
  Result := ValidateSettings;   { also covers silent installs }
  if Result <> '' then
    Exit;

  if not RegQueryStringValue(HKLM, ServiceKey, 'ImagePath', ImagePath) then
    Exit;
  SplitImagePath(ImagePath, Exe, Args);
  if CompareText(Exe, AppExe) = 0 then
  begin
    { Upgrade/reinstall in place: stop the service so its files aren't locked. }
    OldServicePath := '';
    if FileExists(AppExe) then
    begin
      if not RunMyRsm(AppExe, 'service stop', Code) or (Code <> 0) then
        Result := 'Could not stop the MyRsm service (myrsm.exe service stop exited ' + IntToStr(Code) +
                  '). Stop it with "Stop-Service MyRsm" and run setup again.';
    end
    else
    begin
      Exec(ExpandConstant('{sys}\sc.exe'), 'stop MyRsm', '', SW_HIDE, ewWaitUntilTerminated, Code);
      Sleep(5000);
    end;
  end;
  { A service pointing elsewhere (e.g. the old per-user IFW install) doesn't
    lock our files; "myrsm.exe service install" stops and repoints it below. }
end;

function VCRuntimeIsCurrent: Boolean;
var
  Installed, Minor, Bld: Cardinal;
begin
  Result := False;
  if not RegQueryDWordValue(HKLM64, VCRuntimeKey, 'Installed', Installed) or (Installed <> 1) then
    Exit;
  if not RegQueryDWordValue(HKLM64, VCRuntimeKey, 'Minor', Minor) then
    Exit;
  if not RegQueryDWordValue(HKLM64, VCRuntimeKey, 'Bld', Bld) then
    Exit;
  Result := (Minor > {#VCRedistMinor}) or ((Minor = {#VCRedistMinor}) and (Bld >= {#VCRedistBld}));
end;

procedure InstallVCRuntime;
var
  Code: Integer;
begin
#ifdef VCRedist
  if VCRuntimeIsCurrent then
  begin
    Log('VC++ runtime is current.');
    Exit;
  end;
  WizardForm.StatusLabel.Caption := 'Installing the Microsoft Visual C++ runtime...';
  ExtractTemporaryFile('{#ExtractFileName(VCRedist)}');
  if not Exec(ExpandConstant('{tmp}\{#ExtractFileName(VCRedist)}'), '/install /quiet /norestart', '',
              SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Log('vc_redist exit code ' + IntToStr(Code));
  { 0 ok, 3010 ok (reboot pending), 1638 a newer version is installed }
  if (Code <> 0) and (Code <> 3010) and (Code <> 1638) then
    AddProblem('The Visual C++ runtime installer failed (exit code ' + IntToStr(Code) + ').');
#else
  Log('No VC++ runtime bundled.');
#endif
end;

procedure WriteIni;
var
  Dir, Path: String;
begin
  Path := IniPath;
  Dir := ExtractFileDir(Path);
  ForceDirectories(Dir);
  if not FileExists(Path) then
    SaveStringToFile(Path,
      '; MyRsm settings. Edit, then run: Restart-Service MyRsm' + #13#10 +
      '; Other keys: bind_address, freshness_timeout, include_sensors, exclude_sensors,' + #13#10 +
      ';   include_classes, exclude_classes, include_apps, exclude_apps (see README).' + #13#10 +
      '[myrsm]' + #13#10, False);
  { Only the installer's keys are (re)written; anything else is kept. }
  SetIniString(IniSection, 'http_port', Trim(SettingsPage.Values[0]), Path);
  SetIniString(IniSection, 'ws_port', Trim(SettingsPage.Values[1]), Path);
  SetIniString(IniSection, 'poll_ms', Trim(SettingsPage.Values[2]), Path);
  SetIniString(IniSection, 'host_label', Trim(SettingsPage.Values[3]), Path);
  SetIniString(IniSection, 'metric_prefix', Trim(SettingsPage.Values[4]), Path);
  SetIniString(IniSection, 'compat_source_label', BoolStr(OptionsPage.Values[1], 'true', 'false'), Path);
  Log('Wrote ' + Path);
end;

procedure ConfigureFirewall;
var
  Code: Integer;
begin
  RunNetsh('advfirewall firewall delete rule name="{#FirewallRuleName}"');
  if not OptionsPage.Values[0] then
    Exit;
  Code := RunNetsh('advfirewall firewall add rule name="{#FirewallRuleName}" dir=in action=allow protocol=TCP' +
                   ' localport=' + Trim(SettingsPage.Values[0]) +
                   ' remoteip=' + Trim(SettingsPage.Values[5]) +
                   ' program="' + AppExe + '" enable=yes profile=any');
  if Code <> 0 then
    AddProblem('Could not add the Windows Firewall rule (netsh exit code ' + IntToStr(Code) + ').');
end;

{ Polls /healthz for up to 10 s and builds the finish-page message. }
procedure CheckHealth;
var
  Req: Variant;
  Host, Url, Body, Bind: String;
  I, Status: Integer;
  Answered: Boolean;
begin
  Bind := Trim(GetIniString(IniSection, 'bind_address', '', IniPath));
  if (Bind = '') or (CompareText(Bind, 'any') = 0) or (Bind = '0.0.0.0') or (Bind = '::') then
    Host := '127.0.0.1'
  else if Pos(':', Bind) > 0 then
    Host := '[' + Bind + ']'
  else
    Host := Bind;
  Url := 'http://' + Host + ':' + Trim(SettingsPage.Values[0]) + '/healthz';
  Answered := False;
  Status := 0;
  Body := '';
  for I := 1 to 20 do
  begin
    try
      Req := CreateOleObject('WinHttp.WinHttpRequest.5.1');
      Req.SetTimeouts(1000, 1000, 1000, 1000);
      Req.Open('GET', Url, False);
      Req.Send('');
      Status := Req.Status;
      Body := Req.ResponseText;
      Answered := True;
    except
      Answered := False;
    end;
    if Answered then
      Break;
    Sleep(500);
  end;
  Log('Health check ' + Url + ': answered=' + BoolStr(Answered, 'yes', 'no') +
      ' status=' + IntToStr(Status) + ' body=' + Body);

  if not Answered then
    HealthMessage := 'MyRsm did not answer on ' + Url + ' within 10 seconds. Check "myrsm.exe service status"' +
                     ' and the log in ' + ExpandConstant('{commonappdata}') + '\MyRsm\logs.'
  else if Pos('"exporter_up": true', Body) > 0 then
    HealthMessage := 'MyRsm is running and exporting HWiNFO data: http://' + Host + ':' +
                     Trim(SettingsPage.Values[0]) + '/metrics'
  else if Pos('"source_up": true', Body) = 0 then
    HealthMessage := 'MyRsm is running but sees no HWiNFO data. Enable Shared Memory Support in HWiNFO Settings' +
                     ' (on HWiNFO free it switches off every 12 hours) and keep the Sensors window running.'
  else
    HealthMessage := 'MyRsm is running and connected to HWiNFO; waiting for a fresh sample.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  InstallProblems := '';

  InstallVCRuntime;
  WriteIni;

  WizardForm.StatusLabel.Caption := 'Registering the MyRsm service...';
  if not RunMyRsm(AppExe, 'service install', Code) or (Code <> 0) then
    AddProblem('"myrsm.exe service install" failed (exit code ' + IntToStr(Code) + ').');

  WizardForm.StatusLabel.Caption := 'Configuring Windows Firewall...';
  ConfigureFirewall;

  WizardForm.StatusLabel.Caption := 'Starting the MyRsm service...';
  if not RunMyRsm(AppExe, 'service start', Code) or (Code <> 0) then
    AddProblem('"myrsm.exe service start" failed (exit code ' + IntToStr(Code) + ').');

  WizardForm.StatusLabel.Caption := 'Checking that MyRsm answers...';
  CheckHealth;

  if InstallProblems <> '' then
    { Note: no line may start with '#' (ISPP would read it as a directive). }
    SuppressibleMsgBox('Setup finished with problems:' + #13#10#13#10 + InstallProblems + #13#10#13#10 +
                       'Details are in the setup log.', mbError, MB_OK, IDOK);

  if OldServicePath <> '' then
    SuppressibleMsgBox('The MyRsm service used to run from:' + #13#10 + OldServicePath + #13#10#13#10 +
      'It now runs from ' + AppExe + '. If that was the old per-user MyRsm install, remove it from' +
      ' Settings > Apps. If its uninstaller removes the MyRsm service, run this setup again to repair it.',
      mbInformation, MB_OK, IDOK);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (HealthMessage <> '') then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + HealthMessage;
end;

{ ---------------------------------------------------------------- uninstall }

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
  DataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    if FileExists(AppExe) then
    begin
      if not RunMyRsm(AppExe, 'service uninstall', Code) or (Code <> 0) then
        SuppressibleMsgBox('"myrsm.exe service uninstall" failed (exit code ' + IntToStr(Code) +
          '). Remove the service with "sc.exe delete MyRsm".', mbError, MB_OK, IDOK);
    end;
    RunNetsh('advfirewall firewall delete rule name="{#FirewallRuleName}"');
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{commonappdata}\MyRsm');
    if DirExists(DataDir) then
    begin
      if UninstallSilent then
      begin
        if HasFlag('PURGEDATA') then
          DelTree(DataDir, True, True, True);
      end
      else if MsgBox('Also delete the MyRsm settings and logs in ' + DataDir + '?',
                     mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
