using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace MediaCenterUpdater;

public sealed class UpdateCoordinator
{
    private readonly AppSettings settings;
    private readonly SettingsStore store;
    private readonly IUpdateProvider provider;
    private readonly SemaphoreSlim gate = new(1, 1);
    public bool IsBusy { get; private set; }
    /// <summary>How soon a scheduled run tries again after an update waited because its app was in use.</summary>
    public static readonly TimeSpan InUseRetryDelay = TimeSpan.FromMinutes(30);
    public event Action<UpdateResult>? ResultChanged;
    public event Action<ActivityEntry>? Activity;
    public event Action<bool>? BusyChanged;
    public event Action<string>? SummaryChanged;

    public UpdateCoordinator(AppSettings settings, SettingsStore store, IUpdateProvider provider)
    {
        this.settings = settings;
        this.store = store;
        this.provider = provider;
        if (settings.ScheduleEnabled && settings.NextRun is null) RecalculateSchedule();
    }

    public void RecalculateSchedule()
    {
        settings.NextRun = settings.ScheduleEnabled ? ComputeNextRun(settings, DateTimeOffset.Now) : null;
        store.Save(settings);
    }

    public static DateTimeOffset ComputeNextRun(AppSettings settings, DateTimeOffset now)
    {
        if (settings.ScheduleMode == ScheduleMode.Interval)
            return now.AddHours(Math.Clamp(settings.IntervalHours, 1, 168));
        var zone = TimeZoneInfo.Local;
        var localNow = TimeZoneInfo.ConvertTime(now, zone);
        var candidate = localNow.Date.AddHours(Math.Clamp(settings.DailyHour, 0, 23)).AddMinutes(Math.Clamp(settings.DailyMinute, 0, 59));
        if (candidate <= localNow.DateTime) candidate = candidate.AddDays(1);
        // Spring-forward schedules run at the next valid local minute.
        while (zone.IsInvalidTime(candidate)) candidate = candidate.AddMinutes(1);
        var offsets = zone.IsAmbiguousTime(candidate) ? zone.GetAmbiguousTimeOffsets(candidate) : new[] { zone.GetUtcOffset(candidate) };
        var occurrences = offsets.Select(offset => new DateTimeOffset(candidate, offset)).Where(value => value > now).OrderBy(value => value).ToArray();
        if (occurrences.Length > 0) return occurrences[0];
        candidate = candidate.AddDays(1);
        while (zone.IsInvalidTime(candidate)) candidate = candidate.AddMinutes(1);
        return new DateTimeOffset(candidate, zone.GetUtcOffset(candidate));
    }

    public async Task EvaluateScheduleAsync()
    {
        if (!settings.ScheduleEnabled || IsBusy) return;
        if (settings.NextRun is null) { RecalculateSchedule(); return; }
        if (settings.NextRun > DateTimeOffset.Now) return;
        // Persist the next due time before running. A restart or a slow installer cannot create a scan storm.
        settings.LastScheduledRun = DateTimeOffset.Now;
        settings.NextRun = ComputeNextRun(settings, DateTimeOffset.Now);
        store.Save(settings);
        var waiting = await RunAsync(true);
        if (waiting > 0 && settings.ScheduleEnabled)
        {
            // Try again soon instead of waiting a full interval or a whole day.
            var retry = DateTimeOffset.Now.Add(InUseRetryDelay);
            if (settings.NextRun is null || retry < settings.NextRun)
            {
                settings.NextRun = retry;
                store.Save(settings);
            }
        }
    }

    /// <summary>Returns how many updates waited because their app was in use.</summary>
    public async Task<int> RunAsync(bool installUpdates)
    {
        if (!await gate.WaitAsync(0)) return 0;
        IsBusy = true;
        BusyChanged?.Invoke(true);
        var updated = 0;
        var available = 0;
        var needsAttention = 0;
        var waiting = 0;
        try
        {
            // Work from a snapshot so changes made in the form only affect the next run.
            var programs = settings.Programs.Where(p => p.Enabled).Select(p => new ProgramSettings
            { Id = p.Id, Enabled = p.Enabled, AutoInstall = p.AutoInstall, ServerUrl = p.ServerUrl, ApiKey = p.ApiKey, WaitWhileInUse = p.WaitWhileInUse }).ToArray();
            if (programs.Length == 0) { SummaryChanged?.Invoke("Turn on monitoring for at least one program."); return 0; }
            Write("Updater", installUpdates ? "Scan and automatic updates started." : "Update check started. No updates will be installed during this check.");
            foreach (var program in programs)
            {
                var name = ApplicationCatalog.Name(program.Id);
                SummaryChanged?.Invoke($"Checking {name}...");
                ResultChanged?.Invoke(new(program.Id, UpdateState.Checking, Message: "Checking for updates..."));
                try
                {
                    using var checkTimeout = new CancellationTokenSource(TimeSpan.FromMinutes(2));
                    var result = await provider.CheckAsync(program, checkTimeout.Token);
                    ResultChanged?.Invoke(result);
                    Write(name, result.Message, result.State is UpdateState.Error);
                    if (result.State == UpdateState.UpdateAvailable)
                    {
                        var usage = installUpdates && program.AutoInstall && program.WaitWhileInUse
                            ? await CheckUsageAsync(program) : null;
                        if (usage is { State: not UsageState.Idle })
                        {
                            var waitMessage = usage.State == UsageState.InUse
                                ? $"Update waiting: {usage.Message} It will be installed when {name} is not in use."
                                : $"Update waiting: {usage.Message}";
                            ResultChanged?.Invoke(result with { Message = waitMessage });
                            Write(name, waitMessage, usage.State == UsageState.Unknown);
                            if (usage.State == UsageState.InUse) waiting++; else needsAttention++;
                        }
                        else if (installUpdates && program.AutoInstall)
                        {
                            SummaryChanged?.Invoke($"Updating {name}...");
                            ResultChanged?.Invoke(result with { State = UpdateState.Installing, Message = "Installing update. Keep the updater running." });
                            // Do not cancel or terminate a running installer just because the user hides the window.
                            using var installTimeout = new CancellationTokenSource(TimeSpan.FromMinutes(30));
                            result = await provider.InstallAsync(program, result, installTimeout.Token);
                            ResultChanged?.Invoke(result);
                            Write(name, result.Message, result.State is UpdateState.Error);
                            if (result.State is UpdateState.Updated or UpdateState.RestartRequired) updated++;
                            else needsAttention++;
                        }
                        else available++;
                    }
                    else if (result.State is UpdateState.Error or UpdateState.NeedsSetup) needsAttention++;
                }
                catch (Exception ex)
                {
                    // Provider errors can contain endpoint credentials. Show a safe category only.
                    var message = ex is OperationCanceledException ? "The operation timed out. Check the program before retrying." : "The operation failed. Check the connection and program settings, then try again.";
                    ResultChanged?.Invoke(new(program.Id, UpdateState.Error, Message: message));
                    Write(name, message, true);
                    needsAttention++;
                }
            }
            var summary = $"Finished: {updated} updated, {available} updates available, " +
                (waiting > 0 ? $"{waiting} waiting until the app is not in use, " : "") + $"{needsAttention} need attention.";
            SummaryChanged?.Invoke(summary);
            Write("Updater", summary);
            return waiting;
        }
        finally
        {
            IsBusy = false;
            gate.Release();
            BusyChanged?.Invoke(false);
        }
    }

    private async Task<UsageResult> CheckUsageAsync(ProgramSettings program)
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            return await provider.CheckUsageAsync(program, timeout.Token);
        }
        catch (Exception)
        {
            // Never install when the busy check itself failed; the user asked to wait.
            return new(UsageState.Unknown, $"Could not check whether {ApplicationCatalog.Name(program.Id)} is in use, so the update was not installed.");
        }
    }

    private void Write(string application, string message, bool error = false)
    {
        var entry = new ActivityEntry(DateTimeOffset.Now, application, message, error);
        try { store.Log(entry); }
        catch (Exception ex) when (ex is System.IO.IOException or UnauthorizedAccessException) { }
        Activity?.Invoke(entry);
    }
}
