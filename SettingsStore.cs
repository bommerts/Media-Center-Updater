using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace MediaCenterUpdater;

public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly object gate = new();
    public string DataDirectory { get; }
    public string? LoadWarning { get; private set; }
    public SettingsStore(string? dataDirectory = null)
    {
        DataDirectory = dataDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaCenterUpdater");
        Directory.CreateDirectory(DataDirectory);
    }

    public AppSettings Load()
    {
        var path = Path.Combine(DataDirectory, "settings.json");
        if (!File.Exists(path)) return new();
        try
        {
            var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path)) ?? throw new InvalidDataException("Settings were empty.");
            settings.Programs ??= new();
            settings.Programs = settings.Programs.Where(p => p is not null && Enum.IsDefined(p.Id)).GroupBy(p => p.Id).Select(g => g.First()).ToList();
            foreach (var program in new AppSettings().Programs)
                if (!settings.Programs.Any(p => p.Id == program.Id)) settings.Programs.Add(program);
            settings.IntervalHours = Math.Clamp(settings.IntervalHours, 1, 168);
            settings.DailyHour = Math.Clamp(settings.DailyHour, 0, 23);
            settings.DailyMinute = Math.Clamp(settings.DailyMinute, 0, 59);
            if (!Enum.IsDefined(settings.ScheduleMode)) settings.ScheduleMode = ScheduleMode.Interval;
            if (settings.Theme is not ("Dark" or "Light" or "System")) settings.Theme = "Dark";
            foreach (var program in settings.Programs)
            {
                program.ApiKey ??= "";
                program.ServerUrl ??= "";
                if (program.ApiKey.Length == 0) continue;
                try
                {
                    if (!program.ApiKey.StartsWith("dpapi:", StringComparison.Ordinal)) throw new CryptographicException();
                    program.ApiKey = Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(program.ApiKey[6..]), null, DataProtectionScope.CurrentUser));
                }
                catch (Exception ex) when (ex is CryptographicException or FormatException)
                {
                    program.ApiKey = "";
                    settings.ScheduleEnabled = false;
                    LoadWarning = "A saved API key could not be read. Re-enter the key and enable the schedule again.";
                }
            }
            return settings;
        }
        catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException or InvalidDataException)
        {
            // Preserve the original file for diagnosis; never silently resume an unknown schedule.
            LoadWarning = "Saved settings could not be loaded. The schedule is off. Review and save your settings.";
            return new();
        }
    }

    public void Save(AppSettings settings)
    {
        lock (gate)
        {
            var copy = JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(settings))!;
            foreach (var program in copy.Programs)
                if (!string.IsNullOrEmpty(program.ApiKey))
                    program.ApiKey = "dpapi:" + Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(program.ApiKey), null, DataProtectionScope.CurrentUser));
            var path = Path.Combine(DataDirectory, "settings.json");
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(copy, JsonOptions));
            File.Move(temporary, path, true);
        }
    }

    public void Log(ActivityEntry entry)
    {
        lock (gate)
        {
            var path = Path.Combine(DataDirectory, "activity.jsonl");
            if (File.Exists(path) && new FileInfo(path).Length > 2_000_000)
                File.Move(path, Path.Combine(DataDirectory, "activity.previous.jsonl"), true);
            File.AppendAllText(path, JsonSerializer.Serialize(entry) + Environment.NewLine);
        }
    }

    public IEnumerable<ActivityEntry> ReadRecentActivity()
    {
        var path = Path.Combine(DataDirectory, "activity.jsonl");
        if (!File.Exists(path)) return Array.Empty<ActivityEntry>();
        try
        {
            var entries = new List<ActivityEntry>();
            foreach (var line in File.ReadLines(path).TakeLast(100))
            {
                try { if (JsonSerializer.Deserialize<ActivityEntry>(line) is { } entry) entries.Add(entry); }
                catch (JsonException) { }
            }
            return entries.AsEnumerable().Reverse().ToArray();
        }
        catch (IOException) { return Array.Empty<ActivityEntry>(); }
    }
}
