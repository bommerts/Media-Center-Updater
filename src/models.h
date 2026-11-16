/* The application's data: supported apps, settings, and the results of checks. */
#ifndef MCU_MODELS_H
#define MCU_MODELS_H

#include "common.h"

typedef enum { APP_PLEX, APP_SABNZBD, APP_RADARR, APP_SONARR, APP_PULSARR, APP_COUNT } ApplicationId;

typedef enum
{
    STATE_NOT_CHECKED, STATE_CHECKING, STATE_UP_TO_DATE, STATE_UPDATE_AVAILABLE, STATE_INSTALLING, STATE_UPDATED,
    STATE_NOT_INSTALLED, STATE_NEEDS_SETUP, STATE_ERROR, STATE_RESTART_REQUIRED
} UpdateState;

/* SCHEDULE_INTERVAL repeats after a length of time. SCHEDULE_DAILY runs at a set local time on chosen days of the week. */
typedef enum { SCHEDULE_INTERVAL, SCHEDULE_DAILY } ScheduleMode;
/* The unit the interval is shown and edited in. The interval itself is always kept in minutes. */
typedef enum { INTERVAL_MINUTES, INTERVAL_HOURS, INTERVAL_DAYS, INTERVAL_UNIT_COUNT } IntervalUnit;

#define INTERVAL_MIN_MINUTES 15                 /* often enough for anyone, and gentle on the release feeds */
#define INTERVAL_MAX_MINUTES (30 * 24 * 60)
#define ALL_DAYS 0x7F                           /* one bit per day of the week, bit 0 is Sunday */

typedef struct ProgramSettings
{
    ApplicationId id;
    bool enabled;
    bool auto_install;
    wchar_t server_url[URLN];
    wchar_t api_key[KEYN];
    /* Hold back automatic installs while the app is busy (for example Plex streaming). On by default. */
    bool wait_while_in_use;
} ProgramSettings;

/* Bumped when saved settings need a one-time upgrade. Files without it are version 0. */
#define SETTINGS_VERSION 1

typedef struct AppSettings
{
    int settings_version;
    wchar_t theme[16];                  /* "Dark", "Light" or "System" */
    bool minimize_to_tray;
    bool start_minimized;
    bool schedule_enabled;
    ScheduleMode schedule_mode;
    int interval_minutes;
    IntervalUnit interval_unit;
    int daily_hour;
    int daily_minute;
    int daily_days;                     /* days of the week the daily time applies to, as ALL_DAYS bits */
    DateTimeOffset last_scheduled_run;
    DateTimeOffset next_run;
    /* Updates to Media Center Updater itself. Only used by copies installed with the setup program. */
    bool app_update_check;
    bool app_update_install;
    DateTimeOffset last_app_update_check;
    ProgramSettings programs[APP_COUNT];
    int program_count;
} AppSettings;

typedef struct UpdateResult
{
    ApplicationId id;
    UpdateState state;
    wchar_t installed_version[VERN];
    wchar_t available_version[VERN];
    wchar_t message[MSGN];
    wchar_t download_url[URLN];
    wchar_t checksum[80];
} UpdateResult;

typedef enum { USAGE_IDLE, USAGE_IN_USE, USAGE_UNKNOWN } UsageState;
typedef struct UsageResult { UsageState state; wchar_t message[MSGN]; } UsageResult;

typedef struct ActivityEntry
{
    DateTimeOffset time;
    wchar_t application[64];
    wchar_t message[MSGN];
    bool is_error;
} ActivityEntry;

/* What a provider call reports besides its result. A failed call must not reveal why: the reason can hold credentials. */
typedef enum { CALL_OK, CALL_FAILED, CALL_TIMED_OUT } CallStatus;

/* Checks and installs updates. `deadline` is a GetTickCount64 value. */
typedef struct UpdateProvider
{
    void *context;
    CallStatus (*check)(void *context, const ProgramSettings *settings, ULONGLONG deadline, UpdateResult *result);
    CallStatus (*install)(void *context, const ProgramSettings *settings, const UpdateResult *update,
        ULONGLONG deadline, UpdateResult *result);
    /* Read-only check of whether the app is busy right now (streaming, downloading, running a task). */
    CallStatus (*check_usage)(void *context, const ProgramSettings *settings, ULONGLONG deadline, UsageResult *result);
} UpdateProvider;

void settings_defaults(AppSettings *settings);
void program_defaults(ProgramSettings *program, ApplicationId id);
ProgramSettings *settings_program(AppSettings *settings, ApplicationId id);   /* NULL when absent */

int interval_unit_minutes(IntervalUnit unit);                   /* minutes in one unit */
int interval_from_unit(int count, IntervalUnit unit);           /* minutes, kept within the allowed range */
/* Brings the schedule's values into range, and picks a smaller unit when the interval is not a whole number of its unit. */
void settings_normalize_schedule(AppSettings *settings);

const wchar_t *app_name(ApplicationId id);
const wchar_t *app_key(ApplicationId id);                  /* short name used in file names */
const wchar_t *app_description(ApplicationId id);
const wchar_t *app_usage_description(ApplicationId id);

void result_set(UpdateResult *result, ApplicationId id, UpdateState state, const wchar_t *installed,
    const wchar_t *available, const wchar_t *message);

#endif
