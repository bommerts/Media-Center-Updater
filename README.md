# Media Center Updater

A Windows tray app that keeps the software on a home media server up to date. It checks for and installs updates for **Plex Media Server**, **SABnzbd**, **Radarr**, **Sonarr**, and **Pulsarr**, either when you click a button or on a schedule.

It is a native Windows app built with WinUI 3 (Windows App SDK 2.4.0) and .NET 10. The published build is self-contained, so the target PC does not need .NET or the Windows App SDK installed.

## Requirements

- Windows 10 (version 2004 or later) or Windows 11, x64.
- The applications you want to update already installed on the same PC as the updater. You don't need all five; turn off monitoring for any you don't use.
- Administrator rights on that PC. Most installers need them.

## Install

1. Build the app (see [Building](#building)), then copy the **whole** published `App` folder to a permanent location on the PC that runs your media apps, for example `C:\Program Files\Media Center Updater`. Copy every file, including `resources.pri` and the DLLs, not just the EXE.
2. Sign in to Windows as the account that runs your media apps. Plex and SABnzbd in particular are usually run by one specific user. Your Windows password is never stored by the updater.
3. Run `MediaCenterUpdater.exe` and approve the Windows administrator prompt.

## Use

1. **Check for updates** checks every monitored program without installing anything.
2. Review each program's **Monitor** and **Automatic install** switches. Both are on by default.
3. **Check & install** checks and immediately applies eligible updates, respecting each program's Automatic install switch.
4. Optional: turn on **Start with Windows when I sign in**. This creates a Task Scheduler task for the current Windows user that starts the updater in the tray with administrator rights, so there is no elevation prompt at each sign-in. Use this option rather than a shortcut in the Startup folder. It runs after sign-in, not before.
5. Optional: turn on **Scheduled updates** and pick an interval (1 to 168 hours) or a daily time. Scheduled runs install updates for monitored programs that have Automatic install turned on. The schedule is off by default.

Closing or minimizing the window hides it in the notification area by default. Double-click the tray icon to reopen it; its menu offers Open, Check for updates, and Exit. Settings let you turn this off, start minimized, and choose a Light, Dark, or system theme. The updater only runs while a Windows user is signed in and the PC is awake. A missed scheduled run happens once when the app next resumes or starts.

## Supported update methods

| Application | Check and install method |
| --- | --- |
| Plex Media Server | Official public Plex release feed, matching the installed architecture. Verifies the feed checksum and Plex's Authenticode signature, then runs the installer silently without rebooting. |
| SABnzbd | Latest stable GitHub release, matching Windows installer, SHA-256 check, silent update. Restarts the app or service if it was running. |
| Radarr | The app's own version and update APIs and built-in update command. Confirms the new version after restart. |
| Sonarr | Same as Radarr, following Sonarr's configured release branch. |
| Pulsarr | Latest stable GitHub release, SHA-256 check, silent Windows installer. Requires Pulsarr's official Windows installer with the Windows service option, and keeps its saved install options. |

The updater only updates existing installations. It does not install missing applications, update Docker containers, or replace source checkouts. Plex uses its public release channel; Plex Pass preview builds are not included. Radarr and Sonarr stay on whichever release branch they're configured for.

### Radarr and Sonarr connections

Radarr and Sonarr default to `http://localhost:7878` and `http://localhost:8989`. If yours use a different port, URL base, or HTTPS, change the URL in the app.

They also need an API key, which works like a password that lets the updater check them and ask them to update. When the updater opens, it reads each app's local settings file and fills in any empty key boxes. Keys are saved encrypted for the current Windows account. **Find API keys again** repeats the search and replaces a key only when it finds a matching one. Keys are only read for addresses on this PC; for anything else, copy the key from the app's **Settings > General > Security** and click **Save connections**.

Plex, SABnzbd, and Pulsarr don't need API keys.

### Pulsarr installation check

Automatic Pulsarr updates need its official Windows installer with the Windows service option, so the installer can stop Pulsarr, update it, and start it again. The app checks your installation at launch and when you click **Find API keys again**, and tells you whether it already meets this requirement. A supported installation doesn't need reinstalling.

## Safety

Updates can briefly interrupt playback or downloads. Installers run one at a time. The app prevents overlapping runs, rechecks versions just before installing, verifies downloads, never reboots Windows, and reports failures for each application separately. It refuses to act when it finds more than one running copy of an app, or a copy running under a different Windows account, so it can't restart the wrong configuration. It keeps existing launch arguments and refuses relative SABnzbd configuration paths.

If an installer times out, it's left running and automatic retries for that application pause. Check the installer log and installed version before trying again. The pending marker in `%LOCALAPPDATA%\MediaCenterUpdater\Pending` clears itself once the installer finishes and the new version is confirmed. If an install failed, check the application before deleting its pending marker by hand.

## Data

Settings, recent activity, installer logs, and crash reports are stored in `%LOCALAPPDATA%\MediaCenterUpdater` for the Windows user running the updater. **Open log folder** opens it. Activity logs rotate at about 2 MB. Invalid settings or unreadable encrypted keys turn off the schedule until fixed.

## Building

Build with Visual Studio 2026 or the .NET 10 SDK. Run `Publish.ps1` to run the test suite and publish the self-contained x64 app to the `App` folder. `bin`, `obj`, and `App` are build output and aren't checked in.

The tests cover schedule recovery, daylight-saving time, non-overlapping runs, per-program switches, encrypted settings, failure isolation, URL credentials, version comparisons, and Radarr/Sonarr container detection. They use simulated providers and HTTP responses and never install real updates.

## Sources

- [Microsoft Windows interface guidance](https://learn.microsoft.com/en-us/windows/apps/get-started/)
- [.NET support policy](https://dotnet.microsoft.com/en-us/platform/support/policy)
- [Plex official release feed](https://plex.tv/api/downloads/5.json)
- [SABnzbd Windows installer source](https://github.com/sabnzbd/sabnzbd/blob/5.1.3/builder/win/NSIS_Installer.nsi)
- [Pulsarr Windows installer source](https://github.com/jamcalli/Pulsarr/blob/v0.19.2/scripts/installers/windows/pulsarr.iss)
- [Sonarr built-in updater](https://github.com/Sonarr/Sonarr/blob/develop/src/NzbDrone.Core/Update/InstallUpdateService.cs)
- [Inno Setup command-line switches](https://jrsoftware.org/ishelp/topic_setupcmdline.htm)
