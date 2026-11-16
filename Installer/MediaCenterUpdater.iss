; Inno Setup script for Media Center Updater.
; Build it with: .\Publish.ps1 -Installer   (passes MyAppVersion from src\version.h)

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppName "Media Center Updater"
#define MyAppExe "MediaCenterUpdater.exe"

[Setup]
AppId={{6F1B8C2E-4A3D-4E8B-9C71-2D5A0E7F3B94}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=bommerts
AppPublisherURL=https://github.com/bommerts/Media-Center-Updater
AppSupportURL=https://github.com/bommerts/Media-Center-Updater/issues
AppUpdatesURL=https://github.com/bommerts/Media-Center-Updater/releases
VersionInfoVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
SetupIconFile=..\Assets\AppIcon.ico
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
OutputDir=Output
OutputBaseFilename=MediaCenterUpdater-{#MyAppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; Close a running copy (it holds this mutex) before installing or uninstalling.
AppMutex=MediaCenterUpdater.Instance
CloseApplications=yes
; The app's own update shows only the progress window. Cancelling it would leave the app closed.
AllowCancelDuringInstall=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\App\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
; shellexec lets Windows show the app's own administrator prompt.
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent shellexec
; The app's own auto-update runs this installer silently with /RELAUNCH=tray or /RELAUNCH=window,
; then exits. Reopen it the same way once the new version is in place.
Filename: "{app}\{#MyAppExe}"; Parameters: "--tray"; Flags: nowait; Check: ShouldRelaunch('tray')
Filename: "{app}\{#MyAppExe}"; Flags: nowait; Check: ShouldRelaunch('window')

[UninstallRun]
; Remove the "Start with Windows" task so it doesn't point at a deleted program.
Filename: "{app}\{#MyAppExe}"; Parameters: "--remove-startup"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveStartupTask"

[Code]
// The app's own update runs with /SILENT, which shows just the progress window.
// Say plainly what is happening, since the app has just closed itself.
procedure InitializeWizard;
begin
  if WizardSilent then
    WizardForm.Caption := 'Updating {#MyAppName}';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if WizardSilent and (CurPageID = wpInstalling) then
  begin
    WizardForm.PageNameLabel.Caption := 'Installing version {#MyAppVersion}';
    WizardForm.PageDescriptionLabel.Caption := '{#MyAppName} will reopen when the update is finished.';
  end;
end;

function ShouldRelaunch(Mode: String): Boolean;
begin
  Result := WizardSilent and (CompareText(ExpandConstant('{param:RELAUNCH|}'), Mode) = 0);
end;

// Versions up to 0.3.2 were built on .NET and installed several hundred support files and folders
// next to the program. The app is now a single program, so an upgrade removes what they left behind.
// MediaCenterUpdater.dll only exists where one of those versions was installed, so nothing is
// touched in any other folder.
procedure RemoveEarlierDotNetFiles;
var
  App, Folders, Name: String;
  Comma: Integer;
begin
  App := ExpandConstant('{app}');
  if not FileExists(App + '\MediaCenterUpdater.dll') then
    Exit;
  DelTree(App + '\*.dll', False, True, False);
  DelTree(App + '\*.winmd', False, True, False);
  DelTree(App + '\*.pri', False, True, False);
  DelTree(App + '\*.json', False, True, False);
  DeleteFile(App + '\MediaCenterUpdater.pdb');
  DeleteFile(App + '\createdump.exe');
  DeleteFile(App + '\RestartAgent.exe');
  Folders := 'Assets,Microsoft.UI.Xaml,af-ZA,am-ET,ar-SA,as-IN,az-Latn-AZ,bg-BG,bn-IN,bs-Latn-BA,ca-ES,ca-Es-VALENCIA,' +
    'cs-CZ,cs,cy-GB,da-DK,de-DE,de,el-GR,en-GB,en-us,es-ES,es-MX,es,et-EE,eu-ES,fa-IR,fi-FI,fil-PH,fr-CA,fr-FR,fr,' +
    'ga-IE,gd-gb,gl-ES,gu-IN,he-IL,hi-IN,hr-HR,hu-HU,hy-AM,id-ID,is-IS,it-IT,it,ja-JP,ja,ka-GE,kk-KZ,km-KH,kn-IN,' +
    'ko-KR,ko,kok-IN,lb-LU,lo-LA,lt-LT,lv-LV,mi-NZ,mk-MK,ml-IN,mr-IN,ms-MY,mt-MT,nb-NO,ne-NP,nl-NL,nn-NO,or-IN,' +
    'pa-IN,pl-PL,pl,pt-BR,pt-PT,quz-PE,ro-RO,ru-RU,ru,sk-SK,sl-SI,sq-AL,sr-Cyrl-BA,sr-Cyrl-RS,sr-Latn-RS,sv-SE,' +
    'ta-IN,te-IN,th-TH,tr-TR,tr,tt-RU,ug-CN,uk-UA,ur-PK,uz-Latn-UZ,vi-VN,zh-CN,zh-Hans,zh-Hant,zh-TW';
  while Folders <> '' do
  begin
    Comma := Pos(',', Folders);
    if Comma = 0 then
    begin
      Name := Folders;
      Folders := '';
    end
    else
    begin
      Name := Copy(Folders, 1, Comma - 1);
      Delete(Folders, 1, Comma);
    end;
    DelTree(App + '\' + Name, True, True, True);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    RemoveEarlierDotNetFiles;
end;
