using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;
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

    public static string PulsarrDataDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Pulsarr");

    /// <summary>Reads Pulsarr's .env file. Missing files give no values.</summary>
    internal static Dictionary<string, string> ReadPulsarrEnvironment(string dataDirectory)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var env = Path.Combine(dataDirectory, ".env");
        if (!File.Exists(env)) return values;
        foreach (var raw in File.ReadLines(env))
        {
            var line = raw.Trim();
            if (line.StartsWith('#')) continue;
            var equals = line.IndexOf('=');
            if (equals > 0) values[line[..equals].Trim()] = line[(equals + 1)..].Trim().Trim('"', '\'');
        }
        return values;
    }

    /// <summary>
    /// True when Pulsarr turns away requests from this PC that have no API key. Pulsarr requires sign-in
    /// unless its .env sets authenticationMethod to "disabled" or "requiredExceptLocal".
    /// </summary>
    public static bool PulsarrRequiresApiKey(string? dataDirectory = null)
    {
        try
        {
            ReadPulsarrEnvironment(dataDirectory ?? PulsarrDataDirectory).TryGetValue("authenticationMethod", out var method);
            return method is not ("disabled" or "requiredExceptLocal");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SecurityException) { return true; }
    }

    /// <summary>Reads an active API key from Pulsarr's own database on this PC. Never changes the database.</summary>
    public static ApiKeyDiscoveryResult FindPulsarrApiKey(string? dataDirectory = null)
    {
        const string createKey = "In Pulsarr, open Utilities → API Keys and generate a key, then choose Find key again.";
        var directory = dataDirectory ?? PulsarrDataDirectory;
        try
        {
            if (!Directory.Exists(directory))
                return Missing("Pulsarr's data folder was not found on this PC. Start Pulsarr and try again, or paste a key from Utilities → API Keys.");
            var env = ReadPulsarrEnvironment(directory);
            if (env.TryGetValue("dbType", out var type) && type.Equals("postgres", StringComparison.OrdinalIgnoreCase))
                return Missing("Pulsarr keeps its settings in PostgreSQL, so its key cannot be read automatically. Paste a key from Pulsarr's Utilities → API Keys.");
            var database = env.TryGetValue("dbPath", out var configured) && Path.IsPathFullyQualified(configured)
                ? configured : Path.Combine(directory, "db", "pulsarr.db");
            if (!File.Exists(database))
                return Missing("Pulsarr's database was not found on this PC. Start Pulsarr and try again, or paste a key from Utilities → API Keys.");
            var key = PulsarrDatabase.ReadActiveApiKey(database)?.Trim() ?? "";
            if (key.Length == 0)
                return Missing("Pulsarr does not have an API key yet. " + createKey);
            if (key.Length > 1024 || HasControlCharacters(key))
                return Missing("Pulsarr's saved API key could not be read safely. Paste a key from Utilities → API Keys.");
            return new(true, key, "Pulsarr's API key was found on this PC and saved securely.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SecurityException or InvalidDataException
            or DllNotFoundException or EntryPointNotFoundException)
        {
            // Database errors can include file paths, so only a general message is shown.
            return Missing("Could not read Pulsarr's database on this PC. Try Find key again, or paste a key from Utilities → API Keys.");
        }
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

/// <summary>Read-only access to Pulsarr's SQLite database through the SQLite library built into Windows.</summary>
internal static class PulsarrDatabase
{
    private const string Library = "winsqlite3.dll";
    private const int OpenReadOnly = 0x1, Ok = 0, Row = 100, Done = 101;
    // Pulsarr only accepts active keys that belong to an admin account.
    private const string Query = "SELECT k.key FROM api_keys k JOIN admin_users u ON u.id = k.user_id " +
        "WHERE k.is_active = 1 ORDER BY k.created_at DESC, k.id DESC LIMIT 1";

    public static string? ReadActiveApiKey(string databasePath)
    {
        var handle = IntPtr.Zero;
        var statement = IntPtr.Zero;
        try
        {
            if (sqlite3_open_v2(Utf8(databasePath), out handle, OpenReadOnly, IntPtr.Zero) != Ok)
                throw new InvalidDataException("The database could not be opened.");
            sqlite3_busy_timeout(handle, 3000);
            if (sqlite3_prepare_v2(handle, Utf8(Query), -1, out statement, IntPtr.Zero) != Ok)
                throw new InvalidDataException("The database is not in the expected format.");
            var step = sqlite3_step(statement);
            if (step == Done) return null;
            if (step != Row) throw new InvalidDataException("The database could not be read.");
            var text = sqlite3_column_text(statement, 0);
            return text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text, sqlite3_column_bytes(statement, 0));
        }
        finally
        {
            if (statement != IntPtr.Zero) sqlite3_finalize(statement);
            if (handle != IntPtr.Zero) sqlite3_close_v2(handle);
        }
    }

    private static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text + "\0");

    [DllImport(Library)] private static extern int sqlite3_open_v2(byte[] filename, out IntPtr database, int flags, IntPtr vfs);
    [DllImport(Library)] private static extern int sqlite3_busy_timeout(IntPtr database, int milliseconds);
    [DllImport(Library)] private static extern int sqlite3_prepare_v2(IntPtr database, byte[] sql, int length, out IntPtr statement, IntPtr tail);
    [DllImport(Library)] private static extern int sqlite3_step(IntPtr statement);
    [DllImport(Library)] private static extern IntPtr sqlite3_column_text(IntPtr statement, int column);
    [DllImport(Library)] private static extern int sqlite3_column_bytes(IntPtr statement, int column);
    [DllImport(Library)] private static extern int sqlite3_finalize(IntPtr statement);
    [DllImport(Library)] private static extern int sqlite3_close_v2(IntPtr database);
}
