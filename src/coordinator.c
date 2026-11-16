#include "coordinator.h"

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

void coordinator_init(Coordinator *coordinator, AppSettings *settings, SettingsStore *store, UpdateProvider provider)
{
    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->settings = settings;
    coordinator->store = store;
    coordinator->provider = provider;
    if (settings->schedule_enabled && !settings->next_run.has)
    {
        wchar_t error[MSGN];
        coordinator_recalculate_schedule(coordinator, error, COUNTOF(error));
    }
}

bool coordinator_is_busy(const Coordinator *coordinator) { return coordinator->busy != 0; }

bool coordinator_recalculate_schedule(Coordinator *coordinator, wchar_t *error, size_t error_cap)
{
    AppSettings *settings = coordinator->settings;
    if (settings->schedule_enabled) settings->next_run = coordinator_compute_next_run(settings, dto_now());
    else settings->next_run.has = false;
    return store_save(coordinator->store, settings, error, error_cap);
}

DateTimeOffset coordinator_compute_next_run(const AppSettings *settings, DateTimeOffset now)
{
    if (settings->schedule_mode == SCHEDULE_INTERVAL)
        return dto_from_ticks(now.ticks + clamp(settings->interval_hours, 1, 168) * TICKS_PER_HOUR);

    SYSTEMTIME local;
    time_to_local(now.ticks, &local);
    int64_t local_now = time_from_parts(&local);
    local.wHour = (WORD)clamp(settings->daily_hour, 0, 23);
    local.wMinute = (WORD)clamp(settings->daily_minute, 0, 59);
    local.wSecond = local.wMilliseconds = 0;
    int64_t candidate = time_from_parts(&local);
    if (candidate <= local_now) candidate += TICKS_PER_DAY;

    for (int day = 0; day < 3; day++)
    {
        int64_t instants[2];
        int count = 0;
        /* A time skipped by the spring-forward change runs at the next valid local minute. */
        for (int minute = 0; minute < 24 * 60; minute++)
        {
            SYSTEMTIME wall;
            time_to_parts(candidate, &wall);
            count = time_local_to_utc(&wall, instants);
            if (count) break;
            candidate += TICKS_PER_MINUTE;
        }
        /* A repeated hour has two instants; take the first that is still ahead. */
        for (int i = 0; i < count; i++)
            if (instants[i] > now.ticks) return dto_from_ticks(instants[i]);
        candidate += TICKS_PER_DAY;
    }
    return dto_from_ticks(now.ticks + TICKS_PER_DAY);
}

static void write_activity(Coordinator *coordinator, const wchar_t *application, const wchar_t *message, bool error)
{
    ActivityEntry *entry = xalloc(sizeof(ActivityEntry));
    entry->time = dto_now();
    str_copy(entry->application, COUNTOF(entry->application), application);
    str_copy(entry->message, COUNTOF(entry->message), message);
    entry->is_error = error;
    store_log(coordinator->store, entry);
    if (coordinator->events.activity) coordinator->events.activity(coordinator->events.context, entry);
    free(entry);
}

static void notify_result(Coordinator *coordinator, const UpdateResult *result)
{
    if (coordinator->events.result_changed) coordinator->events.result_changed(coordinator->events.context, result);
}

static void notify_summary(Coordinator *coordinator, const wchar_t *summary, bool finished)
{
    if (coordinator->events.summary_changed) coordinator->events.summary_changed(coordinator->events.context, summary, finished);
}

static void notify_busy(Coordinator *coordinator, bool busy)
{
    if (coordinator->events.busy_changed) coordinator->events.busy_changed(coordinator->events.context, busy);
}

bool coordinator_begin(Coordinator *coordinator, RunSnapshot *snapshot)
{
    if (InterlockedCompareExchange(&coordinator->busy, 1, 0) != 0) return false;
    notify_busy(coordinator, true);
    snapshot->count = 0;
    for (int i = 0; i < coordinator->settings->program_count; i++)
        if (coordinator->settings->programs[i].enabled)
            snapshot->programs[snapshot->count++] = coordinator->settings->programs[i];
    return true;
}

int coordinator_execute(Coordinator *coordinator, const RunSnapshot *snapshot, bool install_updates)
{
    int updated = 0, available = 0, needs_attention = 0, waiting = 0;
    wchar_t text[MSGN];
    UpdateResult *result = xalloc(sizeof(UpdateResult)), *shown = xalloc(sizeof(UpdateResult));

    if (snapshot->count == 0)
    {
        notify_summary(coordinator, L"Turn on monitoring for at least one program.", true);
        goto finished;
    }
    write_activity(coordinator, L"Updater", install_updates ? L"Scan and automatic updates started."
        : L"Update check started. No updates will be installed during this check.", false);

    for (int i = 0; i < snapshot->count; i++)
    {
        const ProgramSettings *program = &snapshot->programs[i];
        const wchar_t *name = app_name(program->id);
        str_fmt(text, COUNTOF(text), L"Checking %s...", name);
        notify_summary(coordinator, text, false);
        result_set(shown, program->id, STATE_CHECKING, L"", L"", L"Checking for updates...");
        notify_result(coordinator, shown);

        CallStatus status = coordinator->provider.check(coordinator->provider.context, program,
            GetTickCount64() + 2 * 60 * 1000, result);
        if (status == CALL_OK)
        {
            notify_result(coordinator, result);
            write_activity(coordinator, name, result->message, result->state == STATE_ERROR);
            if (result->state == STATE_UPDATE_AVAILABLE)
            {
                UsageResult usage = { USAGE_IDLE, L"" };
                if (install_updates && program->auto_install && program->wait_while_in_use &&
                    coordinator->provider.check_usage(coordinator->provider.context, program,
                        GetTickCount64() + 30 * 1000, &usage) != CALL_OK)
                {
                    /* Never install when the busy check itself failed; the user asked to wait. */
                    usage.state = USAGE_UNKNOWN;
                    str_fmt(usage.message, COUNTOF(usage.message),
                        L"Could not check whether %s is in use, so the update was not installed.", name);
                }
                if (usage.state != USAGE_IDLE)
                {
                    *shown = *result;
                    if (usage.state == USAGE_IN_USE)
                        str_fmt(shown->message, COUNTOF(shown->message),
                            L"Update waiting: %s It will be installed when %s is not in use.", usage.message, name);
                    else str_fmt(shown->message, COUNTOF(shown->message), L"Update waiting: %s", usage.message);
                    notify_result(coordinator, shown);
                    write_activity(coordinator, name, shown->message, usage.state == USAGE_UNKNOWN);
                    if (usage.state == USAGE_IN_USE) waiting++; else needs_attention++;
                }
                else if (install_updates && program->auto_install)
                {
                    str_fmt(text, COUNTOF(text), L"Updating %s...", name);
                    notify_summary(coordinator, text, false);
                    *shown = *result;
                    shown->state = STATE_INSTALLING;
                    str_copy(shown->message, COUNTOF(shown->message), L"Installing update. Keep the updater running.");
                    notify_result(coordinator, shown);
                    /* Do not cancel or terminate a running installer just because the user hides the window. */
                    *shown = *result;
                    status = coordinator->provider.install(coordinator->provider.context, program, shown,
                        GetTickCount64() + 30 * 60 * 1000, result);
                    if (status == CALL_OK)
                    {
                        notify_result(coordinator, result);
                        write_activity(coordinator, name, result->message, result->state == STATE_ERROR);
                        if (result->state == STATE_UPDATED || result->state == STATE_RESTART_REQUIRED) updated++;
                        else needs_attention++;
                    }
                }
                else available++;
            }
            else if (result->state == STATE_ERROR || result->state == STATE_NEEDS_SETUP) needs_attention++;
        }
        if (status != CALL_OK)
        {
            /* Provider errors can contain endpoint credentials. Show a safe category only. */
            const wchar_t *message = status == CALL_TIMED_OUT ? L"The operation timed out. Check the program before retrying."
                : L"The operation failed. Check the connection and program settings, then try again.";
            result_set(shown, program->id, STATE_ERROR, L"", L"", message);
            notify_result(coordinator, shown);
            write_activity(coordinator, name, message, true);
            needs_attention++;
        }
    }

    wchar_t waiting_text[80] = L"";
    if (waiting > 0) str_fmt(waiting_text, COUNTOF(waiting_text), L"%d waiting until the app is not in use, ", waiting);
    str_fmt(text, COUNTOF(text), L"Finished: %d updated, %d updates available, %s%d need attention.",
        updated, available, waiting_text, needs_attention);
    notify_summary(coordinator, text, true);
    write_activity(coordinator, L"Updater", text, false);

finished:
    free(result);
    free(shown);
    InterlockedExchange(&coordinator->busy, 0);
    notify_busy(coordinator, false);
    return waiting;
}

int coordinator_run(Coordinator *coordinator, bool install_updates)
{
    RunSnapshot *snapshot = xalloc(sizeof(RunSnapshot));
    int waiting = coordinator_begin(coordinator, snapshot) ? coordinator_execute(coordinator, snapshot, install_updates) : 0;
    free(snapshot);
    return waiting;
}

ScheduleCheck coordinator_schedule_due(Coordinator *coordinator, wchar_t *error, size_t error_cap)
{
    AppSettings *settings = coordinator->settings;
    if (!settings->schedule_enabled || coordinator_is_busy(coordinator)) return SCHEDULE_NOT_DUE;
    if (!settings->next_run.has)
        return coordinator_recalculate_schedule(coordinator, error, error_cap) ? SCHEDULE_NOT_DUE : SCHEDULE_SAVE_FAILED;
    DateTimeOffset now = dto_now();
    if (settings->next_run.ticks > now.ticks) return SCHEDULE_NOT_DUE;
    /* Persist the next due time before running. */
    settings->last_scheduled_run = now;
    settings->next_run = coordinator_compute_next_run(settings, now);
    return store_save(coordinator->store, settings, error, error_cap) ? SCHEDULE_DUE : SCHEDULE_SAVE_FAILED;
}

void coordinator_schedule_finished(Coordinator *coordinator, int waiting)
{
    AppSettings *settings = coordinator->settings;
    if (waiting <= 0 || !settings->schedule_enabled) return;
    /* Try again soon instead of waiting a full interval or a whole day. */
    DateTimeOffset retry = dto_from_ticks(time_now() + IN_USE_RETRY_MINUTES * TICKS_PER_MINUTE);
    if (!settings->next_run.has || retry.ticks < settings->next_run.ticks)
    {
        wchar_t error[MSGN];
        settings->next_run = retry;
        store_save(coordinator->store, settings, error, COUNTOF(error));
    }
}

void coordinator_evaluate_schedule(Coordinator *coordinator)
{
    wchar_t error[MSGN];
    if (coordinator_schedule_due(coordinator, error, COUNTOF(error)) == SCHEDULE_DUE)
        coordinator_schedule_finished(coordinator, coordinator_run(coordinator, true));
}
