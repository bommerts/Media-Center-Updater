using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace MediaCenterUpdater;

public enum ApplicationId { Plex, Sabnzbd, Radarr, Sonarr, Pulsarr }
public enum UpdateState { NotChecked, Checking, UpToDate, UpdateAvailable, Installing, Updated, NotInstalled, NeedsSetup, Error, RestartRequired }
public enum ScheduleMode { Interval, Daily }

public sealed class ProgramSettings
{
    public ApplicationId Id { get; set; }
    public bool Enabled { get; set; } = true;
    public bool AutoInstall { get; set; } = true;
    public string ServerUrl { get; set; } = "";
    public string ApiKey { get; set; } = "";
}

public sealed class AppSettings
{
    public string Theme { get; set; } = "Dark";
    public bool MinimizeToTray { get; set; } = true;
    public bool StartMinimized { get; set; }
    public bool ScheduleEnabled { get; set; }
    public ScheduleMode ScheduleMode { get; set; }
    public int IntervalHours { get; set; } = 6;
    public int DailyHour { get; set; } = 3;
    public int DailyMinute { get; set; }
    public DateTimeOffset? LastScheduledRun { get; set; }
    public DateTimeOffset? NextRun { get; set; }
    // Updates to Media Center Updater itself. Only used by copies installed with the setup program.
    public bool AppUpdateCheck { get; set; } = true;
    public bool AppUpdateInstall { get; set; }
    public DateTimeOffset? LastAppUpdateCheck { get; set; }
    public List<ProgramSettings> Programs { get; set; } = new()
    {
        new() { Id = ApplicationId.Plex },
        new() { Id = ApplicationId.Sabnzbd },
        new() { Id = ApplicationId.Radarr, ServerUrl = "http://localhost:7878" },
        new() { Id = ApplicationId.Sonarr, ServerUrl = "http://localhost:8989" },
        new() { Id = ApplicationId.Pulsarr }
    };
}

public sealed record UpdateResult(ApplicationId Id, UpdateState State, string InstalledVersion = "", string AvailableVersion = "", string Message = "", string? DownloadUrl = null, string? Sha256 = null);
public sealed record ActivityEntry(DateTimeOffset Time, string Application, string Message, bool IsError = false);

public interface IUpdateProvider
{
    Task<UpdateResult> CheckAsync(ProgramSettings settings, CancellationToken cancellationToken);
    Task<UpdateResult> InstallAsync(ProgramSettings settings, UpdateResult update, CancellationToken cancellationToken);
}

public static class ApplicationCatalog
{
    public static string Name(ApplicationId id) => id switch
    {
        ApplicationId.Plex => "Plex Media Server", ApplicationId.Sabnzbd => "SABnzbd", ApplicationId.Radarr => "Radarr", ApplicationId.Sonarr => "Sonarr", _ => "Pulsarr"
    };
    public static string Description(ApplicationId id) => id switch
    {
        ApplicationId.Plex => "Your media library, always ready.", ApplicationId.Sabnzbd => "Keep your download manager current.", ApplicationId.Radarr => "Movie collection management.", ApplicationId.Sonarr => "TV series collection management.", _ => "Plex watchlist automation."
    };
}
