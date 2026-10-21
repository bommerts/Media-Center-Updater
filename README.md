<img src="Assets/Logo.svg" alt="" width="96" align="left">

# Media Center Updater

**Current version: 0.2.4** · [Download the installer](https://github.com/bommerts/Media-Center-Updater/releases/latest)
<br clear="left">



A Windows app that keeps the software on a home media server up to date. It checks for and installs updates for **Plex Media Server**, **SABnzbd**, **Radarr**, **Sonarr**, and **Pulsarr**, either when you click a button or on a schedule.

## Features

- Checks all supported apps at once, or installs their updates in one click.
- Scheduled updates, every few hours or at a set time each day.
- Per-app switches to monitor an app, and to let it update automatically.
- Verifies every download (checksums and signatures) before installing, and never reboots Windows.
- Finds the Radarr and Sonarr API keys on the PC for you, and stores keys encrypted.
- Can start when you sign in to Windows, without an administrator prompt each time.
- Minimizes to the notification area (system tray) so it can keep running in the background.
- Light, dark, or system theme.

It is a native Windows app built with WinUI 3 (Windows App SDK 2.4.0) and .NET 10. The published build is self-contained, so the target PC does not need .NET or the Windows App SDK installed.

## Why these apps, and why Windows only?

I built this for my own media server, so it supports the apps I run today (Plex Media Server, SABnzbd, Radarr, Sonarr and Pulsarr) on the operating system I run them on, Windows.

That's where it started, not where it has to stop. I'm open to adding:

- **More apps**, such as Jellyfin, Emby, Lidarr, Prowlarr, Readarr, Bazarr, Overseerr or qBittorrent.
- **More operating systems.** The app is Windows only today, but I'd welcome support for macOS and Linux.

If you'd like to help, contributions are welcome under the project's [MIT License](LICENSE). Open an issue to suggest an app or platform, or to talk through an idea before you start, or send a pull request.

## Requirements

- Windows 10 (version 2004 or later) or Windows 11, x64.
- The applications you want to update already installed on the same PC as the updater. You don't need all five; turn off monitoring for any you don't use.
- Administrator rights on that PC. Most installers need them.

## Installing on your media server

The updater must run **on the same Windows PC as your media apps**. It updates the copies installed on that PC, so it can't update a server elsewhere on your network. In these steps, "media server" means that PC.

### Step 1: Install

1. Sign in to the media server as the **Windows account that runs your media apps**. If Plex or SABnzbd run as a normal app rather than a Windows service, they run under that one user's account. The updater refuses to restart an app that belongs to a different account. Your Windows password is never stored.
2. Download `MediaCenterUpdater-<version>-Setup.exe` from the [latest release](https://github.com/bommerts/Media-Center-Updater/releases/latest).
3. Run it and follow the steps. It installs to `C:\Program Files\Media Center Updater` and adds a Start menu shortcut.

The installer isn't code-signed yet, so Windows SmartScreen may say it "protected your PC". Click **More info**, then **Run anyway**.

Prefer to build it yourself? See [Building](#building).

### Step 2: First run

1. Open **Media Center Updater** from the Start menu (or let the installer start it) and approve the Windows administrator prompt. The app asks for this every time it starts, because most installers need administrator rights.
3. Turn off **Monitor** for any of the five programs you don't have installed.
4. **Radarr and Sonarr:** check their URLs. The defaults are `http://localhost:7878` and `http://localhost:8989`. If you changed the port, added a URL base (such as `/radarr`) or use HTTPS, enter the address you use in your browser on the media server, then click **Save connections**. The app usually finds the API keys by itself. If a key box stays empty, copy the key from that app's **Settings > General > Security**.
5. **Pulsarr:** read the installation note. If it says your setup needs attention, reinstall Pulsarr with its official Windows installer and select the Windows service option.
6. Click **Check for updates**. This only checks and installs nothing, so it's a safe way to confirm each program is detected.

### Step 3: Keep it running

1. Turn on **Start with Windows when I sign in**. The updater then starts in the notification area each time you sign in, without an administrator prompt.
2. Optional: turn on **Scheduled updates** and pick how often to check, or a daily time. Choose a time when nobody is watching or downloading, because updates briefly restart the apps.
3. Close the window. The updater keeps running in the notification area, next to the clock.

The updater only runs while someone is signed in to the media server. If your server sits at the sign-in screen after a reboot, scheduled updates won't run until someone signs in. Windows can sign in automatically if you want unattended updates; that's a Windows setting, not part of this app.

### Updating the updater

Download the newer installer from the [releases page](https://github.com/bommerts/Media-Center-Updater/releases) and run it. It closes the running updater, installs over the old version, and keeps your settings. The version you have is shown in the title bar and at the bottom of **Settings**.

### Uninstalling

Uninstall **Media Center Updater** from Windows **Settings > Apps > Installed apps**. This also removes its "Start with Windows" task. Your settings, saved keys and logs stay in `%LOCALAPPDATA%\MediaCenterUpdater`; delete that folder too if you want them gone.

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

You need the [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (x64), or Visual Studio 2026. Then, in PowerShell in the repo folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\Publish.ps1
```

This runs the tests and publishes the self-contained x64 app to the `App` folder. To also build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and add `-Installer`; the installer lands in `Installer\Output`. The version number comes from `<Version>` in `MediaCenterUpdater.csproj`. `bin`, `obj`, `App` and `Installer\Output` are build output and aren't checked in.

If you run the `App` folder without the installer, copy the whole folder, not just the EXE: the app needs the DLLs and `resources.pri` beside it.

The tests cover schedule recovery, daylight-saving time, non-overlapping runs, per-program switches, encrypted settings, failure isolation, URL credentials, version comparisons, and Radarr/Sonarr container detection. They use simulated providers and HTTP responses and never install real updates.

## License

Released under the [MIT License](LICENSE).

## Sources

- [Microsoft Windows interface guidance](https://learn.microsoft.com/en-us/windows/apps/get-started/)
- [.NET support policy](https://dotnet.microsoft.com/en-us/platform/support/policy)
- [Plex official release feed](https://plex.tv/api/downloads/5.json)
- [SABnzbd Windows installer source](https://github.com/sabnzbd/sabnzbd/blob/5.1.3/builder/win/NSIS_Installer.nsi)
- [Pulsarr Windows installer source](https://github.com/jamcalli/Pulsarr/blob/v0.19.2/scripts/installers/windows/pulsarr.iss)
- [Sonarr built-in updater](https://github.com/Sonarr/Sonarr/blob/develop/src/NzbDrone.Core/Update/InstallUpdateService.cs)
- [Inno Setup command-line switches](https://jrsoftware.org/ishelp/topic_setupcmdline.htm)
