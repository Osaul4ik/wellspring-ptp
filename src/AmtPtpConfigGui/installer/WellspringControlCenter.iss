; ============================================================================
;  WellspringControlCenter.iss
;  Installer for Wellspring Control Center (AmtPtpConfigGui) + optional
;  AmtPtpDeviceUsbKm driver.
;
;  What it does:
;   - At the very start (right after the Welcome page) asks what to install:
;       1) GUI + Driver (default)
;       2) GUI only
;       3) Driver only
;     Only shown when the build actually embeds a driver (see IncludeDriver
;     below) - a GUI-only build behaves exactly like before, no page shown.
;   - When the build also embeds the haptic actuator driver (see
;     IncludeHaptic below), it is installed together with the touchpad
;     driver (same Test Mode / certificate / reboot flow) and removed
;     together with it on uninstall.
;   - Installs the GUI to {autopf}\WellspringControlCenter (Program Files)
;   - Registers an entry in "Programs and Features" (Inno Setup does this
;     automatically: HKLM\...\Uninstall\{AppId}_is1)
;   - Before copying files (fresh install OR upgrade), force-closes
;     "Wellspring Control Center.exe" if it's running, so the file can be
;     replaced
;   - Start Menu shortcut for ALL users, with NO subfolder:
;     {commonprograms}\Wellspring Control Center.lnk
;   - Creates {commonappdata}\WellspringPTP (C:\ProgramData\WellspringPTP)
;     with write permissions for BUILTIN\Users - the GUI stores
;     settings.json/profiles.json there, SHARED across all users on the
;     machine
;   - Driver install (when chosen) runs in-process, right inside setup.exe,
;     NOT via a separate .bat/.vbs. Setup.exe itself already requires admin
;     (PrivilegesRequired=admin below), so it is already elevated by the
;     time it reaches this code - every Exec() call it makes (bcdedit,
;     Import-Certificate, pnputil) inherits that same admin token
;     automatically. There is no second UAC prompt, no self-relaunch, and
;     therefore nothing that can fail the way the old VBS
;     "ShellExecute ... runas" relaunch used to (it would print "Requesting
;     administrator privileges..." and then the console window would just
;     close/crash if the relaunch didn't take).
;   - The selection page also has a red "I understand what I'm doing and I
;     do NOT want my PC restarted automatically" checkbox
;     (NoAutoRebootCheckBox). Right after clicking Next on that page, a
;     warning MsgBox is shown - its text depends on the checkbox:
;       * Unchecked (default): explains that installing the driver
;         requires a restart, and that Setup will close and reboot the
;         machine automatically once the driver install finishes - no
;         "restart now / later" choice is offered later on, it just
;         happens.
;       * Checked: explains that the driver will still be installed, but
;         the PC will NOT be restarted automatically, and that the driver
;         may not be bound to the touchpad until the user manually
;         restarts Windows.
;   - After a successful driver install, if the checkbox is unchecked,
;     Setup no longer waits for the Finished page: it schedules the
;     restart itself (shutdown.exe /r /t 0 /f - immediate, no countdown)
;     and closes its own window as soon as the installation step
;     completes. If the checkbox is checked, none of that happens - no
;     shutdown is scheduled, and Setup does NOT close itself early; it
;     proceeds to the normal Finished page like any other install and
;     waits for the user to close it.
;   - On uninstall (only when the build embeds a driver - see
;     IncludeDriver): no choice is asked and no custom dialog is shown -
;     both the driver and the GUI are always removed. The driver is
;     removed first (with the Test Mode / rescan steps below), then
;     Inno's normal full uninstall proceeds and removes everything,
;     including the uninstaller itself.
;     Removing the driver also asks whether to disable Windows Test Mode
;     (testsigning); if the user says yes, it's turned off. After the
;     driver package is removed, a device rescan (pnputil /scan-devices)
;     is run so the touchpad gets rebound to a working driver right away
;     instead of being left dead until next boot.
;     A GUI-only build (no IncludeDriver) keeps the old, simpler behavior:
;     plain full removal, nothing driver-related to ask about.
;   - On any uninstall path that removes the GUI, the user is also asked
;     whether to delete settings/profiles (%ProgramData%\WellspringPTP)
;     and the autostart key HKCU\...\Run\WellspringPTP for the current
;     user.
;
;  Lives at src/AmtPtpConfigGui/installer/WellspringControlCenter.iss.
;
;  Build: Inno Setup 6.x (innosetup.org), command (from repo root):
;     ISCC.exe src\AmtPtpConfigGui\installer\WellspringControlCenter.iss /DMyAppVersion=1.0.0
;
;  Before compiling, run build-gui.ps1 (same folder) - the installer takes
;  files from publish\win-x64 relative to this same installer\ folder
;  ([Files] below; overridden via /DSourceDir from CI).
; ============================================================================

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

; Folder with the published GUI. From CI (BuildGui.yml) this is
; stage\min\WellspringPTP\Gui - the Min artifact (framework-dependent
; single file). For a local build via build-gui.ps1 - publish\win-x64.
#ifndef SourceDir
  #define SourceDir "publish\win-x64"
#endif

; Optional: folder with the built + signed driver package (.sys/.inf/.cat/
; .cer) - artifacts\driver in BuildAll.yml, passed as
; stage\min\WellspringPTP\Driver from the package-installer job there via
; /DDriverSourceDir=... . Left undefined by BuildGui.yml (GUI-only build has
; no driver to package), in which case the installer behaves exactly as
; before - GUI only, no choice page, "WellspringControlCenter-Setup-*".
;
; When it IS defined, the installer is named "WellspringPTP-Setup-*", shows
; the "what to install" page described above, and the four driver files are
; embedded (Flags: dontcopy - NOT installed under {app}), extracted to
; Setup's own {tmp} folder only for the duration of the install. Whether the
; driver is installed or skipped, Inno Setup deletes everything it
; extracted to {tmp} once Setup exits - nothing driver-related is left
; behind outside of what was actually installed into Windows.
#ifdef DriverSourceDir
  #define IncludeDriver
  #ifndef DriverName
    #define DriverName "AmtPtpDeviceUsbKm"
  #endif
  ; Optional haptic actuator driver package (.sys/.inf/.cat, signed with the
  ; same certificate as the touchpad driver, so no extra .cer): staged by
  ; BuildAll.yml as stage\min\WellspringPTP\HapticDriver and passed with
  ; /DHapticDriverSourceDir=... . Left undefined -> touchpad driver only.
  #ifdef HapticDriverSourceDir
    #define IncludeHaptic
    #ifndef HapticDriverName
      #define HapticDriverName "AmtHapticDeviceUsbKm"
    #endif
  #endif
#endif

#define MyAppName "Wellspring Control Center"
#define MyAppExeName "Wellspring Control Center.exe"
#define MyAppPublisher "WellspringPTP"
#define MyAppURL "https://github.com/Osaul4ik/wellspring-ptp"
; Fixed GUID - do NOT change it between releases, or an upgrade will stop
; being recognized as an update of the previous install.
; "{{" at the start - in Inno Setup a single "{" is always read as the
; start of a constant reference (like {app}), so to get a literal "{" in
; the final AppId value the opening brace has to be doubled; the closing
; "}" does not need escaping - it isn't a special character.
#define MyAppId "{{A6C1E4B0-6C1B-4B7E-9C0A-6E9E7C6E5D01}"

#ifdef IncludeDriver
  #define MyOutputBaseFilename "WellspringPTP-Setup-" + MyAppVersion
#else
  #define MyOutputBaseFilename "WellspringControlCenter-Setup-" + MyAppVersion
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\WellspringControlCenter
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
OutputBaseFilename={#MyOutputBaseFilename}
OutputDir=output
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Published GUI (Min - framework-dependent single file from CI, or
; publish\win-x64 for a local build via build-gui.ps1). Skipped entirely
; when the user picked "Driver only".
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: InstallGui
#ifdef IncludeDriver
; Signed driver package - embedded in setup.exe but NOT copied under {app}.
; dontcopy means these are only extracted (to {tmp}) on demand, via
; ExtractTemporaryFile in [Code]; see CurStepChanged below. DestDir is
; required syntax here but unused for dontcopy entries. Check: InstallDriver
; means these are simply never extracted when the user picked "GUI only".
Source: "{#DriverSourceDir}\{#DriverName}.sys"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.inf"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.cat"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.cer"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
#ifdef IncludeHaptic
Source: "{#HapticDriverSourceDir}\{#HapticDriverName}.sys"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#HapticDriverSourceDir}\{#HapticDriverName}.inf"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#HapticDriverSourceDir}\{#HapticDriverName}.cat"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
#endif
#endif

[Dirs]
; Shared settings/profiles folder for all users. The GUI normally runs
; without elevation, so grant BUILTIN\Users modify rights upfront -
; otherwise a non-admin user couldn't write there. uninsneveruninstall:
; deletion is handled by hand in [Code] (asked for and only on consent),
; not silently by Inno's own automation. Only needed when the GUI is
; actually being installed.
Name: "{commonappdata}\WellspringPTP"; Permissions: users-modify; Flags: uninsneveruninstall; Check: InstallGui

[Icons]
; Shortcut only, NO subfolder in Start Menu, for all users.
Name: "{commonprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Check: InstallGui

[Code]

const
  ProcessName = 'Wellspring Control Center.exe';
  SettingsDirName = 'WellspringPTP';
  RunKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValueName = 'WellspringPTP';
  LB_GETITEMHEIGHT = $01A1;
  SB_VERT = 1;

#ifdef IncludeDriver
// Used to read the CheckListBox's real, native per-item pixel height
// instead of guessing it (see the Height fix in InitializeWizard below).
function SendMessage(hWnd: Longint; Msg: Longint; WParam: Longint;
  LParam: Longint): Longint;
  external 'SendMessageA@user32.dll stdcall';
// Used to force-hide the vertical scrollbar once the box is sized to
// exactly fit its 2 items, so no leftover scroll range can appear even
// if some other DPI/theme edge case still leaves a few stray pixels.
function ShowScrollBar(hWnd: Longint; wBar: Longint; bShow: Boolean): Boolean;
  external 'ShowScrollBar@user32.dll stdcall';
#endif

var
  #ifdef IncludeDriver
  InstallChoicePage: TInputOptionWizardPage;
  NoAutoRebootCheckBox: TNewCheckBox;
  #endif
  RebootNeeded: Boolean;

// Min build - framework-dependent (no bundled CLR), so .NET 8 Desktop
// Runtime must be installed on the machine. Check the shared-runtime
// directory; if not found, only warn - don't block the install.
function IsDotNet8DesktopRuntimeInstalled(): Boolean;
var
  BaseDir: String;
  FindRec: TFindRec;
  Found: Boolean;
begin
  Found := False;
  BaseDir := ExpandConstant('{commonpf}\dotnet\shared\Microsoft.WindowsDesktop.App');
  if DirExists(BaseDir) then
  begin
    if FindFirst(BaseDir + '\8.*', FindRec) then
    begin
      Found := True;
      FindClose(FindRec);
    end;
  end;
  Result := Found;
end;

procedure WarnIfDotNet8Missing;
begin
  if not IsDotNet8DesktopRuntimeInstalled() then
  begin
    MsgBox('.NET 8 Desktop Runtime was not found.' + #13#10 +
      'This build of Wellspring Control Center requires it to run.' + #13#10 +
      'Install it from https://dotnet.microsoft.com/download/dotnet/8.0 ' +
      '("Desktop Runtime" section), then run the installer again, or ' +
      'install the runtime later - setup will continue.',
      mbInformation, MB_OK);
  end;
end;

#ifdef IncludeDriver
// "What to install" page, shown right after the Welcome page - only
// present in builds that actually embed a driver. Exclusive radio-button
// style list (True = exclusive): 0 = GUI + Driver, 1 = GUI only,
// 2 = Driver only.
procedure InitializeWizard();
begin
  InstallChoicePage := CreateInputOptionPage(wpWelcome,
    'Select Installation Type',
    'What would you like to install?',
    'Select one option, then click Next to continue.',
    True, False);
  InstallChoicePage.Add('GUI + Driver (recommended)');
  InstallChoicePage.Add('Driver only');
  InstallChoicePage.SelectedValueIndex := 0;

  // BUG FIX: CreateInputOptionPage's CheckListBox does NOT shrink-wrap to
  // its actual items - Inno sizes it to span almost the entire page
  // (Initialize sets Height := DefaultBoxBottom-relative, regardless of
  // how many options were Add()-ed). Shrink the box down to just those 2
  // items so anything anchored below it doesn't end up positioned at/past
  // the page's own bottom edge and get clipped - that was the original bug
  // with the checkbox below (it was anchored to Surface.Height directly,
  // same failure mode one level up). The box is borderless/surface-colored
  // here (BorderStyle=bsNone, Color=SurfaceColor, set in the same
  // Initialize call for a non-ListBox page), so shrinking it leaves no
  // visible artifact - just unused surface below it.
  //
  // BUG FIX 2: originally used an assumed MinItemHeight of ScaleY(22) here,
  // but that's a guess, not the control's real per-item pixel height - on
  // this box's actual font/DPI the native listbox row is a couple of
  // pixels taller, so 2 items slightly overflowed the box we requested.
  // Windows then kept a (mostly invisible, "empty") vertical scrollbar on
  // the native listbox for that leftover sliver, so scrolling revealed
  // blank space below the two real items instead of anything real. Ask the
  // control for its own real item height via LB_GETITEMHEIGHT instead of
  // guessing, then explicitly hide the vertical scrollbar - once the box
  // exactly fits its content there is nothing left to scroll.
  InstallChoicePage.CheckListBox.Height :=
    InstallChoicePage.CheckListBox.Items.Count *
    SendMessage(InstallChoicePage.CheckListBox.Handle, LB_GETITEMHEIGHT, 0, 0);
  ShowScrollBar(InstallChoicePage.CheckListBox.Handle, SB_VERT, False);

  NoAutoRebootCheckBox := TNewCheckBox.Create(WizardForm);
  NoAutoRebootCheckBox.Parent := InstallChoicePage.Surface;
  // NOTE: TNewCheckBox is a plain, unmodified TCheckBox (see NewCtrls.pas -
  // "TNewCheckBox = class(TCheckBox);", no overrides), and TControl.AutoSize
  // defaults to False, so no explicit reset is needed here. AutoSize itself
  // is NOT exposed by Inno Setup's Pascal Script class wrapper for
  // TNewCheckBox (compiling `NoAutoRebootCheckBox.AutoSize := False;` fails
  // with "Unknown identifier 'AUTOSIZE'"), so it can't be set from script
  // even if a future Inno Setup version changes the underlying default.
  // Align to the CheckListBox's own Left, not an arbitrary offset - that's
  // where the radio items above actually start, so the checkbox's glyph
  // lines up with them instead of sitting at a different, unrelated
  // indent.
  NoAutoRebootCheckBox.Left := InstallChoicePage.CheckListBox.Left;
  NoAutoRebootCheckBox.Top := InstallChoicePage.CheckListBox.Top +
    InstallChoicePage.CheckListBox.Height + ScaleY(8);
  NoAutoRebootCheckBox.Width := InstallChoicePage.CheckListBox.Width;
  NoAutoRebootCheckBox.Height := ScaleY(42);
  NoAutoRebootCheckBox.Caption := 'I understand what I''m doing and I do NOT want my PC restarted automatically';
  NoAutoRebootCheckBox.Font.Color := clRed;
end;

// Warns about the upcoming reboot the moment the user commits to an
// installation choice. Both available choices include the driver.
// on the selection page, before any files are copied - not just later,
// buried in the driver-install confirmation dialog.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = InstallChoicePage.ID) then
  begin
    if NoAutoRebootCheckBox.Checked then
      MsgBox(
        'The driver will be installed, but your PC will NOT be restarted automatically.' + #13#10#13#10 +
        'The driver may not be bound to the touchpad until you manually restart Windows.',
        mbInformation, MB_OK)
    else
      MsgBox(
        'Installing the driver requires a restart to finish.' + #13#10#13#10 +
        'Setup will restart your computer automatically after installation.',
        mbInformation, MB_OK);
  end;
end;

function InstallGui(): Boolean;
begin
  // Installation choices:
  //   0 = GUI + Driver
  //   1 = Driver only
  Result := InstallChoicePage.SelectedValueIndex = 0;
end;

function InstallDriver(): Boolean;
begin
  // Both available installation choices install the driver.
  Result := True;
end;

// Removes every published Driver Store package that originated from the INF
// named InfName (regardless of what "oemNN.inf" number Windows assigned it) -
// both from the Driver Store and from any device currently bound to it.
// Used before a (re)install, so repeated installs don't pile up oemNN.inf
// copies of the same driver, and on uninstall. ListTag only names the temp
// file the list is written to. ShowStatus must be False in the uninstaller
// (there is no WizardForm there).
//
// We deliberately do NOT parse `pnputil /enum-drivers` text output - it
// is localized (headers like "Original Name:" change with Windows'
// display language), so matching against it is unreliable on non-English
// systems. Get-WindowsDriver (DISM PowerShell module) exposes the same
// data through the OriginalFileName .NET property, which is always in
// English no matter the UI language. Without -All it only lists published
// third-party (oemNN.inf) packages - exactly where our drivers appear - so
// it doesn't enumerate thousands of inbox packages.
procedure RemovePackagesByInf(const InfName, ListTag: String; ShowStatus: Boolean);
var
  ResultCode: Integer;
  ListFile, PsCmd, Name: String;
  OldDrivers: TArrayOfString;
  I: Integer;
begin
  ListFile := ExpandConstant('{tmp}\wellspring_') + ListTag + '.txt';
  if FileExists(ListFile) then
    DeleteFile(ListFile);

  PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command ' +
    '"Get-WindowsDriver -Online | ' +
    'Where-Object { $_.OriginalFileName -like ''*' + InfName + ''' } | ' +
    'Select-Object -ExpandProperty Driver | ' +
    'Set-Content -Path ''' + ListFile + ''' -Encoding ascii"';
  // Best-effort: if listing fails, there is nothing we can safely remove.
  if (not Exec('powershell.exe', PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode))
     or (ResultCode <> 0) or (not FileExists(ListFile)) then
    Exit;

  if not LoadStringsFromFile(ListFile, OldDrivers) then
    Exit;

  for I := 0 to GetArrayLength(OldDrivers) - 1 do
  begin
    Name := Trim(OldDrivers[I]);
    if Name = '' then
      Continue;
    if ShowStatus then
      WizardForm.StatusLabel.Caption := 'Removing previous driver (' + Name + ')...';
    // /uninstall also detaches it from any device still using it;
    // /force suppresses the "in use" confirmation prompt. Ignore the
    // result - if one old package can't be removed, move on.
    Exec(ExpandConstant('{sys}\pnputil.exe'), '/delete-driver ' + Name + ' /uninstall /force',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

// Removes previous versions of the touchpad driver (and of the haptic
// actuator driver, when this build ships it) before the new ones go in.
procedure RemoveOldDriverVersions;
begin
  WizardForm.StatusLabel.Caption := 'Checking for previous driver versions...';
  RemovePackagesByInf('{#DriverName}.inf', 'old_drivers', True);
  #ifdef IncludeHaptic
  RemovePackagesByInf('{#HapticDriverName}.inf', 'old_haptic_drivers', True);
  #endif
end;

#ifdef IncludeHaptic
// Installs the haptic actuator driver (separate package, signed with the
// same certificate that was just imported for the touchpad driver). A failure
// here is NOT fatal for the touchpad driver: the user just gets no haptic
// click until it is installed.
procedure InstallHapticDriverNow;
var
  ResultCode: Integer;
  Inf: String;
begin
  Inf := ExpandConstant('{tmp}\{#HapticDriverName}.inf');
  if not FileExists(Inf) then
  begin
    MsgBox('Haptic driver .inf not found: ' + Inf + #13#10 +
      'The touchpad driver was installed, but haptic clicks will not work.',
      mbError, MB_OK);
    Exit;
  end;

  WizardForm.StatusLabel.Caption := 'Installing haptic actuator driver (pnputil)...';
  if not Exec(ExpandConstant('{sys}\pnputil.exe'), '/add-driver "' + Inf + '" /install',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    MsgBox('Could not launch pnputil.exe for the haptic driver.', mbError, MB_OK)
  else if (ResultCode <> 0) and (ResultCode <> 3010) then
    MsgBox('pnputil reported an error installing the haptic driver (code ' +
      IntToStr(ResultCode) + ').' + #13#10 +
      'The touchpad driver was installed, but haptic clicks will not work.',
      mbError, MB_OK)
  else if ResultCode = 3010 then
    RebootNeeded := True;
end;
#endif

// Runs the driver install steps directly inside this (already-elevated)
// setup.exe process - no separate script, no self-relaunch, no VBS.
// bcdedit / Import-Certificate / pnputil all just need SOME admin process
// to run them; since PrivilegesRequired=admin above already guarantees
// setup.exe itself is elevated, every Exec() call here inherits that same
// token automatically.
procedure InstallDriverNow;
var
  ResultCode: Integer;
  Inf, Cer: String;
  Ok: Boolean;
begin
  Inf := ExpandConstant('{tmp}\{#DriverName}.inf');
  Cer := ExpandConstant('{tmp}\{#DriverName}.cer');
  Ok := True;

  RemoveOldDriverVersions;

  WizardForm.StatusLabel.Caption := 'Enabling Windows Test Mode...';
  if (not Exec(ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning on', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    MsgBox('Could not enable Test Mode (bcdedit).' + #13#10 +
      'If Secure Boot is on in the BIOS/UEFI, it must be disabled first.' + #13#10 +
      'Driver installation was NOT completed.', mbError, MB_OK);
    Ok := False;
  end;

  if Ok and (not FileExists(Cer)) then
  begin
    MsgBox('Certificate not found: ' + Cer, mbError, MB_OK);
    Ok := False;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Importing certificate (Trusted Root)...';
    if (not Exec('powershell.exe',
         '-NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath ''' +
         Cer + ''' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox('Failed to import the certificate into Trusted Root.', mbError, MB_OK);
      Ok := False;
    end;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Importing certificate (Trusted Publishers)...';
    if (not Exec('powershell.exe',
         '-NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath ''' +
         Cer + ''' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox('Failed to import the certificate into Trusted Publishers.', mbError, MB_OK);
      Ok := False;
    end;
  end;

  if Ok and (not FileExists(Inf)) then
  begin
    MsgBox('Driver .inf not found: ' + Inf, mbError, MB_OK);
    Ok := False;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Installing driver (pnputil)...';
    // pnputil legitimately returns 3010 (ERROR_SUCCESS_REBOOT_REQUIRED)
    // when the driver installed fine but a reboot is needed to finish
    // binding it to the device - that is NOT a failure. Only treat the
    // Exec() call itself failing, or a genuinely unknown/error code, as
    // a real problem.
    if not Exec(ExpandConstant('{sys}\pnputil.exe'), '/add-driver "' + Inf + '" /install',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    begin
      MsgBox('Could not launch pnputil.exe.', mbError, MB_OK);
      Ok := False;
    end
    else if (ResultCode <> 0) and (ResultCode <> 3010) then
    begin
      MsgBox('pnputil reported an error installing the driver (code ' +
        IntToStr(ResultCode) + ').', mbError, MB_OK);
      Ok := False;
    end
    else if ResultCode = 3010 then
      RebootNeeded := True;
  end;

  #ifdef IncludeHaptic
  if Ok then
    InstallHapticDriverNow;
  #endif

  if Ok then
  begin
    // Best-effort nudge so PnP re-evaluates the device against the
    // freshly-installed package right away; harmless either way since a
    // reboot is required regardless (result intentionally ignored).
    Exec(ExpandConstant('{sys}\pnputil.exe'), '/scan-devices', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // No confirmation dialog here on purpose - the user was already
    // warned about the restart on the selection page. Setup closes and
    // the machine reboots automatically once CurStepChanged reaches
    // ssDone below.
    WizardForm.StatusLabel.Caption :=
      'Driver installed. Setup will close and the computer will restart shortly...';
    RebootNeeded := True;
  end;
end;

// Driver files were embedded with Flags: dontcopy, so they don't exist on
// disk until explicitly extracted. Do that right after the app itself is
// installed, then (if the user chose to install the driver) ask for
// confirmation and run the steps in-process. Inno Setup deletes everything
// it extracted to {tmp} when Setup exits, whether the driver install
// succeeds, is declined, or fails - no manual cleanup needed here.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and InstallDriver() then
  begin
    ExtractTemporaryFile('{#DriverName}.sys');
    ExtractTemporaryFile('{#DriverName}.inf');
    ExtractTemporaryFile('{#DriverName}.cat');
    ExtractTemporaryFile('{#DriverName}.cer');
    #ifdef IncludeHaptic
    ExtractTemporaryFile('{#HapticDriverName}.sys');
    ExtractTemporaryFile('{#HapticDriverName}.inf');
    ExtractTemporaryFile('{#HapticDriverName}.cat');
    #endif

    if MsgBox(
      'Install the Wellspring PTP driver now?' + #13#10#13#10 +
      'This will:' + #13#10 +
      '  1. Enable Windows Test Mode (testsigning) so the driver''s test' + #13#10 +
      '     certificate is trusted.' + #13#10 +
      '  2. Import that certificate into Trusted Root and Trusted Publishers.' + #13#10 +
      #ifdef IncludeHaptic
      '  3. Install the {#DriverName} and {#HapticDriverName} drivers (pnputil).' + #13#10 +
      #else
      '  3. Install the {#DriverName} driver (pnputil).' + #13#10 +
      #endif
      '  4. Automatically close Setup and restart the computer to finish.' + #13#10#13#10 +
      'Save any open work before continuing.',
      mbConfirmation, MB_YESNO) = IDYES then
      InstallDriverNow;
  end;

  // Installation is fully done at this point (files copied, driver step
  // already ran above) - safe to schedule the restart and close our own
  // window without Inno treating it as an aborted/incomplete Setup and
  // asking "Setup is not complete, exit anyway?". This replaces the old
  // Finished-page "restart now / later" choice entirely: it just happens.
  if (CurStep = ssDone) and RebootNeeded and
     (not NoAutoRebootCheckBox.Checked) then
  begin
    // BUG FIX: /t 15 gave Windows a 15-second countdown - and ANY nonzero
    // /t value makes Windows show its own "your PC will restart" warning
    // notification, regardless of anything Setup itself does. That's a
    // real problem here specifically: the trackpad isn't bound to a
    // working driver until this reboot actually happens, so if that
    // notification needs to be dismissed/clicked and the person has no
    // other pointing device, they may have no way to interact with it at
    // all. /t 0 reboots immediately - no countdown, no notification, no
    // dialog of any kind. /c is dropped too since it only ever displays
    // during a visible countdown, which no longer exists at /t 0.
    Exec(ExpandConstant('{sys}\shutdown.exe'),
      '/r /t 0 /f',
      '', SW_HIDE, ewNoWait, ResultCode);
    // WizardForm.Tag := 1 bypasses Inno's built-in OnCloseQuery confirmation
    // ("Setup is not complete. If you exit now... Exit Setup?"), which
    // otherwise fires because we're closing the wizard while it's still on
    // wpInstalling, not wpFinished. Without this, WizardForm.Close below
    // does NOT close silently - the user has to manually confirm/close,
    // defeating the whole point of scheduling an automatic reboot here.
    WizardForm.Tag := 1;
    WizardForm.Close;
  end;
end;

#else
function InstallGui(): Boolean;
begin
  Result := True;
end;

function InstallDriver(): Boolean;
begin
  Result := False;
end;
#endif

// BUG FIX: this is the real fix for "Finished page still shows instead of
// an immediate close+reboot". The earlier approach (WizardForm.Tag := 1;
// WizardForm.Close; inside CurStepChanged(ssDone) above) tried to close
// the wizard AFTER Inno had already decided to navigate to wpFinished -
// by then the page (with the [Run] section's "Launch Wellspring Control
// Center" checkbox) was already being displayed, so closing it just meant
// the user had to dismiss/click through what they were already looking
// at. ShouldSkipPage runs BEFORE Inno ever tries to display a given page,
// so returning True here for wpFinished means that page - and its Launch
// checkbox - is never rendered in the first place whenever a reboot is
// pending. Launching the GUI before the reboot would do nothing useful
// anyway, since the driver isn't actually bound to the device until the
// machine restarts. Once the last page is skipped, Setup has nothing
// left to navigate to and closes itself automatically - combined with
// the scheduled `shutdown /r /t 0 /f` in CurStepChanged, this is what
// makes "driver installed -> Setup closes -> PC reboots" happen with no
// intermediate screen at all. RebootNeeded is declared unconditionally
// (see the `var` block above), so this stays a safe no-op - and the
// normal Finished page keeps showing as before - in a GUI-only build
// where RebootNeeded is never set.
// Also gated on NoAutoRebootCheckBox: when it's checked, CurStepChanged
// above deliberately does NOT schedule a reboot or close the window, so
// the Finished page must NOT be skipped either - Setup has to stay open
// and wait for the user like a normal install, not close itself early.
#ifdef IncludeDriver
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpFinished) and RebootNeeded and
    (not NoAutoRebootCheckBox.Checked);
end;
#else
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpFinished) and RebootNeeded;
end;
#endif

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsDotNet8DesktopRuntimeInstalled() then
  begin
    MsgBox('.NET 8 Desktop Runtime was not found.' + #13#10 +
      'This build of Wellspring Control Center requires it to run.' + #13#10 +
      'Install it from https://dotnet.microsoft.com/download/dotnet/8.0 ' +
      '("Desktop Runtime" section), then run the installer again, or ' +
      'install the runtime later - setup will continue.',
      mbInformation, MB_OK);
  end;
end;

// Force-closes the GUI if it's running (needed both for a clean install
// over an older version and for uninstall).
procedure KillRunningApp;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM "' + ProcessName + '" /T',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Small pause so the OS has time to release the exe's file handles.
  Sleep(400);
end;

// Runs before files are copied - both on a first install and on an
// upgrade over an existing one (Inno determines this itself via AppId).
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillRunningApp;
  Result := '';
end;

// Runs on every uninstall that removes the GUI. Asks once, deletes the
// shared ProgramData settings folder and the per-user autostart key if
// the user agrees.
procedure MaybeDeleteSettings;
var
  SettingsDir: String;
begin
  // Shared folder for all users - unlike the former {userappdata},
  // deleting this affects ALL accounts on the machine, not just the one
  // that ran the uninstall.
  SettingsDir := ExpandConstant('{commonappdata}\') + SettingsDirName;

  if MsgBox(
    'Delete Wellspring Control Center settings and profiles?' + #13#10 +
    SettingsDir + #13#10 +
    '(shared across all users on this machine)',
    mbConfirmation, MB_YESNO) = IDYES then
  begin
    DelTree(SettingsDir, True, True, True);
    // The autostart key is a per-user Windows mechanism (HKCU), so it's
    // only removed for the account that ran the uninstall; if other
    // users enabled autostart under their own accounts, their entries
    // will remain.
    RegDeleteValue(HKEY_CURRENT_USER, RunKeyPath, RunValueName);
  end;
end;

#ifdef IncludeDriver

// Removes every installed package originating from our own INFs - the
// haptic actuator driver first (when this build ships it), then the touchpad
// driver (same Get-WindowsDriver matching approach as on install, see
// RemovePackagesByInf above) - then asks about Test Mode and rescans devices
// so the touchpad doesn't sit dead until next boot.
procedure RemoveDriverNow;
var
  ResultCode: Integer;
begin
  // Best-effort: if listing/removal fails for any reason, fall through to
  // the rescan anyway rather than leaving the touchpad in limbo.
  #ifdef IncludeHaptic
  RemovePackagesByInf('{#HapticDriverName}.inf', 'uninstall_haptic_drivers', False);
  #endif
  RemovePackagesByInf('{#DriverName}.inf', 'uninstall_drivers', False);

  // Relevant whenever the driver itself is being removed - ask whether
  // to also turn Test Mode back off.
  if MsgBox(
    'Disable Windows Test Mode (testsigning)?' + #13#10 +
    'Only say No if some other test-signed driver on this machine still ' +
    'needs it.',
    mbConfirmation, MB_YESNO) = IDYES then
    Exec(ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning off', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // Without this, the touchpad is left with no bound driver at all until
  // the next reboot - rescanning lets Windows re-bind it (to its default
  // HID driver, or back to ours if something went wrong above) right away.
  Exec(ExpandConstant('{sys}\pnputil.exe'), '/scan-devices', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

#endif

function InitializeUninstall(): Boolean;
begin
  // No custom dialog, no choice - always remove both the GUI and the
  // driver. Fall through with Result = True and let Inno's normal
  // uninstall proceed: CurUninstallStepChanged below removes the driver
  // first, then Inno removes the GUI's files, shortcut, registry entry,
  // and finally itself.
  KillRunningApp;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    #ifdef IncludeDriver
    RemoveDriverNow;
    #endif
    MaybeDeleteSettings;
  end;
end;
