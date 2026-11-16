/* Verification for the update logic. Uses simulated providers and HTTP responses and never installs real updates.
   Run without arguments for the test suite, or with one of the --inspect options to look at this PC (read-only). */
#include "../src/coordinator.h"
#include "../src/discovery.h"
#include "../src/providers.h"
#include "../src/selfupdate.h"
#include "../src/system.h"
#include <stdio.h>

static int passed;
static wchar_t directory[PATHN];

static void print(const wchar_t *format, ...)
{
    wchar_t text[4096];
    va_list args;
    va_start(args, format);
    str_vfmt(text, COUNTOF(text), format, args);
    va_end(args);
    char *utf8 = wide_to_utf8(text, NULL);
    fputs(utf8, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(utf8);
}

static void check(bool value, const wchar_t *message)
{
    if (!value)
    {
        print(L"FAIL %s", message);
        exit(1);
    }
    passed++;
    print(L"PASS %s", message);
}

static void sub_path(wchar_t *dst, size_t cap, const wchar_t *parent, const wchar_t *name)
{
    path_join(dst, cap, parent, name);
}

static void write_text(const wchar_t *path, const char *text)
{
    if (!file_write_all(path, text, strlen(text)))
    {
        print(L"FAIL could not write a test file");
        exit(1);
    }
}

static void make_store(SettingsStore *store, const wchar_t *name)
{
    wchar_t path[PATHN];
    sub_path(path, COUNTOF(path), directory, name);
    store_init(store, path);
}

static void only(AppSettings *settings, int count, const ApplicationId *ids)
{
    settings_defaults(settings);
    settings->program_count = count;
    for (int i = 0; i < count; i++)
    {
        program_defaults(&settings->programs[i], ids[i]);
        settings->programs[i].server_url[0] = 0;
    }
}

static void remove_tree(const wchar_t *path)
{
    wchar_t pattern[PATHN];
    WIN32_FIND_DATAW found;
    path_join(pattern, COUNTOF(pattern), path, L"*");
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            wchar_t child[PATHN];
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            path_join(child, COUNTOF(child), path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(path);
}

/* ---- A provider that records what it was asked to do ---- */

typedef struct FakeProvider
{
    ApplicationId checked[32], installed[32], usage_checked[32];
    int checked_count, installed_count, usage_count;
    int fail_id;
    UsageState usage[APP_COUNT];
    HANDLE block;
} FakeProvider;

static CallStatus fake_check(void *context, const ProgramSettings *settings, ULONGLONG deadline, UpdateResult *result)
{
    FakeProvider *fake = context;
    (void)deadline;
    fake->checked[fake->checked_count++] = settings->id;
    if (fake->block) WaitForSingleObject(fake->block, INFINITE);
    /* The real reason for a failure ("https://secret@example.invalid/?apikey=secret") must never reach the screen. */
    if ((int)settings->id == fake->fail_id) return CALL_FAILED;
    result_set(result, settings->id, STATE_UPDATE_AVAILABLE, L"1.0.0", L"2.0.0", L"Update available.");
    return CALL_OK;
}

static CallStatus fake_install(void *context, const ProgramSettings *settings, const UpdateResult *update,
    ULONGLONG deadline, UpdateResult *result)
{
    FakeProvider *fake = context;
    (void)deadline;
    fake->installed[fake->installed_count++] = settings->id;
    *result = *update;
    result->state = STATE_UPDATED;
    str_copy(result->installed_version, COUNTOF(result->installed_version), L"2.0.0");
    str_copy(result->message, COUNTOF(result->message), L"Updated.");
    return CALL_OK;
}

static CallStatus fake_usage(void *context, const ProgramSettings *settings, ULONGLONG deadline, UsageResult *result)
{
    FakeProvider *fake = context;
    (void)deadline;
    fake->usage_checked[fake->usage_count++] = settings->id;
    result->state = fake->usage[settings->id];
    str_copy(result->message, COUNTOF(result->message), L"Test usage.");
    return CALL_OK;
}

static UpdateProvider fake_provider(FakeProvider *fake)
{
    UpdateProvider provider = { fake, fake_check, fake_install, fake_usage };
    memset(fake, 0, sizeof(*fake));
    fake->fail_id = -1;
    return provider;
}

static bool same_ids(const ApplicationId *actual, int count, const ApplicationId *expected, int expected_count)
{
    if (count != expected_count) return false;
    for (int i = 0; i < count; i++) if (actual[i] != expected[i]) return false;
    return true;
}

static bool has_id(const ApplicationId *ids, int count, ApplicationId id)
{
    for (int i = 0; i < count; i++) if (ids[i] == id) return true;
    return false;
}

typedef struct Recorder { UpdateResult results[64]; int count; } Recorder;

static void record_result(void *context, const UpdateResult *result)
{
    Recorder *recorder = context;
    if (recorder->count < (int)COUNTOF(recorder->results)) recorder->results[recorder->count++] = *result;
}

static DWORD WINAPI run_install_thread(LPVOID parameter)
{
    coordinator_run(parameter, true);
    return 0;
}

/* ---- Canned HTTP responses ---- */

typedef struct FakeHttp
{
    bool feed;                      /* answer like Radarr's update API instead of returning `body` */
    const char *body;
    int status;
    bool refuse;
    wchar_t last_url[URLN];
    wchar_t paths[16][256];
    int path_count, authenticated, posts;
    const wchar_t *installed_version, *available_version;
    bool installable, docker;
} FakeHttp;

static HttpStatus fake_send(void *context, const HttpRequest *request, HttpResponse *response)
{
    FakeHttp *http = context;
    Url *url = xalloc(sizeof(Url));
    char body[512];
    memset(response, 0, sizeof(*response));
    str_copy(http->last_url, COUNTOF(http->last_url), request->url);
    url_parse(request->url, url);
    if (http->path_count < 16) str_copy(http->paths[http->path_count++], 256, url->path);
    if (request->method && wcscmp(request->method, L"POST") == 0) http->posts++;
    if (request->headers && wcsstr(request->headers, L"X-Api-Key: test-token\r\n")) http->authenticated++;
    bool status_path = str_iends(url->path, L"/status");
    free(url);
    if (http->refuse) return HTTP_E_CONNECT;
    if (http->feed)
    {
        if (status_path)
            _snprintf_s(body, sizeof(body), _TRUNCATE, "{\"version\":\"%ls\",\"isDocker\":%s,\"packageUpdateMechanism\":\"builtIn\"}",
                http->installed_version, http->docker ? "true" : "false");
        else
            _snprintf_s(body, sizeof(body), _TRUNCATE, "[{\"version\":\"%ls\",\"latest\":true,\"installable\":%s}]",
                http->available_version, http->installable ? "true" : "false");
    }
    else strcpy_s(body, sizeof(body), http->body);
    response->status = http->status;
    response->body_length = strlen(body);
    response->body = xalloc(response->body_length + 1);
    memcpy(response->body, body, response->body_length);
    return HTTP_OK;
}

/* ---- A throwaway SQLite database, through the SQLite library built into Windows ---- */

static HMODULE sqlite;

typedef int (*SqliteOpen)(const char *, void **);
typedef int (*SqliteExec)(void *, const char *, void *, void *, char **);
typedef int (*SqlitePrepare)(void *, const char *, int, void **, const char **);
typedef int (*SqliteHandle)(void *);
typedef int (*SqliteColumn)(void *, int);

static FARPROC sqlite_function(const char *name)
{
    if (!sqlite) sqlite = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FARPROC function = sqlite ? GetProcAddress(sqlite, name) : NULL;
    if (!function)
    {
        print(L"FAIL the Windows SQLite library is unavailable");
        exit(1);
    }
    return function;
}

static void sqlite_execute(const wchar_t *path, const char *sql)
{
    SqliteOpen open = (SqliteOpen)sqlite_function("sqlite3_open");
    SqliteExec exec = (SqliteExec)sqlite_function("sqlite3_exec");
    SqliteHandle close = (SqliteHandle)sqlite_function("sqlite3_close");
    void *database = NULL;
    char *utf8 = wide_to_utf8(path, NULL);
    bool ok = open(utf8, &database) == 0 && exec(database, sql, NULL, NULL, NULL) == 0;
    if (database) close(database);
    free(utf8);
    if (!ok)
    {
        print(L"FAIL test SQL failed");
        exit(1);
    }
}

static int sqlite_count(const wchar_t *path, const char *sql)
{
    SqliteOpen open = (SqliteOpen)sqlite_function("sqlite3_open");
    SqlitePrepare prepare = (SqlitePrepare)sqlite_function("sqlite3_prepare_v2");
    SqliteHandle step = (SqliteHandle)sqlite_function("sqlite3_step");
    SqliteColumn column_int = (SqliteColumn)sqlite_function("sqlite3_column_int");
    SqliteHandle finalize = (SqliteHandle)sqlite_function("sqlite3_finalize");
    SqliteHandle close = (SqliteHandle)sqlite_function("sqlite3_close");
    void *database = NULL, *statement = NULL;
    char *utf8 = wide_to_utf8(path, NULL);
    int result = -1;
    if (open(utf8, &database) == 0 && prepare(database, sql, -1, &statement, NULL) == 0 && step(statement) == 100)
        result = column_int(statement, 0);
    if (statement) finalize(statement);
    if (database) close(database);
    free(utf8);
    return result;
}

/* ---- Read-only looks at this PC ---- */

static const wchar_t *state_name(UpdateState state)
{
    static const wchar_t *const names[] = { L"NotChecked", L"Checking", L"UpToDate", L"UpdateAvailable", L"Installing",
        L"Updated", L"NotInstalled", L"NeedsSetup", L"Error", L"RestartRequired" };
    return names[state];
}

static int inspect_local(void)
{
    /* Deployment verification calls only the provider's read-only check method. */
    Providers providers;
    AppSettings *settings = xalloc(sizeof(AppSettings));
    UpdateResult *result = xalloc(sizeof(UpdateResult));
    providers_init(&providers, NULL, NULL);
    UpdateProvider provider = providers_interface(&providers);
    settings_defaults(settings);
    for (int i = 0; i < settings->program_count; i++)
    {
        CallStatus status = provider.check(provider.context, &settings->programs[i], GetTickCount64() + 120000, result);
        if (status != CALL_OK) print(L"%s: timed out", app_key(settings->programs[i].id));
        else print(L"%s: %s installed=%s available=%s - %s", app_key(result->id), state_name(result->state),
            result->installed_version, result->available_version, result->message);
    }
    return 0;
}

static int inspect_usage(void)
{
    /* Read-only: asks each app on this PC whether it is busy. Uses the saved Radarr/Sonarr/Pulsarr settings. */
    static const wchar_t *const names[] = { L"Idle", L"InUse", L"Unknown" };
    Providers providers;
    SettingsStore store;
    AppSettings *settings = xalloc(sizeof(AppSettings));
    UsageResult usage;
    providers_init(&providers, NULL, NULL);
    UpdateProvider provider = providers_interface(&providers);
    store_init(&store, NULL);
    store_load(&store, settings);
    for (int i = 0; i < settings->program_count; i++)
    {
        if (provider.check_usage(provider.context, &settings->programs[i], GetTickCount64() + 30000, &usage) != CALL_OK)
            print(L"%s: timed out", app_key(settings->programs[i].id));
        else print(L"%s: %s - %s", app_key(settings->programs[i].id), names[usage.state], usage.message);
    }
    return 0;
}

static int inspect_pulsarr_key(void)
{
    /* Read-only: looks for Pulsarr's API key on this PC. Prints whether one was found, never the key. */
    KeyDiscovery *found = xalloc(sizeof(KeyDiscovery));
    print(L"Sign-in required: %s", pulsarr_requires_api_key(NULL) ? L"True" : L"False");
    discover_pulsarr_api_key(NULL, found);
    print(L"%s", found->message);
    return 0;
}

static int inspect_self_update(const wchar_t *version)
{
    /* Finds and downloads the real latest release as if this were the given older version.
       It verifies the checksum but never runs the setup program. */
    SelfUpdateInfo info;
    wchar_t error[MSGN], folder[PATHN], temp[PATHN], guid[40], file[PATHN];
    SelfUpdateCheck result = self_update_check(NULL, NULL, version, &info, error, COUNTOF(error));
    if (result == SELF_UPDATE_FAILED) { print(L"Check failed: %s", error); return 1; }
    if (result == SELF_UPDATE_CURRENT) { print(L"Up to date."); return 0; }
    print(L"Found %s at %s", info.version, info.download_url);
    GetTempPathW(PATHN, temp);
    new_guid_text(guid, COUNTOF(guid));
    path_join(folder, COUNTOF(folder), temp, L"MediaCenterUpdater.Tests");
    path_join(folder, COUNTOF(folder), folder, guid);
    dir_create(folder);
    if (!self_update_download(NULL, NULL, &info, folder, file, COUNTOF(file), error, COUNTOF(error)))
    {
        print(L"Download failed: %s", error);
        remove_tree(folder);
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA data = { 0 };
    GetFileAttributesExW(file, GetFileExInfoStandard, &data);
    print(L"Downloaded and verified %llu bytes.", ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
    remove_tree(folder);
    return 0;
}

/* ---- The test suite ---- */

static const char *release_json(char *buffer, size_t cap, const char *tag, const char *name, const char *digest,
    const char *url, bool prerelease)
{
    _snprintf_s(buffer, cap, _TRUNCATE,
        "{\"tag_name\":\"%s\",\"prerelease\":%s,\"draft\":false,\"assets\":[{\"name\":\"%s\",\"digest\":\"%s\",\"browser_download_url\":\"%s\"}]}",
        tag, prerelease ? "true" : "false", name, digest, url);
    return buffer;
}

static SelfUpdateCheck parse_release_text(const char *json, const wchar_t *current, SelfUpdateInfo *info)
{
    wchar_t error[MSGN];
    JsonDoc *doc = json_parse_utf8(json, strlen(json));
    SelfUpdateCheck result = doc ? self_update_parse_release(doc->root, current, info, error, COUNTOF(error)) : SELF_UPDATE_FAILED;
    json_free(doc);
    return result;
}

static void test_settings(void)
{
    AppSettings *defaults = xalloc(sizeof(AppSettings)), *loaded = xalloc(sizeof(AppSettings));
    SettingsStore store, upgrade;
    wchar_t path[PATHN], error[MSGN];
    char *data;
    size_t length;

    settings_defaults(defaults);
    check(wcscmp(defaults->theme, L"Dark") == 0 && defaults->minimize_to_tray && !defaults->schedule_enabled,
        L"Dark and tray defaults, schedule requires opt-in");
    make_store(&store, L"secrets");
    str_copy(defaults->programs[2].api_key, KEYN, L"test-key-not-real-4321");
    check(store_save(&store, defaults, error, COUNTOF(error)), L"Settings can be saved");
    sub_path(path, COUNTOF(path), store.data_directory, L"settings.json");
    file_read_all(path, 1 << 20, &data, &length);
    check(data && !strstr(data, "test-key-not-real"), L"API keys are encrypted on disk");
    free(data);
    store_load(&store, loaded);
    check(wcscmp(loaded->programs[2].api_key, defaults->programs[2].api_key) == 0, L"API keys round-trip for the Windows user");
    check(wcscmp(defaults->programs[2].api_key, L"test-key-not-real-4321") == 0, L"Saving does not replace the in-memory API key");
    write_text(path, "{ broken");
    store_load(&store, loaded);
    check(!loaded->schedule_enabled && store.load_warning[0], L"Corrupt settings cannot resume automatic updates");
    settings_defaults(loaded);
    bool all_wait = true;
    for (int i = 0; i < loaded->program_count; i++) all_wait = all_wait && loaded->programs[i].wait_while_in_use;
    check(all_wait, L"New installs wait while every app is in use");

    make_store(&upgrade, L"upgrade");
    sub_path(path, COUNTOF(path), upgrade.data_directory, L"settings.json");
    write_text(path, "{\"Programs\":[{\"Id\":0,\"WaitWhileInUse\":false},{\"Id\":4,\"WaitWhileInUse\":false}]}");
    store_load(&upgrade, loaded);
    all_wait = loaded->program_count == APP_COUNT;
    for (int i = 0; i < loaded->program_count; i++) all_wait = all_wait && loaded->programs[i].wait_while_in_use;
    check(all_wait, L"Settings saved by 0.3.0 are upgraded once to wait while apps are in use");
    loaded->programs[0].wait_while_in_use = false;
    store_save(&upgrade, loaded, error, COUNTOF(error));
    store_load(&upgrade, loaded);
    check(!loaded->programs[0].wait_while_in_use, L"Turning waiting off after the upgrade is kept");

    /* A settings file written by the earlier .NET version of the app, as that version formats it. */
    write_text(path, "{\r\n  \"SettingsVersion\": 1,\r\n  \"Theme\": \"Light\",\r\n  \"MinimizeToTray\": false,\r\n  \"StartMinimized\": true,\r\n"
        "  \"ScheduleEnabled\": true,\r\n  \"ScheduleMode\": 1,\r\n  \"IntervalHours\": 12,\r\n  \"DailyHour\": 4,\r\n  \"DailyMinute\": 45,\r\n"
        "  \"LastScheduledRun\": null,\r\n  \"NextRun\": \"2026-11-16T04:45:00.1234567-06:00\",\r\n  \"AppUpdateCheck\": false,\r\n"
        "  \"AppUpdateInstall\": true,\r\n  \"LastAppUpdateCheck\": \"2026-11-15T09:30:00+00:00\",\r\n  \"Programs\": [\r\n"
        "    {\r\n      \"Id\": 2,\r\n      \"Enabled\": false,\r\n      \"AutoInstall\": false,\r\n      \"ServerUrl\": \"http://localhost:7878/radarr\",\r\n"
        "      \"ApiKey\": \"\",\r\n      \"WaitWhileInUse\": false\r\n    }\r\n  ]\r\n}");
    store_load(&upgrade, loaded);
    SYSTEMTIME next;
    time_to_parts(loaded->next_run.ticks, &next);
    check(!upgrade.load_warning[0] && wcscmp(loaded->theme, L"Light") == 0 && !loaded->minimize_to_tray && loaded->start_minimized &&
        loaded->schedule_enabled && loaded->schedule.mode == SCHEDULE_DAILY && loaded->schedule.interval_minutes == 12 * 60 &&
        loaded->schedule.interval_unit == INTERVAL_HOURS && loaded->schedule.daily_days == ALL_DAYS &&
        loaded->schedule.daily_hour == 4 && loaded->schedule.daily_minute == 45 && !loaded->last_scheduled_run.has && loaded->next_run.has &&
        next.wDay == 16 && next.wHour == 10 && next.wMinute == 45 && loaded->next_run.offset_minutes == -360 &&
        !loaded->app_update_check && loaded->app_update_install && loaded->last_app_update_check.has &&
        loaded->app_update_timing.mode == SCHEDULE_INTERVAL && loaded->app_update_timing.interval_minutes == 24 * 60 &&
        loaded->app_update_timing.interval_unit == INTERVAL_DAYS &&
        loaded->program_count == APP_COUNT && loaded->programs[0].id == APP_RADARR && !loaded->programs[0].enabled &&
        !loaded->programs[0].auto_install && !loaded->programs[0].wait_while_in_use &&
        wcscmp(loaded->programs[0].server_url, L"http://localhost:7878/radarr") == 0,
        L"Settings saved by the earlier .NET version load unchanged");

    loaded->schedule.mode = SCHEDULE_INTERVAL;
    loaded->schedule.interval_minutes = 45;
    loaded->schedule.interval_unit = INTERVAL_MINUTES;
    loaded->schedule.daily_days = (1 << 1) | (1 << 5);
    loaded->app_update_timing.mode = SCHEDULE_DAILY;
    loaded->app_update_timing.interval_minutes = 7 * 24 * 60;
    loaded->app_update_timing.daily_hour = 21;
    loaded->app_update_timing.daily_minute = 5;
    loaded->app_update_timing.daily_days = 1 << 0;
    store_save(&upgrade, loaded, error, COUNTOF(error));
    store_load(&upgrade, loaded);
    check(loaded->schedule.interval_minutes == 45 && loaded->schedule.interval_unit == INTERVAL_MINUTES && loaded->schedule.daily_days == ((1 << 1) | (1 << 5)),
        L"An interval in minutes and chosen days of the week are saved and read back");
    check(loaded->app_update_timing.mode == SCHEDULE_DAILY && loaded->app_update_timing.interval_minutes == 7 * 24 * 60 &&
        loaded->app_update_timing.interval_unit == INTERVAL_DAYS && loaded->app_update_timing.daily_hour == 21 &&
        loaded->app_update_timing.daily_minute == 5 && loaded->app_update_timing.daily_days == 1 << 0 &&
        loaded->schedule.mode == SCHEDULE_INTERVAL,
        L"The timing of checks for a new app version is saved and read back, apart from the update schedule");
    write_text(path, "{\"IntervalMinutes\":90,\"IntervalUnit\":1,\"DailyDays\":0}");
    store_load(&upgrade, loaded);
    check(loaded->schedule.interval_minutes == 90 && loaded->schedule.interval_unit == INTERVAL_MINUTES && loaded->schedule.daily_days == ALL_DAYS,
        L"An interval that is not whole hours is shown in minutes, and a schedule with no days runs every day");
    write_text(path, "{\"IntervalMinutes\":1,\"IntervalUnit\":9}");
    store_load(&upgrade, loaded);
    check(loaded->schedule.interval_minutes == INTERVAL_MIN_MINUTES && interval_from_unit(0, INTERVAL_HOURS) == 60 &&
        interval_from_unit(1000, INTERVAL_DAYS) == INTERVAL_MAX_MINUTES && interval_from_unit(3, INTERVAL_MINUTES) == INTERVAL_MIN_MINUTES,
        L"Intervals stay between 15 minutes and 30 days");

    ActivityEntry entry = { dto_now(), L"Updater", L"A \"quoted\" message \x2192 with\nnew line", true }, read_back[4];
    store_log(&upgrade, &entry);
    entry.time.ticks += TICKS_PER_SECOND;
    str_copy(entry.message, COUNTOF(entry.message), L"Second");
    entry.is_error = false;
    store_log(&upgrade, &entry);
    int entries = store_read_recent_activity(&upgrade, read_back, 4);
    check(entries == 2 && wcscmp(read_back[0].message, L"Second") == 0 && !read_back[0].is_error && read_back[1].is_error &&
        wcscmp(read_back[1].message, L"A \"quoted\" message \x2192 with\nnew line") == 0 && wcscmp(read_back[1].application, L"Updater") == 0,
        L"Recent activity is read back newest first");

    store_destroy(&store);
    store_destroy(&upgrade);
    free(defaults);
    free(loaded);
}

static void test_coordinator(void)
{
    static const ApplicationId three[] = { APP_PLEX, APP_SABNZBD, APP_SONARR }, plex_only[] = { APP_PLEX };
    static const ApplicationId plex_sonarr[] = { APP_PLEX, APP_SONARR }, sonarr_only[] = { APP_SONARR };
    AppSettings *settings = xalloc(sizeof(AppSettings)), *loaded = xalloc(sizeof(AppSettings));
    Recorder *recorder = xalloc(sizeof(Recorder));
    FakeProvider fake;
    Coordinator coordinator;
    SettingsStore store;
    bool found;

    only(settings, 3, three);
    settings->programs[1].enabled = false;
    settings->programs[2].auto_install = false;
    make_store(&store, L"check");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    coordinator_run(&coordinator, false);
    check(same_ids(fake.checked, fake.checked_count, plex_sonarr, 2) && fake.installed_count == 0,
        L"Check-only scans skip disabled apps and never install");
    fake.checked_count = 0;
    coordinator_run(&coordinator, true);
    check(same_ids(fake.installed, fake.installed_count, plex_only, 1), L"Automatic updates respect each program's install switch");

    only(settings, 3, three);
    settings->programs[2].wait_while_in_use = false;
    make_store(&store, L"in-use");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    fake.usage[APP_PLEX] = USAGE_IN_USE;
    fake.usage[APP_SABNZBD] = USAGE_UNKNOWN;
    fake.usage[APP_SONARR] = USAGE_IN_USE;
    coordinator.events.context = recorder;
    coordinator.events.result_changed = record_result;
    int waiting = coordinator_run(&coordinator, true);
    check(same_ids(fake.installed, fake.installed_count, sonarr_only, 1),
        L"Apps set to wait are not updated while busy or when their activity cannot be checked");
    found = false;
    for (int i = 0; i < recorder->count; i++)
        found = found || (recorder->results[i].id == APP_PLEX && recorder->results[i].state == STATE_UPDATE_AVAILABLE &&
            wcsncmp(recorder->results[i].message, L"Update waiting", 14) == 0);
    check(waiting == 1 && found, L"A busy app keeps its available update and says it is waiting");
    check(!has_id(fake.usage_checked, fake.usage_count, APP_SONARR), L"Apps not set to wait are installed without a busy check");

    make_store(&store, L"idle");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    coordinator_run(&coordinator, true);
    check(fake.installed_count == 3, L"Idle apps set to wait are updated normally");
    make_store(&store, L"check-only-usage");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    coordinator_run(&coordinator, false);
    check(fake.usage_count == 0, L"Check-only scans never ask whether apps are busy");

    settings->schedule_enabled = true;
    settings->schedule.mode = SCHEDULE_DAILY;
    settings->next_run = dto_from_ticks(time_now() - TICKS_PER_MINUTE);
    make_store(&store, L"retry");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    fake.usage[APP_PLEX] = USAGE_IN_USE;
    coordinator_evaluate_schedule(&coordinator);
    check(fake.checked_count == 3 && settings->next_run.ticks <= time_now() + IN_USE_RETRY_MINUTES * TICKS_PER_MINUTE + 5 * TICKS_PER_SECOND,
        L"A scheduled update that waited for a busy app retries in about 30 minutes");

    make_store(&store, L"failure");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    fake.fail_id = APP_PLEX;
    recorder->count = 0;
    coordinator.events.context = recorder;
    coordinator.events.result_changed = record_result;
    coordinator_run(&coordinator, true);
    check(has_id(fake.checked, fake.checked_count, APP_SONARR) && !coordinator_is_busy(&coordinator),
        L"A failed app does not stop the remaining checks or leave the updater busy");
    found = false;
    for (int i = 0; i < recorder->count; i++)
        found = found || (recorder->results[i].id == APP_PLEX && recorder->results[i].state == STATE_ERROR &&
            !wcsstr(recorder->results[i].message, L"secret"));
    check(found, L"Provider exceptions do not leak credentials to the UI");

    only(settings, 1, plex_only);
    make_store(&store, L"overlap");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    fake.block = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE first_run = CreateThread(NULL, 0, run_install_thread, &coordinator, 0, NULL);
    for (int i = 0; i < 500 && fake.checked_count == 0; i++) Sleep(10);
    coordinator_run(&coordinator, true);
    check(fake.checked_count == 1, L"Overlapping manual and scheduled runs are excluded");
    SetEvent(fake.block);
    WaitForSingleObject(first_run, INFINITE);
    CloseHandle(first_run);
    CloseHandle(fake.block);
    check(fake.installed_count == 1, L"Overlapping requests cannot install an update twice");

    only(settings, 1, plex_only);
    settings->schedule_enabled = true;
    settings->schedule.interval_minutes = 6 * 60;
    settings->next_run = dto_from_ticks(time_now() - 3 * TICKS_PER_DAY);
    make_store(&store, L"schedule");
    coordinator_init(&coordinator, settings, &store, fake_provider(&fake));
    coordinator_evaluate_schedule(&coordinator);
    coordinator_evaluate_schedule(&coordinator);
    check(fake.checked_count == 1 && settings->next_run.ticks > time_now(), L"A missed schedule runs once and advances to the next interval");
    store_load(&store, loaded);
    check(loaded->next_run.has && loaded->next_run.ticks == settings->next_run.ticks, L"Next scheduled run survives restart");
    settings->schedule_enabled = false;
    settings->next_run = dto_from_ticks(time_now() - TICKS_PER_MINUTE);
    coordinator_evaluate_schedule(&coordinator);
    check(fake.checked_count == 1, L"Disabling the schedule blocks overdue work");

    SYSTEMTIME parts = { 2026, 9, 0, 11, 16, 0, 0, 0 }, local;
    DateTimeOffset now = { true, time_from_parts(&parts), 0 };
    settings->schedule.interval_minutes = 45;
    DateTimeOffset next = coordinator_compute_next_run(&settings->schedule, now);
    check(next.ticks - now.ticks == 45 * TICKS_PER_MINUTE, L"An interval can be shorter than an hour");
    settings->schedule.interval_minutes = 3 * 24 * 60;
    check(coordinator_compute_next_run(&settings->schedule, now).ticks - now.ticks == 3 * TICKS_PER_DAY, L"An interval can be several days");

    settings->schedule.mode = SCHEDULE_DAILY;
    settings->schedule.daily_hour = 3;
    settings->schedule.daily_minute = 15;
    next = coordinator_compute_next_run(&settings->schedule, now);
    time_to_local(next.ticks, &local);
    check(next.ticks > now.ticks && next.ticks - now.ticks <= 25 * TICKS_PER_HOUR && local.wHour == 3 && local.wMinute == 15,
        L"Daily schedule uses the next occurrence in local time");
    settings->schedule.daily_hour = 1;
    settings->schedule.daily_minute = 30;
    SYSTEMTIME repeated = { 2026, 11, 0, 1, 6, 15, 0, 0 };      /* 1:15 at UTC-5, inside the repeated hour in North America */
    now.ticks = time_from_parts(&repeated);
    check(coordinator_compute_next_run(&settings->schedule, now).ticks > now.ticks, L"Repeated daylight-saving hour cannot produce a past next-run time");
    /* Every minute across two years, a day at a time, must give a later time that lands on the requested minute. */
    bool consistent = true;
    for (int day = 0; day < 730 && consistent; day++)
    {
        for (int hour = 0; hour < 24 && consistent; hour += 1)
        {
            settings->schedule.daily_hour = hour;
            settings->schedule.daily_minute = 30;
            SYSTEMTIME start = { 2026, 1, 0, 1, 0, 0, 0, 0 };
            now.ticks = time_from_parts(&start) + day * TICKS_PER_DAY + 17 * TICKS_PER_MINUTE;
            next = coordinator_compute_next_run(&settings->schedule, now);
            consistent = next.ticks > now.ticks && next.ticks - now.ticks <= 25 * TICKS_PER_HOUR + TICKS_PER_MINUTE;
        }
    }
    check(consistent, L"Daily schedule always moves forward, by a day at most, on every day of the year");

    /* Chosen days: every start day across two years, for each single day and for a weekend pair. */
    static const int day_sets[] = { 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6, (1 << 0) | (1 << 6) };
    settings->schedule.daily_hour = 3;
    consistent = true;
    for (int set = 0; set < (int)COUNTOF(day_sets) && consistent; set++)
    {
        settings->schedule.daily_days = day_sets[set];
        int64_t longest = (day_sets[set] == ((1 << 0) | (1 << 6)) ? 6 : 7) * TICKS_PER_DAY + 2 * TICKS_PER_HOUR;
        for (int day = 0; day < 730 && consistent; day++)
        {
            SYSTEMTIME start = { 2026, 1, 0, 1, 0, 0, 0, 0 };
            now.ticks = time_from_parts(&start) + day * TICKS_PER_DAY + 17 * TICKS_PER_MINUTE;
            next = coordinator_compute_next_run(&settings->schedule, now);
            time_to_local(next.ticks, &local);
            consistent = next.ticks > now.ticks && next.ticks - now.ticks <= longest && (day_sets[set] & (1 << local.wDayOfWeek)) &&
                local.wMinute == 30;
        }
    }
    check(consistent, L"A schedule for chosen days of the week only runs on those days, at the requested time");
    settings->schedule.daily_days = 0;
    SYSTEMTIME anytime = { 2026, 6, 0, 10, 12, 0, 0, 0 };
    now.ticks = time_from_parts(&anytime);
    check(coordinator_compute_next_run(&settings->schedule, now).ticks - now.ticks <= 25 * TICKS_PER_HOUR, L"A schedule with no days chosen still runs daily");
    settings->schedule.daily_days = ALL_DAYS;

    Version version;
    check(version_parse(L"v1.2.3", &version) && version.part[0] == 1 && version.part[1] == 2 && version.part[2] == 3 && version.part[3] == 0,
        L"Official release tags compare with normalized installed versions");
    check(!version_parse(L"unknown", &version), L"Unknown versions cannot be treated as update candidates");
    check(version_parse(L"1.41.3.9314-a0bfb8370", &version) && version.part[3] == 9314 && !version_parse(L"1", &version) &&
        !version_parse(L"1.2.3.4.5", &version) && !version_parse(L"1.2-", &version) && !version_parse(L"1.2 beta", &version),
        L"Version labels are accepted and malformed versions are refused");

    store_destroy(&store);
    free(settings);
    free(loaded);
    free(recorder);
}

static void test_discovery(void)
{
    wchar_t root[PATHN], radarr[PATHN], config[PATHN], address[300], computer[MAX_COMPUTERNAME_LENGTH + 1], executable[PATHN];
    wchar_t other[PATHN], command[PATHN + 8], folder[PATHN];
    DWORD size = COUNTOF(computer);
    KeyDiscovery *found = xalloc(sizeof(KeyDiscovery));
    PulsarrSetup setup;

    sub_path(root, COUNTOF(root), directory, L"local-setup");
    sub_path(radarr, COUNTOF(radarr), root, L"Radarr");
    dir_create(radarr);
    sub_path(config, COUNTOF(config), radarr, L"config.xml");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(!found->found && !found->api_key[0], L"Missing application settings leave the API key empty");
    write_text(config, "<Config><Port>7878</Port><ApiKey>test-discovered-key</ApiKey><UrlBase></UrlBase></Config>");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(found->found && wcscmp(found->api_key, L"test-discovered-key") == 0,
        L"Local retrieval finds the key in the matching application's existing settings");
    check(!wcsstr(found->message, L"test-discovered-key"), L"Discovery status and diagnostic text do not reveal API keys");
    GetComputerNameW(computer, &size);
    str_fmt(address, COUNTOF(address), L"http://%s:7878", computer);
    discover_api_key(APP_RADARR, address, root, found);
    check(found->found, L"This PC's exact computer name is supported for local retrieval");
    discover_api_key(APP_RADARR, L"http://other-computer.invalid:7878", root, found);
    check(!found->found, L"A local API key is never discovered for another computer's address");
    discover_api_key(APP_RADARR, L"http://localhost:9999", root, found);
    check(!found->found, L"Another local port cannot receive the wrong application's key");
    discover_api_key(APP_RADARR, L"http://localhost:7878/another-instance", root, found);
    check(!found->found, L"Another base path cannot receive the wrong application's key");
    write_text(config, "<Config><Port>7878</Port><ApiKey>test-discovered-key</ApiKey><UrlBase>/radarr</UrlBase></Config>");
    discover_api_key(APP_RADARR, L"http://127.0.0.1:7878/radarr/", root, found);
    check(found->found, L"Retrieval accepts the configured base path and loopback address");
    write_text(config, "<Config><Port>7878</Port><ApiKey> </ApiKey></Config>");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(!found->found, L"An empty API key is reported without filling a field");
    write_text(config, "<Config><ApiKey>test-secret");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(!found->found && !wcsstr(found->message, L"test-secret"), L"Malformed settings fail without exposing configuration contents");
    write_text(config, "<!DOCTYPE Config [<!ENTITY key 'test-secret'>]><Config><ApiKey>&key;</ApiKey></Config>");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(!found->found, L"Configuration retrieval prohibits XML document type declarations");
    char *large = xalloc(1000100);
    strcpy_s(large, 1000100, "<Config><ApiKey>");
    memset(large + 16, 'x', 1000000);
    strcpy_s(large + 1000016, 80, "</ApiKey></Config>");
    write_text(config, large);
    free(large);
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(!found->found, L"Oversized configuration files are rejected before parsing");
    write_text(config, "\xEF\xBB\xBF<?xml version=\"1.0\"?>\r\n<!-- settings -->\r\n<Config>\r\n  <BindAddress>*</BindAddress>\r\n  <Port>7878</Port>\r\n"
        "  <SslPort>9898</SslPort>\r\n  <EnableSsl>False</EnableSsl>\r\n  <ApiKey>abc&amp;def&#x31;<![CDATA[<2>]]></ApiKey>\r\n  <UrlBase />\r\n</Config>\r\n");
    discover_api_key(APP_RADARR, L"http://localhost:7878", root, found);
    check(found->found && wcscmp(found->api_key, L"abc&def1<2>") == 0, L"A settings file as Radarr writes it is read, including escaped characters");
    discover_api_key(APP_RADARR, L"https://localhost:9898", root, found);
    check(!found->found, L"An HTTPS address is not matched when the application has HTTPS turned off");

    sub_path(folder, COUNTOF(folder), directory, L"Pulsarr");
    sub_path(executable, COUNTOF(executable), folder, L"pulsarr-service.exe");
    str_fmt(command, COUNTOF(command), L"\"%s\"", executable);
    evaluate_pulsarr_setup(true, executable, true, true, command, L"startafterinstall,firewall", &setup);
    check(setup.supported && wcsstr(setup.message, L"no reinstall"), L"A matching official Pulsarr service with saved options satisfies the requirement as-is");
    evaluate_pulsarr_setup(false, L"", false, false, NULL, NULL, &setup);
    check(!setup.supported, L"Unregistered Pulsarr copies cannot claim automatic-update support");
    evaluate_pulsarr_setup(true, executable, true, false, NULL, L"", &setup);
    check(!setup.supported, L"A Pulsarr installation without its Windows service needs setup");
    sub_path(folder, COUNTOF(folder), directory, L"Other");
    sub_path(other, COUNTOF(other), folder, L"pulsarr-service.exe");
    str_fmt(command, COUNTOF(command), L"\"%s\"", other);
    evaluate_pulsarr_setup(true, executable, true, true, command, L"", &setup);
    check(!setup.supported, L"A service for a different Pulsarr copy cannot claim automatic-update support");
    str_fmt(command, COUNTOF(command), L"\"%s\"", executable);
    evaluate_pulsarr_setup(true, executable, true, true, command, NULL, &setup);
    check(!setup.supported, L"Pulsarr support requires saved installation choices so updates can preserve them");
    str_fmt(command, COUNTOF(command), L"%s --service", executable);
    evaluate_pulsarr_setup(true, executable, true, true, command, L"", &setup);
    check(setup.supported, L"A service command without quotes is matched up to its program name");
    free(found);
}

static void test_usage(void)
{
    wchar_t data[PATHN], keys[PATHN], path[PATHN], database[PATHN], broken[PATHN], base[256], ini[PATHN];
    int port, sessions;
    SabnzbdConfig *sab = xalloc(sizeof(SabnzbdConfig));
    KeyDiscovery *found = xalloc(sizeof(KeyDiscovery));
    ProgramSettings *program = xalloc(sizeof(ProgramSettings));
    Url *url = xalloc(sizeof(Url));
    UsageResult usage;
    UpdateResult *result = xalloc(sizeof(UpdateResult));

    read_sabnzbd_config(L"__version__ = 19\n[misc]\nhost = 0.0.0.0\nport = 8090\nurl_base = /sab\napi_key = test-sab-key\nenable_https = 0\n[servers]\n[[news]]\nport = 563\n", sab);
    check(wcscmp(sab->host, L"127.0.0.1") == 0 && sab->port == 8090 && !sab->https && wcscmp(sab->url_base, L"/sab") == 0 &&
        wcscmp(sab->api_key, L"test-sab-key") == 0, L"SABnzbd's address and key are read from its [misc] settings only");
    read_sabnzbd_config(L"[misc]\nenable_https = 1\n", sab);
    check(sab->https, L"SABnzbd with HTTPS on its main port is checked over HTTPS");
    JsonDoc *doc = json_parse(L"{\"MediaContainer\":{\"size\":2}}");
    check(doc && count_plex_sessions(doc->root, &sessions) && sessions == 2, L"Plex stream count comes from its sessions list");
    json_free(doc);

    sub_path(data, COUNTOF(data), directory, L"pulsarr-data");
    dir_create(data);
    read_pulsarr_address(data, &port, base, COUNTOF(base));
    check(port == 3003 && !base[0], L"Pulsarr defaults to port 3003");
    sub_path(path, COUNTOF(path), data, L".env");
    write_text(path, "# comment\nport=4000\nlistenPort=3100\nbasePath=/pulsarr\n");
    read_pulsarr_address(data, &port, base, COUNTOF(base));
    check(port == 3100 && wcscmp(base, L"/pulsarr") == 0, L"Pulsarr's listen port and base path come from its .env file");

    sub_path(keys, COUNTOF(keys), directory, L"pulsarr-keys");
    discover_pulsarr_api_key(keys, found);
    check(!found->found, L"A missing Pulsarr data folder finds no key");
    sub_path(path, COUNTOF(path), keys, L"db");
    dir_create(path);
    check(pulsarr_requires_api_key(keys), L"Pulsarr requires sign-in by default");
    sub_path(path, COUNTOF(path), keys, L".env");
    write_text(path, "authenticationMethod=required\n");
    check(pulsarr_requires_api_key(keys), L"Pulsarr's required sign-in setting is read from its .env file");
    write_text(path, "authenticationMethod=requiredExceptLocal\n");
    check(!pulsarr_requires_api_key(keys), L"Pulsarr's requiredExceptLocal sign-in setting is read from its .env file");
    write_text(path, "authenticationMethod=disabled\n");
    check(!pulsarr_requires_api_key(keys), L"Pulsarr's disabled sign-in setting is read from its .env file");
    discover_pulsarr_api_key(keys, found);
    check(!found->found && wcsstr(found->message, L"database was not found"), L"A missing Pulsarr database is reported");
    sub_path(database, COUNTOF(database), keys, L"db\\pulsarr.db");
    sqlite_execute(database, "CREATE TABLE admin_users (id integer PRIMARY KEY, role text); "
        "CREATE TABLE api_keys (id integer PRIMARY KEY AUTOINCREMENT, name text, key text, created_at datetime, is_active boolean DEFAULT '1', user_id integer NULL);");
    discover_pulsarr_api_key(keys, found);
    check(!found->found && wcsstr(found->message, L"Utilities"), L"Pulsarr without a key says where to create one");
    sqlite_execute(database, "INSERT INTO admin_users VALUES (1, 'admin'); "
        "INSERT INTO api_keys (name, key, created_at, is_active, user_id) VALUES ('old', 'test-pulsarr-old', '2026-01-01', 1, 1); "
        "INSERT INTO api_keys (name, key, created_at, is_active, user_id) VALUES ('revoked', 'test-pulsarr-revoked', '2026-03-01', 0, 1); "
        "INSERT INTO api_keys (name, key, created_at, is_active, user_id) VALUES ('orphan', 'test-pulsarr-orphan', '2026-04-01', 1, NULL); "
        "INSERT INTO api_keys (name, key, created_at, is_active, user_id) VALUES ('new', 'test-pulsarr-new', '2026-02-01', 1, 1);");
    discover_pulsarr_api_key(keys, found);
    check(found->found && wcscmp(found->api_key, L"test-pulsarr-new") == 0, L"The newest active admin key is read from Pulsarr's database");
    check(!wcsstr(found->message, L"test-pulsarr"), L"Pulsarr discovery messages never include the key");
    check(file_exists(database) && sqlite_count(database, "SELECT count(*) FROM api_keys") == 4, L"Reading Pulsarr's key does not change its database");
    write_text(path, "dbType=postgres\n");
    discover_pulsarr_api_key(keys, found);
    check(!found->found, L"Pulsarr on PostgreSQL asks for the key to be pasted");
    write_text(path, "");
    sub_path(broken, COUNTOF(broken), directory, L"pulsarr-broken");
    sub_path(path, COUNTOF(path), broken, L"db");
    dir_create(path);
    sub_path(path, COUNTOF(path), broken, L"db\\pulsarr.db");
    write_text(path, "not a database");
    discover_pulsarr_api_key(broken, found);
    check(!found->found && !wcsstr(found->message, broken), L"An unreadable Pulsarr database fails without showing its path");

    FakeHttp *http = xalloc(sizeof(FakeHttp));
    Providers providers;
    http->status = 200;
    http->body = "{}";
    sub_path(ini, COUNTOF(ini), directory, L"sabnzbd.ini");
    write_text(ini, "[misc]\nport = 8080\napi_key = test-sab-key\n");
    providers_init(&providers, fake_send, http);
    providers.sabnzbd_config_override = ini;
    providers.pulsarr_data_override = data;
    UpdateProvider provider = providers_interface(&providers);
    ULONGLONG deadline = GetTickCount64() + 60000;

    program_defaults(program, APP_RADARR);
    str_copy(program->api_key, KEYN, L"test-token");
    http->body = "[{\"name\":\"RefreshMonitoredDownloads\",\"status\":\"started\"},{\"name\":\"MoviesSearch\",\"commandName\":\"Movies Search\",\"status\":\"completed\"}]";
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_IDLE, L"Routine Radarr housekeeping does not count as in use");
    http->body = "[{\"name\":\"MoviesSearch\",\"commandName\":\"Movies Search\",\"status\":\"started\"}]";
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_IN_USE && wcsstr(usage.message, L"Movies Search"), L"A running Radarr search counts as in use");
    http->body = "{\"queue\":{\"paused\":false,\"noofslots_total\":3}}";
    program_defaults(program, APP_SABNZBD);
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_IN_USE, L"An active SABnzbd download counts as in use");
    check(url_parse(http->last_url, url) && wcscmp(url->path, L"/sabnzbd/api") == 0 && wcsstr(url->query, L"mode=queue"),
        L"SABnzbd is checked through its local API");
    http->body = "{\"MediaContainer\":{\"size\":0}}";
    program_defaults(program, APP_PLEX);
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_IDLE, L"Plex with no streams is idle");
    http->status = 401;
    program_defaults(program, APP_PULSARR);
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_UNKNOWN && wcsstr(usage.message, L"API key") && url_parse(http->last_url, url) && url->port == 3100,
        L"Pulsarr asks for an API key when its check is refused");
    http->status = 200;
    http->refuse = true;
    program_defaults(program, APP_PLEX);
    provider.check_usage(provider.context, program, deadline, &usage);
    check(usage.state == USAGE_IDLE, L"An app that is not running is not in use");

    memset(http, 0, sizeof(*http));
    http->feed = true;
    http->status = 200;
    http->installed_version = L"1.0.0";
    http->available_version = L"2.0.0";
    http->installable = true;
    program_defaults(program, APP_RADARR);
    str_copy(program->server_url, URLN, L"http://localhost:7878/radarr");
    str_copy(program->api_key, KEYN, L"test-token");
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_UPDATE_AVAILABLE && http->path_count == 2 &&
        wcscmp(http->paths[0], L"/radarr/api/v3/system/status") == 0 && wcscmp(http->paths[1], L"/radarr/api/v3/update") == 0,
        L"Arr checks preserve URL base paths and recognize a newer release");
    check(http->authenticated == 2 && http->posts == 0, L"Read-only provider checks authenticate without posting an update command");
    http->installable = false;
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_NEEDS_SETUP, L"A newer release blocked by the app is not reported as up to date");
    http->installable = true;
    http->available_version = L"0.9.0";
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_UP_TO_DATE, L"A lower available version cannot trigger a downgrade");
    http->docker = true;
    http->path_count = 0;
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_NEEDS_SETUP && http->path_count == 1, L"Container installations are identified and excluded from native updates");
    http->docker = false;
    http->installed_version = L"invalid";
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_ERROR, L"Malformed server versions are rejected");
    str_copy(program->server_url, URLN, L"http://user:secret@localhost:7878");
    http->path_count = 0;
    provider.check(provider.context, program, deadline, result);
    check(result->state == STATE_ERROR && http->path_count == 0 && !wcsstr(result->message, L"secret"),
        L"Embedded URL credentials are rejected before sending requests");

    free(http);
    free(sab);
    free(found);
    free(program);
    free(url);
    free(result);
}

static void test_self_update(void)
{
    static const char digest[] = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char address[] = "https://github.com/bommerts/Media-Center-Updater/releases/download/v0.3.0/MediaCenterUpdater-0.3.0-Setup.exe";
    static const char setup[] = "MediaCenterUpdater-0.3.0-Setup.exe";
    char json[1024];
    wchar_t expected[80];
    AppSettings *defaults = xalloc(sizeof(AppSettings));
    SelfUpdateInfo info;

    settings_defaults(defaults);
    check(defaults->app_update_check && !defaults->app_update_install,
        L"App update checks are on by default, automatic self-install requires opt-in");
    wmemset(expected, L'A', 64);
    expected[64] = 0;
    check(parse_release_text(release_json(json, sizeof(json), "v0.3.0", setup, digest, address, false), L"0.2.4", &info) == SELF_UPDATE_FOUND &&
        wcscmp(info.version, L"0.3.0") == 0 && wcscmp(info.sha256, expected) == 0 &&
        wcscmp(info.download_url, L"https://github.com/bommerts/Media-Center-Updater/releases/download/v0.3.0/MediaCenterUpdater-0.3.0-Setup.exe") == 0,
        L"A newer app release is found with its setup program and checksum");
    check(parse_release_text(release_json(json, sizeof(json), "v0.2.4", "MediaCenterUpdater-0.2.4-Setup.exe", digest, address, false), L"0.2.4", &info) == SELF_UPDATE_CURRENT,
        L"The same app version is not offered as an update");
    check(parse_release_text(release_json(json, sizeof(json), "v0.2.3", "MediaCenterUpdater-0.2.3-Setup.exe", digest, address, false), L"0.2.4", &info) == SELF_UPDATE_CURRENT,
        L"An older app release cannot trigger a downgrade");
    check(parse_release_text(release_json(json, sizeof(json), "v0.3.0", setup, digest, address, true), L"0.2.4", &info) == SELF_UPDATE_FAILED,
        L"Pre-releases are never installed");
    check(parse_release_text(release_json(json, sizeof(json), "v0.3.0", setup, "", address, false), L"0.2.4", &info) == SELF_UPDATE_FAILED,
        L"A release without a SHA-256 checksum is not installed");
    check(parse_release_text(release_json(json, sizeof(json), "v0.3.0", "Other-0.3.0.exe", digest, address, false), L"0.2.4", &info) == SELF_UPDATE_FAILED,
        L"A release without the expected setup program is not installed");
    check(parse_release_text(release_json(json, sizeof(json), "v0.3.0", setup, digest, "https://example.invalid/MediaCenterUpdater-0.3.0-Setup.exe", false), L"0.2.4", &info) == SELF_UPDATE_FAILED,
        L"A setup program from another site is not installed");
    check(!self_update_is_allowed_download(L"https://github.com/someone-else/Media-Center-Updater/releases/download/v1/MediaCenterUpdater-1.0.0-Setup.exe"),
        L"A setup program from another GitHub repository is not installed");
    check(!self_update_is_allowed_download(L"https://github.com/bommerts/Media-Center-Updater/releases/download/../../../../someone-else/x/releases/download/v1/MediaCenterUpdater-1.0.0-Setup.exe") &&
        !self_update_is_allowed_download(L"https://github.com@example.invalid/bommerts/Media-Center-Updater/releases/download/v1/MediaCenterUpdater-1.0.0-Setup.exe"),
        L"A download address cannot step outside this app's releases");

    StrBuf script;
    sb_init(&script);
    self_update_installer_script(&script, L"C:\\Users\\O'Brien\\AppData\\MediaCenterUpdater-0.3.0-Setup.exe", expected, false, 4242);
    check(wcsstr(script.p, L"Wait-Process -Id 4242 ") && wcsstr(script.p, L"$file = 'C:\\Users\\O''Brien\\AppData\\MediaCenterUpdater-0.3.0-Setup.exe'\n") &&
        wcsstr(script.p, L".Hash -ne 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA') { exit 1 }") &&
        wcsstr(script.p, L"@('/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/RELAUNCH=window', '/LOG=\"C:\\Users\\O''Brien\\AppData\\MediaCenterUpdater-0.3.0-Setup.log\"')"),
        L"The update helper waits for the app, rechecks the checksum, and quotes paths safely");
    script.len = 0;
    self_update_installer_script(&script, L"C:\\Data\\Setup.exe", expected, true, 1);
    check(wcsstr(script.p, L"'/VERYSILENT'") && wcsstr(script.p, L"'/RELAUNCH=tray'"), L"An update started from the notification area stays hidden and reopens there");
    sb_free(&script);
    free(defaults);
}

/* A small file last changed some hours ago. */
static void write_aged(const wchar_t *folder, const wchar_t *name, int hours_old)
{
    wchar_t path[PATHN];
    int64_t ticks = time_now() - hours_old * TICKS_PER_HOUR;
    FILETIME changed = { (DWORD)ticks, (DWORD)(ticks >> 32) };
    sub_path(path, COUNTOF(path), folder, name);
    write_text(path, "log");
    HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetFileTime(file, NULL, NULL, &changed);
    CloseHandle(file);
}

static bool has_file(const wchar_t *folder, const wchar_t *name)
{
    wchar_t path[PATHN];
    sub_path(path, COUNTOF(path), folder, name);
    return file_exists(path);
}

static void test_cleanup(void)
{
    wchar_t data[PATHN], downloads[PATHN], updates[PATHN];
    sub_path(data, COUNTOF(data), directory, L"cleanup");
    sub_path(downloads, COUNTOF(downloads), data, L"Downloads");
    sub_path(updates, COUNTOF(updates), data, L"AppUpdates");
    providers_prune_downloads(data);
    self_update_prune(data);
    check(!dir_exists(downloads) && !dir_exists(updates), L"Cleaning up a data folder that has no downloads yet does nothing");

    dir_create(downloads);
    dir_create(updates);
    write_aged(downloads, L"Pulsarr-a.log", 500);
    write_aged(downloads, L"Pulsarr-b.log", 400);
    write_aged(downloads, L"Pulsarr-c.log", 300);
    write_aged(downloads, L"Pulsarr-d.log", 200);
    write_aged(downloads, L"Pulsarr-e.log", 1);
    write_aged(downloads, L"Plex-a.log", 900);
    write_aged(downloads, L"Plex-b.log", 800);
    write_aged(downloads, L"Pulsarr-old.exe", 48);
    write_aged(downloads, L"Pulsarr-new.exe", 1);
    write_aged(downloads, L"notes.txt", 900);
    providers_prune_downloads(data);
    check(!has_file(downloads, L"Pulsarr-a.log") && !has_file(downloads, L"Pulsarr-b.log") && has_file(downloads, L"Pulsarr-c.log") &&
        has_file(downloads, L"Pulsarr-d.log") && has_file(downloads, L"Pulsarr-e.log"),
        L"Only the newest three installation logs of an app are kept");
    check(has_file(downloads, L"Plex-a.log") && has_file(downloads, L"Plex-b.log") && has_file(downloads, L"notes.txt"),
        L"Each app keeps its own installation logs, and other files are left alone");
    check(!has_file(downloads, L"Pulsarr-old.exe") && has_file(downloads, L"Pulsarr-new.exe"),
        L"An installer left behind is removed after a day, and a recent one is kept in case it is still running");

    write_aged(updates, L"MediaCenterUpdater-0.3.0-Setup.log", 900);
    write_aged(updates, L"MediaCenterUpdater-0.3.1-Setup.log", 800);
    write_aged(updates, L"MediaCenterUpdater-0.4.0-Setup.log", 700);
    write_aged(updates, L"MediaCenterUpdater-0.4.1-Setup.log", 600);
    write_aged(updates, L"MediaCenterUpdater-0.4.0-Setup.exe", 600);
    write_aged(updates, L"MediaCenterUpdater-0.4.1-Setup.exe", 0);
    self_update_prune(data);
    check(!has_file(updates, L"MediaCenterUpdater-0.3.0-Setup.log") && has_file(updates, L"MediaCenterUpdater-0.3.1-Setup.log") &&
        has_file(updates, L"MediaCenterUpdater-0.4.1-Setup.log") && !has_file(updates, L"MediaCenterUpdater-0.4.0-Setup.exe") &&
        has_file(updates, L"MediaCenterUpdater-0.4.1-Setup.exe"),
        L"This app's own setup logs and setup programs are cleaned up the same way");
}

int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 1; i < argc; i++)
    {
        if (wcscmp(argv[i], L"--inspect-local") == 0) return inspect_local();
        if (wcscmp(argv[i], L"--inspect-usage") == 0) return inspect_usage();
        if (wcscmp(argv[i], L"--inspect-pulsarr-key") == 0) return inspect_pulsarr_key();
    }
    if (argc == 3 && wcscmp(argv[1], L"--inspect-self-update") == 0) return inspect_self_update(argv[2]);

    wchar_t temp[PATHN], guid[40], parent[PATHN];
    GetTempPathW(PATHN, temp);
    new_guid_text(guid, COUNTOF(guid));
    path_join(parent, COUNTOF(parent), temp, L"MediaCenterUpdater.Tests");
    path_join(directory, COUNTOF(directory), parent, guid);
    if (!dir_create(directory))
    {
        print(L"FAIL could not create the test folder");
        return 1;
    }
    test_settings();
    test_coordinator();
    test_discovery();
    test_usage();
    test_self_update();
    test_cleanup();
    print(L"All %d assertions passed. No real updates were installed.", passed);
    remove_tree(directory);
    return 0;
}
