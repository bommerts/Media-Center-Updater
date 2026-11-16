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
    settings->interval_hours = 6;
    settings->daily_hour = 3;
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
