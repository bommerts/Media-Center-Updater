using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace MediaCenterUpdater;

/// <summary>Read-only checks of whether an app is busy, so an update can wait instead of interrupting it.</summary>
public sealed partial class UpdateProviders
{
    // Frequent, short housekeeping that Radarr and Sonarr run on their own. These do not count as "in use".
    private static readonly HashSet<string> RoutineArrCommands = new(StringComparer.OrdinalIgnoreCase)
    {
        "RefreshMonitoredDownloads", "CheckHealth", "MessagingCleanup", "Housekeeping", "RssSync",
        "ApplicationUpdate", "ApplicationUpdateCheck", "ImportListSync", "CleanUpRecycleBin", "Backup"
    };
    private static readonly HashSet<string> SabnzbdBusyHistory = new(StringComparer.OrdinalIgnoreCase)
    {
        "Queued", "QuickCheck", "Verifying", "Repairing", "Fetching", "Extracting", "Moving", "Running"
    };

    internal Uri PlexAddress { get; init; } = new("http://127.0.0.1:32400/");
    internal string? SabnzbdConfigOverride { get; init; }
    internal string? PulsarrDataOverride { get; init; }

    public async Task<UsageResult> CheckUsageAsync(ProgramSettings settings, CancellationToken cancellationToken)
    {
        var name = ApplicationCatalog.Name(settings.Id);
        try
        {
            return settings.Id switch
            {
                ApplicationId.Plex => await CheckPlexUsageAsync(cancellationToken),
                ApplicationId.Sabnzbd => await CheckSabnzbdUsageAsync(cancellationToken),
                ApplicationId.Radarr or ApplicationId.Sonarr => await CheckArrUsageAsync(settings, cancellationToken),
                _ => await CheckPulsarrUsageAsync(settings, cancellationToken)
            };
        }
        catch (HttpRequestException ex) when (ex.HttpRequestError == HttpRequestError.ConnectionError)
        {
            // Nothing is listening, so the app is not running and nothing can be interrupted.
            return new(UsageState.Idle, $"{name} is not running.");
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception ex)
        {
            var reason = ex is ProviderException ? " " + ex.Message : "";
            return new(UsageState.Unknown, $"Could not check whether {name} is in use, so the update was not installed.{reason}");
        }
    }

    private async Task<UsageResult> CheckPlexUsageAsync(CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(PlexAddress, "status/sessions"));
        request.Headers.Accept.ParseAdd("application/json");
        var token = ReadPlexToken();
        if (!string.IsNullOrEmpty(token)) request.Headers.Add("X-Plex-Token", token);
        using var response = await Http.SendAsync(request, ct);
        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden)
            throw new ProviderException("Plex did not allow the check. Make sure Plex Media Server is signed in on this PC under the same Windows account.");
        RequireSuccess(response);
        using var body = await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
        var streams = CountPlexSessions(body.RootElement);
        return streams > 0
            ? new(UsageState.InUse, streams == 1 ? "Plex is streaming 1 item." : $"Plex is streaming {streams} items.")
            : new(UsageState.Idle, "Plex has no active streams.");
    }

    internal static int CountPlexSessions(JsonElement root)
    {
        if (!root.TryGetProperty("MediaContainer", out var container)) throw new ProviderException("Plex returned an unexpected response.");
        if (container.TryGetProperty("size", out var size) && size.TryGetInt32(out var count)) return count;
        return container.TryGetProperty("Metadata", out var items) && items.ValueKind == JsonValueKind.Array ? items.GetArrayLength() : 0;
    }

    private static string? ReadPlexToken()
    {
        // Plex keeps its server token in the registry of the Windows account it runs under.
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Plex, Inc.\Plex Media Server");
        return key?.GetValue("PlexOnlineToken") as string;
    }

    private async Task<UsageResult> CheckSabnzbdUsageAsync(CancellationToken ct)
    {
        var path = SabnzbdConfigOverride ?? FindSabnzbdConfig();
        if (path is null || !File.Exists(path))
            throw new ProviderException("SABnzbd's settings file (sabnzbd.ini) was not found.");
        var config = ReadSabnzbdConfig(File.ReadAllText(path));
        if (config.ApiKey.Length == 0) throw new ProviderException("SABnzbd's settings file has no API key.");

        using var queue = await GetJsonAsync(SabnzbdApi(config, "queue"), null, ct);
        var q = queue.RootElement.GetProperty("queue");
        var paused = q.TryGetProperty("paused", out var p) && p.ValueKind == JsonValueKind.True;
        var slots = q.TryGetProperty("noofslots_total", out var n) && n.TryGetInt32(out var total) ? total
            : q.TryGetProperty("noofslots", out n) && n.TryGetInt32(out total) ? total : 0;
        if (!paused && slots > 0)
            return new(UsageState.InUse, slots == 1 ? "SABnzbd is downloading 1 item." : $"SABnzbd is downloading {slots} items.");

        using var history = await GetJsonAsync(SabnzbdApi(config, "history"), null, ct);
        var processing = history.RootElement.GetProperty("history").TryGetProperty("slots", out var items) && items.ValueKind == JsonValueKind.Array
            ? items.EnumerateArray().Count(x => SabnzbdBusyHistory.Contains(Text(x, "status"))) : 0;
        return processing > 0
            ? new(UsageState.InUse, "SABnzbd is checking or unpacking a finished download.")
            : new(UsageState.Idle, "SABnzbd has no active downloads.");
    }

    internal sealed record SabnzbdConfig(string Host, int Port, bool Https, string UrlBase, string ApiKey);

    internal static SabnzbdConfig ReadSabnzbdConfig(string ini)
    {
        // sabnzbd.ini is a ConfigObj file. Read the top-level [misc] section only.
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var inMisc = false;
        foreach (var raw in ini.Split('\n'))
        {
            var line = raw.Trim();
            if (line.StartsWith('['))
            {
                inMisc = line.Equals("[misc]", StringComparison.OrdinalIgnoreCase);
                continue;
            }
            if (!inMisc || line.Length == 0 || line.StartsWith('#')) continue;
            var equals = line.IndexOf('=');
            if (equals > 0) values[line[..equals].Trim()] = line[(equals + 1)..].Trim().Trim('"');
        }
        string Get(string key, string fallback) => values.TryGetValue(key, out var v) && v.Length > 0 ? v : fallback;
        var host = Get("host", "127.0.0.1");
        if (host is "0.0.0.0" or "::" or "[::]" or "localhost" or "") host = "127.0.0.1";
        var https = Get("enable_https", "0") == "1";
        var httpsPort = Get("https_port", "");
        // With HTTPS on and no separate HTTPS port, the main port serves HTTPS only.
        var useHttps = https && httpsPort.Length == 0;
        if (!int.TryParse(Get("port", "8080"), out var port) || port is < 1 or > 65535) port = 8080;
        var urlBase = "/" + Get("url_base", "/sabnzbd").Trim('/');
        return new(host, port, useHttps, urlBase, Get("api_key", ""));
    }

    private static Uri SabnzbdApi(SabnzbdConfig config, string mode)
    {
        var builder = new UriBuilder(config.Https ? "https" : "http", config.Host, config.Port, config.UrlBase.TrimEnd('/') + "/api")
        {
            Query = $"mode={mode}&output=json&limit=20&apikey={Uri.EscapeDataString(config.ApiKey)}"
        };
        return builder.Uri;
    }

    private static string? FindSabnzbdConfig()
    {
        // A SABnzbd Windows service is started with "-f <path to sabnzbd.ini>".
        using (var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\SABnzbd\Parameters"))
        {
            var parameters = service?.GetValue("AppParameters") as string;
            if (!string.IsNullOrWhiteSpace(parameters))
            {
                var args = SplitCommandLine("sabnzbd " + parameters);
                for (var i = 1; i < args.Length - 1; i++)
                    if (args[i] is "-f" or "--config-file" && Path.IsPathFullyQualified(args[i + 1])) return args[i + 1];
            }
        }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "sabnzbd", "sabnzbd.ini");
    }

    private async Task<UsageResult> CheckArrUsageAsync(ProgramSettings settings, CancellationToken ct)
    {
        var name = ApplicationCatalog.Name(settings.Id);
        var connection = GetArrConnection(settings);
        if (string.IsNullOrWhiteSpace(connection.Key)) throw new ProviderException($"{name}'s API key is missing.");
        using var commands = await GetJsonAsync(Endpoint(connection.Url, "api/v3/command"), connection.Key, ct);
        var running = commands.RootElement.EnumerateArray()
            .Where(c => Text(c, "status").Equals("started", StringComparison.OrdinalIgnoreCase) && !RoutineArrCommands.Contains(Text(c, "name")))
            .Select(c => Text(c, "commandName") is { Length: > 0 } label ? label : Text(c, "name"))
            .Distinct().ToArray();
        return running.Length > 0
            ? new(UsageState.InUse, $"{name} is running: {string.Join(", ", running)}.")
            : new(UsageState.Idle, $"{name} is not running any tasks.");
    }

    private async Task<UsageResult> CheckPulsarrUsageAsync(ProgramSettings settings, CancellationToken ct)
    {
        var (port, basePath) = ReadPulsarrAddress(PulsarrDataOverride ?? LocalSetupDiscovery.PulsarrDataDirectory);
        var uri = new UriBuilder("http", "127.0.0.1", port, basePath.TrimEnd('/') + "/v1/watchlist-workflow/status").Uri;
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        if (!string.IsNullOrWhiteSpace(settings.ApiKey)) request.Headers.Add("X-API-Key", settings.ApiKey.Trim());
        using var response = await Http.SendAsync(request, ct);
        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden)
            throw new ProviderException("Pulsarr turned the check away because it needs an API key. In Settings under Wait while apps are in use, choose Find key, or paste a key from Pulsarr's Utilities → API Keys.");
        RequireSuccess(response);
        using var body = await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
        var status = Text(body.RootElement, "status");
        return status is "starting" or "stopping"
            ? new(UsageState.InUse, "Pulsarr is syncing watchlists.")
            : new(UsageState.Idle, "Pulsarr is not syncing.");
    }

    internal static (int Port, string BasePath) ReadPulsarrAddress(string dataDirectory)
    {
        var values = LocalSetupDiscovery.ReadPulsarrEnvironment(dataDirectory);
        // listenPort is what the server binds to; port is the public port and the usual fallback.
        var port = 3003;
        foreach (var key in new[] { "listenPort", "port" })
            if (values.TryGetValue(key, out var text) && int.TryParse(text, out var parsed) && parsed is > 0 and < 65536) { port = parsed; break; }
        var basePath = values.TryGetValue("basePath", out var path) && path.Length > 0 ? "/" + path.Trim('/') : "";
        return (port, basePath);
    }
}
