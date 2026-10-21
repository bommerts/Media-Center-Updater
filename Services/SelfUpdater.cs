using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace MediaCenterUpdater;

public sealed record SelfUpdateInfo(string Version, Uri DownloadUrl, string Sha256);

/// <summary>Finds, downloads and starts new versions of this app's own setup program from GitHub releases.</summary>
public sealed class SelfUpdater
{
    public const string Repository = "bommerts/Media-Center-Updater";
    public static readonly Uri ReleasesPage = new($"https://github.com/{Repository}/releases/latest");
    private const string DownloadPrefix = $"https://github.com/{Repository}/releases/download/";
    // Matches AppId in Installer\MediaCenterUpdater.iss. Inno Setup adds the "_is1" suffix.
    private const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6F1B8C2E-4A3D-4E8B-9C71-2D5A0E7F3B94}_is1";
    private const long MaxInstallerBytes = 500_000_000;
    private static readonly HttpClient SharedHttp = CreateClient();
    private static readonly Lazy<bool> InstalledCopy = new(DetectInstalledCopy);
    private readonly HttpClient http;

    public SelfUpdater(HttpClient? httpClient = null) => http = httpClient ?? SharedHttp;

    /// <summary>True when this copy is the one the setup program installed, so the setup program can replace it.</summary>
    public static bool IsInstalledCopy => InstalledCopy.Value;

    /// <summary>Returns the newer release, or null when this version is current.</summary>
    public async Task<SelfUpdateInfo?> CheckAsync(string currentVersion, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Repository}/releases/latest");
        request.Headers.Accept.ParseAdd("application/vnd.github+json");
        using var response = await http.SendAsync(request, ct);
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException($"GitHub returned HTTP {(int)response.StatusCode} while checking for a new version.");
        using var release = await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
        return ParseRelease(release.RootElement, currentVersion);
    }

    internal static SelfUpdateInfo? ParseRelease(JsonElement release, string currentVersion)
    {
        if (!UpdateProviders.TryVersion(currentVersion, out var current))
            throw new InvalidOperationException("This app's own version could not be read.");
        if (Bool(release, "prerelease") || Bool(release, "draft"))
            throw new InvalidOperationException("GitHub did not return a stable release.");
        var version = Text(release, "tag_name").Trim().TrimStart('v');
        if (!UpdateProviders.TryVersion(version, out var latest))
            throw new InvalidOperationException("The latest release has a version that could not be compared.");
        if (latest <= current) return null;

        var expectedName = $"MediaCenterUpdater-{version}-Setup.exe";
        if (!release.TryGetProperty("assets", out var assets) || assets.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException("The latest release has no setup program.");
        foreach (var asset in assets.EnumerateArray())
        {
            if (Text(asset, "name") != expectedName) continue;
            var digest = Text(asset, "digest");
            if (!Regex.IsMatch(digest, "\\Asha256:[0-9a-fA-F]{64}\\z"))
                throw new InvalidOperationException("The release did not include a SHA-256 checksum, so it cannot be installed automatically.");
            if (!Uri.TryCreate(Text(asset, "browser_download_url"), UriKind.Absolute, out var url) || !IsAllowedDownload(url))
                throw new InvalidOperationException("The release's setup program has an unexpected download address.");
            return new SelfUpdateInfo(version, url, digest[7..].ToUpperInvariant());
        }
        throw new InvalidOperationException($"The latest release (version {version}) has no {expectedName}.");
    }

    internal static bool IsAllowedDownload(Uri url) =>
        url.Scheme == Uri.UriSchemeHttps && string.IsNullOrEmpty(url.UserInfo) &&
        url.AbsoluteUri.StartsWith(DownloadPrefix, StringComparison.OrdinalIgnoreCase) &&
        url.AbsolutePath.EndsWith("-Setup.exe", StringComparison.OrdinalIgnoreCase);

    /// <summary>Downloads the setup program and checks it against the release checksum. Returns its path.</summary>
    public async Task<string> DownloadAsync(SelfUpdateInfo update, string dataDirectory, CancellationToken ct)
    {
        var folder = Path.Combine(dataDirectory, "AppUpdates");
        Directory.CreateDirectory(folder);
        // Only keep the setup program for this attempt.
        foreach (var old in Directory.EnumerateFiles(folder, "*.exe"))
            try { File.Delete(old); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        var file = Path.Combine(folder, $"MediaCenterUpdater-{update.Version}-Setup.exe");
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(TimeSpan.FromMinutes(10));
        var uri = update.DownloadUrl;
        try
        {
            for (var redirects = 0; ; redirects++)
            {
                if (redirects > 5) throw new InvalidOperationException("The download redirected too many times.");
                using var request = new HttpRequestMessage(HttpMethod.Get, uri);
                using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token);
                if ((int)response.StatusCode is >= 300 and < 400)
                {
                    var location = response.Headers.Location ?? throw new InvalidOperationException("The download redirect had no destination.");
                    uri = location.IsAbsoluteUri ? location : new Uri(uri, location);
                    // GitHub serves release files from its own content host.
                    if (uri.Scheme != Uri.UriSchemeHttps || !string.IsNullOrEmpty(uri.UserInfo) ||
                        !uri.Host.EndsWith(".githubusercontent.com", StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("GitHub redirected the download to an unexpected address.");
                    continue;
                }
                if (!response.IsSuccessStatusCode)
                    throw new InvalidOperationException($"The download failed with HTTP {(int)response.StatusCode}.");
                if (response.Content.Headers.ContentLength is > MaxInstallerBytes)
                    throw new InvalidOperationException("The setup program is larger than expected.");
                await using var source = await response.Content.ReadAsStreamAsync(deadline.Token);
                await using var destination = new FileStream(file, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true);
                var buffer = new byte[81920]; long total = 0; int count;
                while ((count = await source.ReadAsync(buffer, deadline.Token)) > 0)
                {
                    total += count;
                    if (total > MaxInstallerBytes) throw new InvalidOperationException("The setup program is larger than expected.");
                    await destination.WriteAsync(buffer.AsMemory(0, count), deadline.Token);
                }
                break;
            }
            await using (var input = File.OpenRead(file))
            {
                var hash = Convert.ToHexString(await SHA256.HashDataAsync(input, ct));
                if (!hash.Equals(update.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("The downloaded setup program did not match the release checksum. It was not run.");
            }
            return file;
        }
        catch
        {
            try { File.Delete(file); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            throw;
        }
    }

    /// <summary>
    /// Starts a hidden helper that waits for this process to exit, checks the file again, then runs the
    /// setup program silently. The caller must exit right after this returns, because the setup program
    /// will not replace a running copy. The setup program reopens the app when it finishes.
    /// When the app was open on screen, the setup program shows a progress window (no questions asked)
    /// so the user can see the update happening. From the tray it runs with no window at all.
    /// </summary>
    public static void StartInstaller(string installerPath, string sha256, bool reopenInTray)
    {
        if (!Regex.IsMatch(sha256, "\\A[0-9a-fA-F]{64}\\z")) throw new ArgumentException("Invalid checksum.", nameof(sha256));
        static string Quote(string value) => "'" + value.Replace("'", "''") + "'";
        var log = Path.ChangeExtension(installerPath, ".log");
        var quietness = reopenInTray ? "/VERYSILENT" : "/SILENT";
        var script = string.Join("\n",
            "$ErrorActionPreference = 'Stop'",
            $"Wait-Process -Id {Environment.ProcessId} -Timeout 120 -ErrorAction SilentlyContinue",
            $"$file = {Quote(installerPath)}",
            // Check again right before running, in case the file changed after the app verified it.
            $"if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne {Quote(sha256.ToUpperInvariant())}) {{ exit 1 }}",
            $"Start-Process -FilePath $file -ArgumentList @('{quietness}', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/RELAUNCH={(reopenInTray ? "tray" : "window")}', {Quote("/LOG=\"" + log + "\"")})");
        var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(installerPath)!
        };
        foreach (var argument in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
            "-EncodedCommand", Convert.ToBase64String(Encoding.Unicode.GetBytes(script)) })
            start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows could not start the update.");
    }

    private static bool DetectInstalledCopy()
    {
        try
        {
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var key = hive.OpenSubKey(UninstallKey);
            if (key?.GetValue("InstallLocation") is not string location || location.Length == 0) return false;
            return string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(location)),
                Path.TrimEndingDirectorySeparator(Path.GetFullPath(AppContext.BaseDirectory)), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException or ArgumentException) { return false; }
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromSeconds(60) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaCenterUpdater/1.0");
        return client;
    }

    private static string Text(JsonElement element, string property) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
    private static bool Bool(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.True;
}
