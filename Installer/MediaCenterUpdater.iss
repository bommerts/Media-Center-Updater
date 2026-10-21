; Inno Setup script for Media Center Updater.
; Build it with: .\Publish.ps1 -Installer   (passes MyAppVersion from MediaCenterUpdater.csproj)

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

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\App\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
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
function ShouldRelaunch(Mode: String): Boolean;
begin
  Result := WizardSilent and (CompareText(ExpandConstant('{param:RELAUNCH|}'), Mode) = 0);
end;
