#include "models.h"

void program_defaults(ProgramSettings *program, ApplicationId id)
{
    memset(program, 0, sizeof(*program));
    program->id = id;
    program->enabled = true;
    program->auto_install = true;
    program->wait_while_in_use = true;
    if (id == APP_RADARR) str_copy(program->server_url, COUNTOF(program->server_url), L"http://localhost:7878");
    if (id == APP_SONARR) str_copy(program->server_url, COUNTOF(program->server_url), L"http://localhost:8989");
}

void settings_defaults(AppSettings *settings)
{
    memset(settings, 0, sizeof(*settings));
    str_copy(settings->theme, COUNTOF(settings->theme), L"Dark");
    settings->minimize_to_tray = true;
    settings->interval_minutes = 6 * 60;
    settings->interval_unit = INTERVAL_HOURS;
    settings->daily_hour = 3;
    settings->daily_days = ALL_DAYS;
    settings->app_update_check = true;
    settings->program_count = APP_COUNT;
    for (int i = 0; i < APP_COUNT; i++) program_defaults(&settings->programs[i], (ApplicationId)i);
}

ProgramSettings *settings_program(AppSettings *settings, ApplicationId id)
{
    for (int i = 0; i < settings->program_count; i++)
        if (settings->programs[i].id == id) return &settings->programs[i];
    return NULL;
}

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

int interval_unit_minutes(IntervalUnit unit)
{
    return unit == INTERVAL_DAYS ? 24 * 60 : unit == INTERVAL_HOURS ? 60 : 1;
}

int interval_from_unit(int count, IntervalUnit unit)
{
    int unit_minutes = interval_unit_minutes(unit);
    /* Round the limits inward so the result is still a whole number of the unit. */
    int low = (INTERVAL_MIN_MINUTES + unit_minutes - 1) / unit_minutes, high = INTERVAL_MAX_MINUTES / unit_minutes;
    return clamp(count, low, high) * unit_minutes;
}

void settings_normalize_schedule(AppSettings *settings)
{
    settings->schedule_mode = settings->schedule_mode == SCHEDULE_DAILY ? SCHEDULE_DAILY : SCHEDULE_INTERVAL;
    settings->interval_minutes = clamp(settings->interval_minutes, INTERVAL_MIN_MINUTES, INTERVAL_MAX_MINUTES);
    if (settings->interval_unit < INTERVAL_MINUTES || settings->interval_unit >= INTERVAL_UNIT_COUNT)
        settings->interval_unit = INTERVAL_HOURS;
    while (settings->interval_unit > INTERVAL_MINUTES && settings->interval_minutes % interval_unit_minutes(settings->interval_unit))
        settings->interval_unit = (IntervalUnit)(settings->interval_unit - 1);
    settings->daily_hour = clamp(settings->daily_hour, 0, 23);
    settings->daily_minute = clamp(settings->daily_minute, 0, 59);
    settings->daily_days &= ALL_DAYS;
    if (!settings->daily_days) settings->daily_days = ALL_DAYS;
}

const wchar_t *app_name(ApplicationId id)
{
    switch (id)
    {
    case APP_PLEX: return L"Plex Media Server";
    case APP_SABNZBD: return L"SABnzbd";
    case APP_RADARR: return L"Radarr";
    case APP_SONARR: return L"Sonarr";
    default: return L"Pulsarr";
    }
}

const wchar_t *app_key(ApplicationId id)
{
    switch (id)
    {
    case APP_PLEX: return L"Plex";
    case APP_SABNZBD: return L"Sabnzbd";
    case APP_RADARR: return L"Radarr";
    case APP_SONARR: return L"Sonarr";
    default: return L"Pulsarr";
    }
}

const wchar_t *app_description(ApplicationId id)
{
    switch (id)
    {
    case APP_PLEX: return L"Your media library, always ready.";
    case APP_SABNZBD: return L"Keep your download manager current.";
    case APP_RADARR: return L"Movie collection management.";
    case APP_SONARR: return L"TV series collection management.";
    default: return L"Plex watchlist automation.";
    }
}

const wchar_t *app_usage_description(ApplicationId id)
{
    switch (id)
    {
    case APP_PLEX: return L"Someone is watching or listening.";
    case APP_SABNZBD: return L"A download is in progress or being unpacked.";
    case APP_RADARR: case APP_SONARR: return L"A search, import, rename, or other task is running.";
    default: return L"Pulsarr is starting up or syncing watchlists.";
    }
}

void result_set(UpdateResult *result, ApplicationId id, UpdateState state, const wchar_t *installed,
    const wchar_t *available, const wchar_t *message)
{
    memset(result, 0, sizeof(*result));
    result->id = id;
    result->state = state;
    str_copy(result->installed_version, COUNTOF(result->installed_version), installed);
    str_copy(result->available_version, COUNTOF(result->available_version), available);
    str_copy(result->message, COUNTOF(result->message), message);
}
