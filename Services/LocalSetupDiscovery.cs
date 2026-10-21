using System;
using System.Globalization;
using System.IO;
using System.Security;
using System.Xml;
using System.Xml.Linq;
using Microsoft.Win32;

namespace MediaCenterUpdater;

public sealed class ApiKeyDiscoveryResult(bool found, string apiKey, string message)
{
    public bool Found { get; } = found;
    public string ApiKey { get; } = apiKey;
    public string Message { get; } = message;
    // Diagnostic output must never include the value of a discovered key.
    public override string ToString() => Message;
}

public sealed record PulsarrSetupResult(bool IsSupported, string Message);

/// <summary>Reads this computer's existing setup without contacting or changing any application.</summary>
public static class LocalSetupDiscovery
{
    private const string PulsarrInstallerKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{B8A42C5E-7D91-4F8C-B5E3-9A7C6D8E2F10}_is1";
    private const long MaximumConfigLength = 1_000_000;

    public static ApiKeyDiscoveryResult FindApiKey(ApplicationId id, string serverUrl, string? dataRoot = null)
    {
        if (id is not (ApplicationId.Radarr or ApplicationId.Sonarr))
            return Missing("Automatic key retrieval is available for Radarr and Sonarr.");
        var name = ApplicationCatalog.Name(id);
        if (!Uri.TryCreate(serverUrl.Trim(), UriKind.Absolute, out var address) ||
            (address.Scheme != Uri.UriSchemeHttp && address.Scheme != Uri.UriSchemeHttps) ||
            !string.IsNullOrEmpty(address.UserInfo) || !string.IsNullOrEmpty(address.Query) || !string.IsNullOrEmpty(address.Fragment))
            return Missing("Enter a valid HTTP or HTTPS address for " + name + ", then try retrieving its key again.");
        if (!address.IsLoopback && !address.DnsSafeHost.Equals(Environment.MachineName, StringComparison.OrdinalIgnoreCase))
            return Missing(name + " uses another computer's address. Automatic retrieval only reads keys on this PC; enter that application's key yourself.");

        try
        {
            var path = Path.Combine(dataRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), name, "config.xml");
            if (!File.Exists(path))
                return Missing(name + "'s settings were not found in the usual location on this PC. Start " + name + " and try again, or enter its key yourself.");
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (stream.Length > MaximumConfigLength)
                return Missing(name + "'s settings file could not be read safely. Enter its key yourself.");
            using var reader = XmlReader.Create(stream, new XmlReaderSettings
            {
                DtdProcessing = DtdProcessing.Prohibit,
                XmlResolver = null,
                MaxCharactersInDocument = MaximumConfigLength
            });
            var config = XDocument.Load(reader).Root;
            if (config is null || !EndpointMatches(id, address, config))
                return Missing("The " + name + " address does not match the copy installed on this PC. Check its address or enter its key yourself.");
            var key = config.Element("ApiKey")?.Value.Trim() ?? "";
            if (key.Length == 0)
                return Missing(name + "'s settings were found, but they do not contain an API key. Start " + name + " and try again, or copy its key from Settings > General > Security.");
            if (key.Length > 1024 || HasControlCharacters(key))
                return Missing(name + "'s saved API key could not be read safely. Copy its key from Settings > General > Security.");
            return new(true, key, name + "'s API key was found in its settings on this PC.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SecurityException or XmlException or ArgumentException or NotSupportedException)
        {
            // XML and filesystem exception text may contain configuration values or private paths.
            return Missing("Could not read " + name + "'s settings on this PC. Try retrieving its key again, or copy it from Settings > General > Security.");
        }
    }

    private static bool EndpointMatches(ApplicationId id, Uri address, XElement config)
    {
        var useSsl = address.Scheme == Uri.UriSchemeHttps;
        if (useSsl && !string.Equals(config.Element("EnableSsl")?.Value.Trim(), "true", StringComparison.OrdinalIgnoreCase)) return false;
        var configuredPort = config.Element(useSsl ? "SslPort" : "Port")?.Value.Trim();
        var defaultPort = id == ApplicationId.Radarr ? (useSsl ? 9898 : 7878) : (useSsl ? 9898 : 8989);
        var port = defaultPort;
        if (!string.IsNullOrEmpty(configuredPort) &&
            (!int.TryParse(configuredPort, NumberStyles.None, CultureInfo.InvariantCulture, out port) || port is < 1 or > 65535)) return false;
        var urlBase = (config.Element("UrlBase")?.Value.Trim() ?? "").Trim('/');
        return address.Port == port && address.AbsolutePath.Trim('/').Equals(urlBase, StringComparison.Ordinal);
    }

    public static PulsarrSetupResult CheckPulsarrSetup()
    {
        try
        {
            foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
            foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var entry = root.OpenSubKey(PulsarrInstallerKey);
                if (entry is null) continue;
                var directory = (entry.GetValue("InstallLocation") as string ?? "").Trim('"');
                if (string.IsNullOrEmpty(directory))
                {
                    var uninstall = ExecutableFromCommand(entry.GetValue("UninstallString") as string ?? "");
                    if (!string.IsNullOrEmpty(uninstall)) directory = Path.GetDirectoryName(uninstall) ?? "";
                }
                var executable = Path.IsPathFullyQualified(directory) ? Path.Combine(directory, "pulsarr-service.exe") : "";
                using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\pulsarr");
                return EvaluatePulsarrSetup(true, executable, File.Exists(executable), service is not null,
                    service?.GetValue("ImagePath") as string, entry.GetValue("Inno Setup: Selected Tasks") as string);
            }
            return EvaluatePulsarrSetup(false, "", false, false, null, null);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SecurityException or ArgumentException or NotSupportedException)
        {
            return new(false, "Pulsarr's installation could not be checked on this PC. Try again before enabling its automatic updates.");
        }
    }

    internal static PulsarrSetupResult EvaluatePulsarrSetup(bool registered, string expectedExecutable, bool executableExists,
        bool serviceExists, string? serviceCommand, string? selectedTasks)
    {
        if (!registered)
            return new(false, "Pulsarr's official Windows installation was not found on this PC. Automatic updates require its Windows installer with the service option selected. Copies started by hand or through Docker need a different update method.");
        if (!executableExists)
            return new(false, "Pulsarr's Windows installation was found, but its background service program is missing. Run its official Windows installer with the service option selected before using automatic updates.");
        if (!serviceExists)
            return new(false, "Pulsarr is installed, but its Windows service was not found. Run its official Windows installer with the service option selected before using automatic updates.");
        if (!SameExecutable(expectedExecutable, ExecutableFromCommand(serviceCommand ?? "")))
            return new(false, "Pulsarr's Windows service points to a different or unrecognized copy. Its setup needs to be corrected with the official Windows installer before automatic updates can be used.");
        if (selectedTasks is null)
            return new(false, "Pulsarr's Windows service was found, but its saved installation choices could not be read. Run its official Windows installer once to save those choices before using automatic updates.");
        return new(true, "This PC already meets the requirement: Pulsarr is installed using its official Windows installer and is set up as a Windows service. Its saved installation choices were found, so no reinstall is needed for automatic updates.");
    }

    private static string ExecutableFromCommand(string command)
    {
        command = command.Trim();
        if (command.StartsWith('"'))
        {
            var closingQuote = command.IndexOf('"', 1);
            return closingQuote > 1 ? command[1..closingQuote] : "";
        }
        var end = command.IndexOf(".exe", StringComparison.OrdinalIgnoreCase);
        return end >= 0 && (end + 4 == command.Length || char.IsWhiteSpace(command[end + 4])) ? command[..(end + 4)] : "";
    }

    private static bool SameExecutable(string expected, string actual)
    {
        try
        {
            return Path.IsPathFullyQualified(expected) && Path.IsPathFullyQualified(actual) &&
                Path.GetFullPath(expected).Equals(Path.GetFullPath(actual), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    private static bool HasControlCharacters(string text)
    {
        foreach (var character in text) if (char.IsControl(character)) return true;
        return false;
    }

    private static ApiKeyDiscoveryResult Missing(string message) => new(false, "", message);
}
