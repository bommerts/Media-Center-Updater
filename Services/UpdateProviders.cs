using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace MediaCenterUpdater;

/// <summary>Uses official public release feeds and each application's own Windows updater.</summary>
public sealed class UpdateProviders : IUpdateProvider
{
    private const string PulsarrKey = "{B8A42C5E-7D91-4F8C-B5E3-9A7C6D8E2F10}_is1";
    private const string UninstallRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
    private static readonly HttpClient SharedHttp = CreateClient();
    private readonly HttpClient Http;

    public UpdateProviders(HttpClient? httpClient = null) => Http = httpClient ?? SharedHttp;

    public async Task<UpdateResult> CheckAsync(ProgramSettings settings, CancellationToken cancellationToken)
    {
        try
        {
            if (settings.Id is ApplicationId.Radarr or ApplicationId.Sonarr)
                return await CheckArrAsync(settings, cancellationToken);
            return await CheckInstallerAsync(settings.Id, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception ex) { return Failure(settings.Id, ex); }
    }

    public async Task<UpdateResult> InstallAsync(ProgramSettings settings, UpdateResult update, CancellationToken cancellationToken)
    {
        try
        {
            // Recheck immediately before changing an application. Never trust a stale UI result or URL.
            var current = await CheckAsync(settings, cancellationToken);
            if (current.State != UpdateState.UpdateAvailable) return current;
            if (settings.Id is ApplicationId.Radarr or ApplicationId.Sonarr)
                return await InstallArrAsync(settings, current, cancellationToken);
            if (!IsAdministrator())
                return current with { State = UpdateState.NeedsSetup, Message = "Restart Media Center Updater as administrator to install Windows updates automatically." };
            return await InstallWindowsAsync(settings.Id, current, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception ex) { return Failure(settings.Id, ex) with { InstalledVersion = update.InstalledVersion, AvailableVersion = update.AvailableVersion }; }
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromSeconds(30) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaCenterUpdater/1.0");
        return client;
    }

    private async Task<UpdateResult> CheckInstallerAsync(ApplicationId id, CancellationToken ct)
    {
        var install = FindInstallation(id);
        var pending = ReadPendingInstallation(id);
        if (pending is not null)
        {
            var active = Process.GetProcessesByName(Path.GetFileNameWithoutExtension(pending.InstallerPath));
            var stillRunning = active.Length > 0;
            foreach (var process in active) process.Dispose();
            if (!stillRunning && install is not null && TryVersion(install.Version, out _) && CompareVersions(install.Version, pending.Version) >= 0)
                File.Delete(PendingPath(id));
            else
                return new(id, UpdateState.NeedsSetup, install?.Version ?? "", pending.Version, "A previous installer is running or its outcome is unverified. Automatic retries are paused to avoid overlapping installations. Check the application and its installer log.");
        }
        if (install is null)
            return new(id, UpdateState.NotInstalled, Message: "No supported native Windows installation was found. Docker and source installations are not changed.");
        if (!TryVersion(install.Version, out var installed))
            return new(id, UpdateState.NeedsSetup, install.Version, Message: "The installed version could not be determined safely.");
        if (!File.Exists(install.Executable))
            return new(id, UpdateState.NeedsSetup, install.Version, Message: "The registered application files were not found. Repair its Windows installation first.");
        if (id == ApplicationId.Sabnzbd && !Is64BitExecutable(install.Executable))
            return new(id, UpdateState.NeedsSetup, install.Version, Message: "SABnzbd's current Windows installer is 64-bit. Migrate this 32-bit installation manually first.");
        if (id == ApplicationId.Pulsarr)
        {
            var setup = LocalSetupDiscovery.CheckPulsarrSetup();
            if (!setup.IsSupported) return new(id, UpdateState.NeedsSetup, install.Version, Message: setup.Message);
        }

        string version, url, digest;
        if (id == ApplicationId.Plex)
        {
            using var feed = await GetJsonAsync(new Uri("https://plex.tv/api/downloads/5.json"), null, ct);
            var windows = feed.RootElement.GetProperty("computer").GetProperty("Windows");
            var build = Is64BitExecutable(install.Executable) ? "windows-x86_64" : "windows-x86";
            var release = windows.GetProperty("releases").EnumerateArray().FirstOrDefault(x => Text(x, "build") == build);
            if (release.ValueKind == JsonValueKind.Undefined)
                return new(id, UpdateState.NeedsSetup, install.Version, Message: "Plex has no public Windows release for this installed architecture.");
            version = Text(release, "version"); url = Text(release, "url"); digest = Text(release, "checksum");
            if (!Regex.IsMatch(digest, "\\A[0-9a-fA-F]{40}\\z")) throw new ProviderException("The official Plex feed did not provide a valid checksum.");
            ValidateDownloadUrl(id, new Uri(url));
            // Plex changed from the WiX bootstrapper to Inno Setup with its 1.43 series.
            // Older x86 releases remain visible for status, but are not run with guessed switches.
            if (CompareVersions(version, install.Version) > 0 && (!TryVersion(version, out var target) || target < new Version(1, 43)))
                return new(id, UpdateState.NeedsSetup, install.Version, version, "This older Plex installer requires a manual upgrade. Current 64-bit releases are supported.");
        }
        else
        {
            var repository = id == ApplicationId.Sabnzbd ? "sabnzbd/sabnzbd" : "jamcalli/Pulsarr";
            using var release = await GetJsonAsync(new Uri($"https://api.github.com/repos/{repository}/releases/latest"), null, ct);
            if (release.RootElement.GetProperty("prerelease").GetBoolean() || release.RootElement.GetProperty("draft").GetBoolean())
                throw new ProviderException("The release feed did not return a stable published release.");
            version = Text(release.RootElement, "tag_name").TrimStart('v');
            var expected = id == ApplicationId.Sabnzbd ? $"SABnzbd-{version}-win-setup.exe" : $"pulsarr-v{version}-windows-x64-setup.exe";
            var asset = release.RootElement.GetProperty("assets").EnumerateArray().FirstOrDefault(x => Text(x, "name") == expected);
            if (asset.ValueKind == JsonValueKind.Undefined) throw new ProviderException("The latest stable release has no supported Windows installer.");
            url = Text(asset, "browser_download_url"); digest = Text(asset, "digest");
            if (!Regex.IsMatch(digest, "\\Asha256:[0-9a-fA-F]{64}\\z")) throw new ProviderException("The official release did not provide a SHA-256 digest. Installation was disabled.");
            digest = digest[7..];
            ValidateDownloadUrl(id, new Uri(url));
            if (!Environment.Is64BitOperatingSystem)
                return new(id, UpdateState.NeedsSetup, install.Version, version, "Current releases require 64-bit Windows.");
        }
        if (!TryVersion(version, out var available)) throw new ProviderException("The published version could not be compared safely.");
        var newer = available > installed;
        return new(id, newer ? UpdateState.UpdateAvailable : UpdateState.UpToDate, install.Version, version,
            newer ? "A stable update is available from the official publisher." : "The installed version is current or newer than the public release.", url, digest);
    }

    private async Task<UpdateResult> CheckArrAsync(ProgramSettings settings, CancellationToken ct)
    {
        var connection = GetArrConnection(settings);
        if (string.IsNullOrWhiteSpace(connection.Key))
            return new(settings.Id, UpdateState.NeedsSetup, Message: "This application's API key was not found automatically. In Settings, choose Retrieve API keys, or copy its key from the application's Settings > General > Security page.");
        using var status = await GetJsonAsync(Endpoint(connection.Url, "api/v3/system/status"), connection.Key, ct);
        var installed = Text(status.RootElement, "version");
        if (!TryVersion(installed, out _)) throw new ProviderException("The application returned an unrecognized installed version.");
        var mechanism = Text(status.RootElement, "packageUpdateMechanism");
        if ((status.RootElement.TryGetProperty("isDocker", out var docker) && docker.ValueKind == JsonValueKind.True) ||
            (!string.IsNullOrEmpty(mechanism) && !mechanism.Equals("builtIn", StringComparison.OrdinalIgnoreCase) && mechanism != "0"))
            return new(settings.Id, UpdateState.NeedsSetup, installed, Message: "This installation uses an external package or container updater. Its installation is left unchanged.");
        using var updates = await GetJsonAsync(Endpoint(connection.Url, "api/v3/update"), connection.Key, ct);
        var available = updates.RootElement.EnumerateArray().FirstOrDefault(x => x.TryGetProperty("latest", out var latest) && latest.ValueKind == JsonValueKind.True);
        if (available.ValueKind == JsonValueKind.Undefined)
            return new(settings.Id, UpdateState.UpToDate, installed, installed, "The application's update service reports no newer release.");
        var version = Text(available, "version");
        if (!TryVersion(installed, out _) || !TryVersion(version, out _)) throw new ProviderException("The application returned an unrecognized version.");
        var newer = CompareVersions(version, installed) > 0;
        var installable = available.TryGetProperty("installable", out var flag) && flag.ValueKind == JsonValueKind.True && newer;
        if (newer && !installable)
            return new(settings.Id, UpdateState.NeedsSetup, installed, version, "A newer version exists, but the application's update service says it cannot be installed automatically. Check System > Updates in the application.");
        return new(settings.Id, installable ? UpdateState.UpdateAvailable : UpdateState.UpToDate, installed, version,
            installable ? "An update is available on the application's configured release branch." : "The application's configured release branch is current.");
    }

    private async Task<UpdateResult> InstallArrAsync(ProgramSettings settings, UpdateResult update, CancellationToken ct)
    {
        var connection = GetArrConnection(settings);
        using var request = new HttpRequestMessage(HttpMethod.Post, Endpoint(connection.Url, "api/v3/command"));
        request.Headers.Add("X-Api-Key", connection.Key);
        request.Content = JsonContent.Create(new { name = "ApplicationUpdate", installMajorUpdate = true });
        using var response = await Http.SendAsync(request, ct);
        RequireSuccess(response);
        using var command = await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
        var commandId = command.RootElement.GetProperty("id").GetInt32();
        // A successful POST only accepts work. The server restart and running version prove completion.
        var deadline = DateTimeOffset.UtcNow.AddMinutes(5);
        using var pollTimeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        pollTimeout.CancelAfter(TimeSpan.FromMinutes(5));
        while (DateTimeOffset.UtcNow < deadline)
        {
            await Task.Delay(TimeSpan.FromSeconds(5), ct);
            try
            {
                using var status = await GetJsonAsync(Endpoint(connection.Url, "api/v3/system/status"), connection.Key, pollTimeout.Token);
                var installed = Text(status.RootElement, "version");
                if (CompareVersions(installed, update.AvailableVersion) >= 0)
                    return update with { State = UpdateState.Updated, InstalledVersion = installed, Message = "Update installed and the running application responded with the new version." };
                using var running = await GetJsonAsync(Endpoint(connection.Url, $"api/v3/command/{commandId}"), connection.Key, pollTimeout.Token);
                if (Text(running.RootElement, "status").Equals("failed", StringComparison.OrdinalIgnoreCase))
                    return update with { State = UpdateState.Error, Message = "The application's updater failed. Check its System > Logs page for details." };
            }
            catch (HttpRequestException) { }
            catch (TaskCanceledException) when (!ct.IsCancellationRequested) { }
            catch (ProviderException) { }
        }
        return update with { State = UpdateState.Error, Message = "The update was requested, but the new running version was not verified within five minutes. It may still be applying. Check the application before retrying." };
    }

    private static (Uri Url, string Key) GetArrConnection(ProgramSettings settings)
    {
        if (!Uri.TryCreate(settings.ServerUrl.TrimEnd('/') + "/", UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) ||
            !string.IsNullOrEmpty(uri.UserInfo) || !string.IsNullOrEmpty(uri.Query) || !string.IsNullOrEmpty(uri.Fragment))
            throw new ProviderException("Enter an HTTP or HTTPS application address without embedded credentials, a query, or a fragment.");
        var key = settings.ApiKey.Trim();
        if (key.Length == 0) key = LocalSetupDiscovery.FindApiKey(settings.Id, settings.ServerUrl).ApiKey;
        return (uri, key);
    }

    private async Task<UpdateResult> InstallWindowsAsync(ApplicationId id, UpdateResult update, CancellationToken ct)
    {
        var install = FindInstallation(id) ?? throw new ProviderException("The Windows installation was removed before updating.");
        var serviceName = id switch { ApplicationId.Sabnzbd => "SABnzbd", ApplicationId.Pulsarr => "pulsarr", _ => "" };
        var serviceRunning = serviceName.Length > 0 && ServiceRunning(serviceName);
        var processName = id == ApplicationId.Plex ? "Plex Media Server" : "SABnzbd";
        var running = id == ApplicationId.Pulsarr || serviceRunning ? null : await GetRunningApplicationAsync(processName, install.Executable, ct);
        if (serviceRunning && id == ApplicationId.Sabnzbd)
        {
            var instances = Process.GetProcessesByName(processName);
            var multiple = instances.Length > 1;
            foreach (var process in instances) process.Dispose();
            if (multiple) throw new ProviderException("Multiple SABnzbd instances are running. Automatic installation is disabled to preserve their separate settings.");
        }
        var cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaCenterUpdater", "Downloads");
        Directory.CreateDirectory(cache);
        var file = Path.Combine(cache, $"{id}-{Guid.NewGuid():N}.exe");
        try
        {
            await DownloadAsync(id, new Uri(update.DownloadUrl!), file, ct);
            await VerifyDownloadAsync(id, file, update.Sha256!, ct);
            ct.ThrowIfCancellationRequested();
            var args = new List<string>();
            if (id == ApplicationId.Sabnzbd)
            {
                args.Add("/S");
                // NSIS treats the last /D argument specially. Quote handling belongs to its parser,
                // so let the official installer recover its registered existing directory instead.
            }
            else
            {
                args.AddRange(new[] { "/VERYSILENT", "/SUPPRESSMSGBOXES", "/SP-", "/NORESTART", "/RESTARTEXITCODE=3010", "/LOG=" + Path.ChangeExtension(file, ".log") });
                if (id == ApplicationId.Pulsarr)
                {
                    // Existing service and firewall options must survive an unattended upgrade.
                    args.Add("/COMPONENTS=main,service");
                    if (install.SelectedTasks is null)
                        throw new ProviderException("Pulsarr's saved installation options could not be read. Run its current installer once interactively before enabling automatic updates.");
                    var tasks = install.SelectedTasks.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).ToHashSet(StringComparer.OrdinalIgnoreCase);
                    if (serviceRunning) tasks.Add("startafterinstall"); else tasks.Remove("startafterinstall");
                    args.Add("/TASKS=" + string.Join(',', tasks));
                }
            }
            // Once an installer starts it must finish even if the window closes or cancellation is requested.
            // A timeout reports uncertainty and deliberately does not kill a live installer.
            var pendingPath = PendingPath(id);
            Directory.CreateDirectory(Path.GetDirectoryName(pendingPath)!);
            await File.WriteAllTextAsync(pendingPath, JsonSerializer.Serialize(new PendingInstallation(update.AvailableVersion, file)), ct);
            var exit = await RunProcessAsync(file, args, TimeSpan.FromMinutes(15), CancellationToken.None, false);
            // A completed installer can be retried after a reported error. A timeout/exception leaves
            // the marker in place across application restarts until the new version is verified.
            File.Delete(pendingPath);
            if (exit.ExitCode != 0 && exit.ExitCode != 3010)
                throw new ProviderException($"The official installer returned error {exit.ExitCode}. Check the application's installation log.");
            var after = FindInstallation(id);
            if (after is null || CompareVersions(after.Version, update.AvailableVersion) < 0)
                return update with { State = UpdateState.Error, Message = "The installer finished, but Windows did not report the expected new version. Check the application before retrying." };
            if (exit.ExitCode == 3010)
                return update with { State = UpdateState.RestartRequired, InstalledVersion = after.Version, Message = "The new version is installed. Windows reports that a restart is required; no restart was performed." };
            if (serviceRunning && !ServiceRunning(serviceName))
            {
                var result = await RunProcessAsync(Path.Combine(Environment.SystemDirectory, "sc.exe"), new[] { "start", serviceName }, TimeSpan.FromSeconds(30), CancellationToken.None, true);
                if (result.ExitCode != 0 && result.ExitCode != 1056)
                    return update with { State = UpdateState.Error, InstalledVersion = after.Version, Message = "The update installed, but its previously running Windows service could not be restarted." };
            }
            else if (running is not null && !IsExpectedProcessRunning(processName, after.Executable))
            {
                var start = new ProcessStartInfo(after.Executable) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(after.Executable)! };
                foreach (var argument in running) start.ArgumentList.Add(argument);
                Process.Start(start)?.Dispose();
            }
            if (serviceRunning)
            {
                for (var attempt = 0; attempt < 20 && !ServiceRunning(serviceName); attempt++) await Task.Delay(1500);
                if (!ServiceRunning(serviceName))
                    return update with { State = UpdateState.Error, InstalledVersion = after.Version, Message = "The new version installed, but the Windows service has not recovered." };
            }
            else if (running is not null)
            {
                await Task.Delay(3000);
                if (!IsExpectedProcessRunning(processName, after.Executable))
                    return update with { State = UpdateState.Error, InstalledVersion = after.Version, Message = "The new version installed, but the previously running application did not stay running." };
            }
            return update with { State = UpdateState.Updated, InstalledVersion = after.Version, Message = "The new installed version was verified. Previously running applications or services were restored." };
        }
        finally
        {
            // Keep the small installation log, but remove this invocation's downloaded installer.
            try { File.Delete(file); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    private async Task DownloadAsync(ApplicationId id, Uri uri, string file, CancellationToken ct)
    {
        ValidateDownloadUrl(id, uri);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(TimeSpan.FromMinutes(10));
        for (var redirects = 0; redirects < 6; redirects++)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token);
            if ((int)response.StatusCode is >= 300 and < 400)
            {
                var location = response.Headers.Location ?? throw new ProviderException("The download redirect had no destination.");
                uri = location.IsAbsoluteUri ? location : new Uri(uri, location);
                if (uri.Scheme != "https" || !string.IsNullOrEmpty(uri.UserInfo) ||
                    !(uri.Host.Equals("downloads.plex.tv", StringComparison.OrdinalIgnoreCase) || uri.Host.EndsWith(".githubusercontent.com", StringComparison.OrdinalIgnoreCase)))
                    throw new ProviderException("The publisher redirected the download to an unsupported destination.");
                continue;
            }
            RequireSuccess(response);
            if (response.Content.Headers.ContentLength is > 1_500_000_000) throw new ProviderException("The installer exceeded the download size limit.");
            await using var source = await response.Content.ReadAsStreamAsync(deadline.Token);
            await using var destination = new FileStream(file, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true);
            var buffer = new byte[81920]; long total = 0; int count;
            while ((count = await source.ReadAsync(buffer, deadline.Token)) > 0)
            {
                total += count;
                if (total > 1_500_000_000) throw new ProviderException("The installer exceeded the download size limit.");
                await destination.WriteAsync(buffer.AsMemory(0, count), deadline.Token);
            }
            return;
        }
        throw new ProviderException("The download exceeded the redirect limit.");
    }

    private static async Task VerifyDownloadAsync(ApplicationId id, string file, string expected, CancellationToken ct)
    {
        await using (var input = File.OpenRead(file))
        {
            var hash = id == ApplicationId.Plex ? await SHA1.HashDataAsync(input, ct) : await SHA256.HashDataAsync(input, ct);
            if (!Convert.ToHexString(hash).Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new ProviderException("The downloaded installer did not match the official checksum. It was not run.");
        }
        // Plex's feed provides SHA-1 only. Also require a trusted Authenticode chain and Plex publisher.
        if (id == ApplicationId.Plex)
        {
            if (!AuthenticodeTrusted(file)) throw new ProviderException("The Plex installer did not pass Windows signature verification. It was not run.");
            // X509CertificateLoader has no signed-PE signer extraction equivalent.
#pragma warning disable SYSLIB0057
            using var certificate = new X509Certificate2(X509Certificate.CreateFromSignedFile(file));
#pragma warning restore SYSLIB0057
            var name = certificate.GetNameInfo(X509NameType.SimpleName, false);
            if (name != "Plex, Inc." && name != "Plex Inc." && name != "Plex Inc")
                throw new ProviderException("The installer signature was not issued to the expected Plex publisher. It was not run.");
        }
    }

    private static void ValidateDownloadUrl(ApplicationId id, Uri uri)
    {
        var expectedPrefix = id switch
        {
            ApplicationId.Plex => "https://downloads.plex.tv/plex-media-server-new/",
            ApplicationId.Sabnzbd => "https://github.com/sabnzbd/sabnzbd/releases/download/",
            ApplicationId.Pulsarr => "https://github.com/jamcalli/Pulsarr/releases/download/",
            _ => throw new ProviderException("Unsupported Windows installer.")
        };
        if (uri.Scheme != "https" || !string.IsNullOrEmpty(uri.UserInfo) || !uri.AbsoluteUri.StartsWith(expectedPrefix, StringComparison.OrdinalIgnoreCase) || !uri.AbsolutePath.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            throw new ProviderException("The release contained an unsupported installer URL.");
    }

    private sealed record Installation(string Version, string Executable, string? SelectedTasks);
    private sealed record PendingInstallation(string Version, string InstallerPath);
    private static string PendingPath(ApplicationId id) => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaCenterUpdater", "Pending", id + ".json");
    private static PendingInstallation? ReadPendingInstallation(ApplicationId id)
    {
        var path = PendingPath(id);
        if (!File.Exists(path)) return null;
        try
        {
            return JsonSerializer.Deserialize<PendingInstallation>(File.ReadAllText(path))
                ?? throw new ProviderException("A previous installer record is invalid. Automatic updates are paused until its state is reviewed.");
        }
        catch (JsonException) { throw new ProviderException("A previous installer record is invalid. Automatic updates are paused until its state is reviewed."); }
    }

    private static Installation? FindInstallation(ApplicationId id)
    {
        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            using var root = RegistryKey.OpenBaseKey(hive, view);
            using var entries = root.OpenSubKey(UninstallRoot);
            if (entries is null) continue;
            foreach (var name in entries.GetSubKeyNames())
            {
                using var entry = entries.OpenSubKey(name);
                if (entry is null) continue;
                var display = entry.GetValue("DisplayName") as string ?? "";
                var match = id switch
                {
                    ApplicationId.Plex => Regex.IsMatch(display, "^Plex Media Server(?: |$)", RegexOptions.IgnoreCase),
                    ApplicationId.Sabnzbd => name.Equals("SABnzbd", StringComparison.OrdinalIgnoreCase),
                    ApplicationId.Pulsarr => name.Equals(PulsarrKey, StringComparison.OrdinalIgnoreCase),
                    _ => false
                };
                if (!match) continue;
                var directory = (entry.GetValue("InstallLocation") as string ?? "").Trim('"');
                if (id == ApplicationId.Sabnzbd)
                {
                    using var sab = root.OpenSubKey(@"SOFTWARE\SABnzbd");
                    directory = sab?.GetValue("") as string ?? directory;
                }
                if (string.IsNullOrEmpty(directory))
                {
                    var uninstall = entry.GetValue("UninstallString") as string ?? "";
                    var executable = uninstall.StartsWith('"') ? uninstall.Split('"').ElementAtOrDefault(1) : uninstall;
                    if (!string.IsNullOrWhiteSpace(executable) && executable.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) directory = Path.GetDirectoryName(executable) ?? "";
                }
                if (string.IsNullOrEmpty(directory) && id == ApplicationId.Plex)
                    directory = Path.Combine(Environment.GetFolderPath(view == RegistryView.Registry64 ? Environment.SpecialFolder.ProgramFiles : Environment.SpecialFolder.ProgramFilesX86), "Plex", "Plex Media Server");
                var appExe = id switch { ApplicationId.Plex => "Plex Media Server.exe", ApplicationId.Sabnzbd => "SABnzbd.exe", _ => "pulsarr-service.exe" };
                if (!Path.IsPathFullyQualified(directory))
                    throw new ProviderException("The application's registered installation directory is not an absolute path.");
                return new(entry.GetValue("DisplayVersion") as string ?? "", Path.Combine(directory, appExe), entry.GetValue("Inno Setup: Selected Tasks") as string);
            }
        }
        return null;
    }

    private static async Task<string[]?> GetRunningApplicationAsync(string name, string executable, CancellationToken ct)
    {
        var matches = Process.GetProcessesByName(name);
        try
        {
            if (matches.Length == 0) return null;
            if (matches.Length > 1) throw new ProviderException("Multiple application instances are running. Automatic installation is disabled to preserve their separate settings.");
            var process = matches[0];
            if (!string.Equals(process.MainModule?.FileName, executable, StringComparison.OrdinalIgnoreCase))
                throw new ProviderException("The running application does not match its Windows installation. Close the separate copy before updating.");
            if (!OpenProcessToken(process.Handle, 8, out var token))
                throw new ProviderException("The running application's Windows account could not be verified. Close it before updating.");
            try
            {
                using var owner = new WindowsIdentity(token);
                using var current = WindowsIdentity.GetCurrent();
                if (owner.User != current.User)
                    throw new ProviderException("The application runs under another Windows account. Run this updater in that account or use the application's Windows service.");
            }
            finally { CloseHandle(token); }
            // Preserve command-line configuration such as SABnzbd's custom data-file argument.
            // The only variable in this script is a process ID obtained from Windows.
            var script = $"$p=Get-CimInstance Win32_Process -Filter 'ProcessId={process.Id}'; if($p){{ $p.CommandLine | ConvertTo-Json -Compress }}";
            var result = await RunProcessAsync(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"), new[] { "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script }, TimeSpan.FromSeconds(20), ct, true);
            var line = result.ExitCode == 0 ? JsonSerializer.Deserialize<string>(result.Output.Trim()) : null;
            if (string.IsNullOrWhiteSpace(line)) throw new ProviderException("The running application's launch options could not be preserved. Close it before updating.");
            var arguments = SplitCommandLine(line).Skip(1).ToArray();
            for (var i = 0; i < arguments.Length; i++)
            {
                if ((arguments[i] is "-f" or "--config-file") && (i + 1 >= arguments.Length || !Path.IsPathFullyQualified(arguments[i + 1])))
                    throw new ProviderException("The application uses a relative configuration path. Close it before updating so its settings are not changed by a restart.");
                if (arguments[i].StartsWith("--config-file=", StringComparison.Ordinal) && !Path.IsPathFullyQualified(arguments[i][14..]))
                    throw new ProviderException("The application uses a relative configuration path. Close it before updating so its settings are not changed by a restart.");
            }
            return arguments;
        }
        finally { foreach (var process in matches) process.Dispose(); }
    }

    private static bool IsExpectedProcessRunning(string name, string executable)
    {
        foreach (var process in Process.GetProcessesByName(name))
        {
            using (process)
            {
                try { if (string.Equals(process.MainModule?.FileName, executable, StringComparison.OrdinalIgnoreCase)) return true; }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
        }
        return false;
    }

    private static async Task<(int ExitCode, string Output)> RunProcessAsync(string file, IEnumerable<string> args, TimeSpan timeout, CancellationToken ct, bool killOnTimeout)
    {
        var info = new ProcessStartInfo(file) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var arg in args) info.ArgumentList.Add(arg);
        using var process = Process.Start(info) ?? throw new ProviderException("Windows could not start the required process.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(timeout);
        try { await process.WaitForExitAsync(deadline.Token); }
        catch (OperationCanceledException)
        {
            if (killOnTimeout) { try { process.Kill(); } catch (InvalidOperationException) { } }
            if (ct.IsCancellationRequested) throw;
            throw new ProviderException(killOnTimeout ? "A required Windows operation timed out." : "The installer is still running after fifteen minutes. It was left running; check its status before retrying.");
        }
        await error;
        return (process.ExitCode, await output);
    }

    private static Uri Endpoint(Uri baseUri, string relative) => new(baseUri, relative);
    private async Task<JsonDocument> GetJsonAsync(Uri uri, string? apiKey, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        if (!string.IsNullOrEmpty(apiKey)) request.Headers.Add("X-Api-Key", apiKey);
        using var response = await Http.SendAsync(request, ct);
        RequireSuccess(response);
        return await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
    }

    private static void RequireSuccess(HttpResponseMessage response)
    {
        if (response.IsSuccessStatusCode) return;
        if ((int)response.StatusCode is >= 300 and < 400) throw new ProviderException("The application address redirected. Enter its direct address; credentials are not forwarded to redirects.");
        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden) throw new ProviderException("Access was denied. Check the API key or wait for the publisher's request limit to reset.");
        throw new ProviderException($"The update service returned HTTP {(int)response.StatusCode}.");
    }

    private static string Text(JsonElement element, string property) => element.TryGetProperty(property, out var value) ? value.ToString() : "";
    internal static bool TryVersion(string text, out Version version)
    {
        var match = Regex.Match(text.Trim().TrimStart('v'), @"\A(?<version>\d+(?:\.\d+){1,3})(?:[-+][0-9A-Za-z][0-9A-Za-z.-]*)?\z");
        if (match.Success && Version.TryParse(match.Groups["version"].Value, out var parsed))
        {
            version = new Version(parsed.Major, parsed.Minor, Math.Max(0, parsed.Build), Math.Max(0, parsed.Revision));
            return true;
        }
        version = new Version(0, 0, 0, 0); return false;
    }
    private static int CompareVersions(string left, string right) => TryVersion(left, out var a) && TryVersion(right, out var b) ? a.CompareTo(b) : throw new ProviderException("A version could not be compared safely.");
    private static UpdateResult Failure(ApplicationId id, Exception ex) => new(id, UpdateState.Error, Message: ex switch
    {
        ProviderException => ex.Message,
        OperationCanceledException => "The update service timed out. The next check can retry.",
        HttpRequestException => "The update service could not be reached. Check the address and network connection.",
        UnauthorizedAccessException => "Windows denied access. Check the application installation and administrator permissions.",
        _ => "The update could not be completed safely. Check the installation, settings, and publisher availability."
    });
    private sealed class ProviderException(string message) : Exception(message) { }
    public static bool IsAdministrator() { using var identity = WindowsIdentity.GetCurrent(); return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator); }
    private static bool ServiceRunning(string name)
    {
        if (string.IsNullOrEmpty(name)) return false;
        var manager = OpenSCManager(null, null, 1);
        if (manager == IntPtr.Zero) return false;
        try
        {
            var service = OpenService(manager, name, 4);
            if (service == IntPtr.Zero) return false;
            try { return QueryServiceStatus(service, out var status) && status.CurrentState == 4; }
            finally { CloseServiceHandle(service); }
        }
        finally { CloseServiceHandle(manager); }
    }
    private static bool Is64BitExecutable(string path)
    {
        using var reader = new BinaryReader(File.OpenRead(path));
        if (reader.ReadUInt16() != 0x5A4D) throw new ProviderException("The installed application is not a recognized Windows executable.");
        reader.BaseStream.Position = 0x3C; var offset = reader.ReadInt32();
        if (offset < 0 || offset > reader.BaseStream.Length - 6) throw new ProviderException("The installed executable header is invalid.");
        reader.BaseStream.Position = offset;
        if (reader.ReadUInt32() != 0x4550) throw new ProviderException("The installed executable header is invalid.");
        return reader.ReadUInt16() == 0x8664;
    }
    private static string[] SplitCommandLine(string command)
    {
        var pointer = CommandLineToArgvW(command, out var count);
        if (pointer == IntPtr.Zero) throw new ProviderException("The application's launch options could not be read.");
        try { return Enumerable.Range(0, count).Select(i => Marshal.PtrToStringUni(Marshal.ReadIntPtr(pointer, i * IntPtr.Size)) ?? "").ToArray(); }
        finally { LocalFree(pointer); }
    }

    private static bool AuthenticodeTrusted(string path)
    {
        var file = new TrustFile { Size = (uint)Marshal.SizeOf<TrustFile>(), FilePath = path };
        var filePtr = Marshal.AllocHGlobal(Marshal.SizeOf<TrustFile>());
        Marshal.StructureToPtr(file, filePtr, false);
        var data = new TrustData { Size = (uint)Marshal.SizeOf<TrustData>(), UiChoice = 2, RevocationChecks = 1, UnionChoice = 1, File = filePtr, StateAction = 1 };
        var action = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
        try { return WinVerifyTrust(new IntPtr(-1), ref action, ref data) == 0; }
        finally
        {
            data.StateAction = 2; WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            Marshal.DestroyStructure<TrustFile>(filePtr); Marshal.FreeHGlobal(filePtr);
        }
    }
    [StructLayout(LayoutKind.Sequential)] private struct ServiceStatus { public uint Type, CurrentState, ControlsAccepted, Win32ExitCode, ServiceSpecificExitCode, CheckPoint, WaitHint; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct TrustFile { public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string FilePath; public IntPtr FileHandle, KnownSubject; }
    [StructLayout(LayoutKind.Sequential)] private struct TrustData { public uint Size; public IntPtr PolicyCallback, SipClient; public uint UiChoice, RevocationChecks, UnionChoice; public IntPtr File; public uint StateAction; public IntPtr StateData, UrlReference; public uint ProviderFlags, UiContext; public IntPtr SignatureSettings; }
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr OpenSCManager(string? machine, string? database, uint access);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr OpenService(IntPtr manager, string name, uint access);
    [DllImport("advapi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool QueryServiceStatus(IntPtr service, out ServiceStatus status);
    [DllImport("advapi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CloseServiceHandle(IntPtr handle);
    [DllImport("advapi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CommandLineToArgvW(string command, out int count);
    [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr pointer);
    [DllImport("wintrust.dll", ExactSpelling = true)] private static extern int WinVerifyTrust(IntPtr hwnd, ref Guid action, ref TrustData data);
}
