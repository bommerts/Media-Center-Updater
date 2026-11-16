#include "settings.h"
#include "json.h"
#include <wincrypt.h>

void store_init(SettingsStore *store, const wchar_t *data_directory)
{
    memset(store, 0, sizeof(*store));
    if (data_directory) str_copy(store->data_directory, COUNTOF(store->data_directory), data_directory);
    else app_data_directory(store->data_directory, COUNTOF(store->data_directory));
    dir_create(store->data_directory);
    InitializeCriticalSection(&store->lock);
}

void store_destroy(SettingsStore *store)
{
    DeleteCriticalSection(&store->lock);
}

/* ---- API key encryption (Windows data protection, current user) ---- */

static bool protect_key(const wchar_t *plain, StrBuf *out)
{
    size_t length;
    char *utf8 = wide_to_utf8(plain, &length);
    DATA_BLOB input = { (DWORD)length, (BYTE *)utf8 }, output = { 0 };
    bool ok = CryptProtectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output) != 0;
    if (ok)
    {
        DWORD chars = 0;
        CryptBinaryToStringW(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
        wchar_t *encoded = xalloc(((size_t)chars + 1) * sizeof(wchar_t));
        ok = CryptBinaryToStringW(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &chars) != 0;
        if (ok)
        {
            sb_append(out, L"dpapi:");
            sb_append(out, encoded);
        }
        free(encoded);
        LocalFree(output.pbData);
    }
    SecureZeroMemory(utf8, length);
    free(utf8);
    return ok;
}

static bool unprotect_key(const wchar_t *stored, wchar_t *plain, size_t cap)
{
    if (wcsncmp(stored, L"dpapi:", 6) != 0) return false;
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(stored + 6, 0, CRYPT_STRING_BASE64, NULL, &bytes, NULL, NULL) || !bytes) return false;
    BYTE *decoded = xalloc(bytes);
    bool ok = CryptStringToBinaryW(stored + 6, 0, CRYPT_STRING_BASE64, decoded, &bytes, NULL, NULL) != 0;
    DATA_BLOB input = { bytes, decoded }, output = { 0 };
    ok = ok && CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (ok)
    {
        wchar_t *wide = utf8_to_wide((const char *)output.pbData, (int)output.cbData);
        ok = wcslen(wide) < cap;
        if (ok) str_copy(plain, cap, wide);
        SecureZeroMemory(wide, wcslen(wide) * sizeof(wchar_t));
        free(wide);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    free(decoded);
    return ok;
}

/* ---- Reading. A value of the wrong type makes the whole file invalid. ---- */

static void read_bool(const JsonValue *object, const wchar_t *name, bool *value, bool *bad)
{
    const JsonValue *member = json_get(object, name);
    if (!member) return;
    if (member->kind == JSON_TRUE) *value = true;
    else if (member->kind == JSON_FALSE) *value = false;
    else *bad = true;
}

static void read_int(const JsonValue *object, const wchar_t *name, int *value, bool *bad)
{
    const JsonValue *member = json_get(object, name);
    if (member && !json_int(member, value)) *bad = true;
}

static void read_string(const JsonValue *object, const wchar_t *name, wchar_t *value, size_t cap, bool *bad)
{
    const JsonValue *member = json_get(object, name);
    if (!member) return;
    if (member->kind == JSON_STRING) str_copy(value, cap, member->text);
    else if (member->kind == JSON_NULL) value[0] = 0;
    else *bad = true;
}

static void read_time(const JsonValue *object, const wchar_t *name, DateTimeOffset *value, bool *bad)
{
    const JsonValue *member = json_get(object, name);
    if (!member) return;
    if (member->kind == JSON_NULL) value->has = false;
    else if (member->kind != JSON_STRING || !dto_parse(member->text, value)) *bad = true;
}

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

static bool parse_settings(const JsonValue *root, AppSettings *settings, bool *key_failed)
{
    if (!root || root->kind != JSON_OBJECT) return false;
    bool bad = false;
    int mode = 0, unit = INTERVAL_HOURS, hours = 0;
    settings_defaults(settings);
    read_int(root, L"SettingsVersion", &settings->settings_version, &bad);
    read_string(root, L"Theme", settings->theme, COUNTOF(settings->theme), &bad);
    read_bool(root, L"MinimizeToTray", &settings->minimize_to_tray, &bad);
    read_bool(root, L"StartMinimized", &settings->start_minimized, &bad);
    read_bool(root, L"ScheduleEnabled", &settings->schedule_enabled, &bad);
    read_int(root, L"ScheduleMode", &mode, &bad);
    /* Earlier versions saved the interval as whole hours, from 1 to 168. */
    if (json_get(root, L"IntervalMinutes"))
    {
        read_int(root, L"IntervalMinutes", &settings->interval_minutes, &bad);
        read_int(root, L"IntervalUnit", &unit, &bad);
    }
    else if (json_get(root, L"IntervalHours"))
    {
        read_int(root, L"IntervalHours", &hours, &bad);
        settings->interval_minutes = clamp(hours, 1, 168) * 60;
    }
    read_int(root, L"DailyHour", &settings->daily_hour, &bad);
    read_int(root, L"DailyMinute", &settings->daily_minute, &bad);
    read_int(root, L"DailyDays", &settings->daily_days, &bad);
    read_time(root, L"LastScheduledRun", &settings->last_scheduled_run, &bad);
    read_time(root, L"NextRun", &settings->next_run, &bad);
    read_bool(root, L"AppUpdateCheck", &settings->app_update_check, &bad);
    read_bool(root, L"AppUpdateInstall", &settings->app_update_install, &bad);
    read_time(root, L"LastAppUpdateCheck", &settings->last_app_update_check, &bad);

    /* Keep the saved order, drop unknown or repeated apps, then add any that are missing. */
    const JsonValue *programs = json_get(root, L"Programs");
    if (programs)
    {
        if (programs->kind != JSON_ARRAY && programs->kind != JSON_NULL) return false;
        settings->program_count = 0;
        for (const JsonValue *item = programs->first; item; item = item->next)
        {
            if (item->kind == JSON_NULL) continue;
            if (item->kind != JSON_OBJECT) return false;
            ProgramSettings program;
            int id = 0;
            memset(&program, 0, sizeof(program));
            program.enabled = program.auto_install = program.wait_while_in_use = true;
            read_int(item, L"Id", &id, &bad);
            read_bool(item, L"Enabled", &program.enabled, &bad);
            read_bool(item, L"AutoInstall", &program.auto_install, &bad);
            read_string(item, L"ServerUrl", program.server_url, COUNTOF(program.server_url), &bad);
            const JsonValue *key = json_get(item, L"ApiKey");
            if (key && key->kind == JSON_STRING)
            {
                if (key->text[0] && !unprotect_key(key->text, program.api_key, COUNTOF(program.api_key)))
                {
                    program.api_key[0] = 0;
                    *key_failed = true;
                }
            }
            else if (key && key->kind != JSON_NULL) bad = true;
            read_bool(item, L"WaitWhileInUse", &program.wait_while_in_use, &bad);
            if (id < 0 || id >= APP_COUNT || settings_program(settings, (ApplicationId)id)) continue;
            program.id = (ApplicationId)id;
            settings->programs[settings->program_count++] = program;
        }
        for (int id = 0; id < APP_COUNT; id++)
            if (!settings_program(settings, (ApplicationId)id))
                program_defaults(&settings->programs[settings->program_count++], (ApplicationId)id);
    }
    if (bad) return false;

    settings->schedule_mode = mode == SCHEDULE_DAILY ? SCHEDULE_DAILY : SCHEDULE_INTERVAL;
    settings->interval_unit = unit >= 0 && unit < INTERVAL_UNIT_COUNT ? (IntervalUnit)unit : INTERVAL_HOURS;
    settings_normalize_schedule(settings);
    if (wcscmp(settings->theme, L"Dark") != 0 && wcscmp(settings->theme, L"Light") != 0 && wcscmp(settings->theme, L"System") != 0)
        str_copy(settings->theme, COUNTOF(settings->theme), L"Dark");
    /* Version 0.3.0 saved "wait while in use" as off. It is now on by default so updates never interrupt an app. */
    if (settings->settings_version < 1)
        for (int i = 0; i < settings->program_count; i++) settings->programs[i].wait_while_in_use = true;
    settings->settings_version = SETTINGS_VERSION;
    return true;
}

void store_load(SettingsStore *store, AppSettings *settings)
{
    wchar_t path[PATHN];
    char *data;
    size_t length;
    store->load_warning[0] = 0;
    settings_defaults(settings);
    path_join(path, COUNTOF(path), store->data_directory, L"settings.json");
    FileRead read = file_read_all(path, 16 * 1024 * 1024, &data, &length);
    if (read == FR_MISSING) return;

    JsonDoc *doc = read == FR_OK ? json_parse_utf8(data, length) : NULL;
    free(data);
    bool key_failed = false;
    bool ok = doc && parse_settings(doc->root, settings, &key_failed);
    json_free(doc);
    if (!ok)
    {
        /* Keep the original file for diagnosis; never silently resume an unknown schedule. */
        settings_defaults(settings);
        str_copy(store->load_warning, COUNTOF(store->load_warning),
            L"Saved settings could not be loaded. The schedule is off. Review and save your settings.");
        return;
    }
    if (key_failed)
    {
        settings->schedule_enabled = false;
        str_copy(store->load_warning, COUNTOF(store->load_warning),
            L"A saved API key could not be read. Re-enter the key and enable the schedule again.");
    }
}

/* ---- Writing ---- */

static void write_time(StrBuf *sb, const wchar_t *name, const DateTimeOffset *value)
{
    wchar_t text[64];
    sb_appendf(sb, L"  \"%s\": ", name);
    if (value->has)
    {
        dto_format(value, text, COUNTOF(text));
        json_write_string(sb, text);
    }
    else sb_append(sb, L"null");
    sb_append(sb, L",\n");
}

static const wchar_t *bool_text(bool value) { return value ? L"true" : L"false"; }

bool store_save(SettingsStore *store, const AppSettings *settings, wchar_t *error, size_t error_cap)
{
    StrBuf sb;
    bool ok = true;
    sb_init(&sb);
    sb_appendf(&sb, L"{\n  \"SettingsVersion\": %d,\n  \"Theme\": ", SETTINGS_VERSION);
    json_write_string(&sb, settings->theme);
    sb_appendf(&sb, L",\n  \"MinimizeToTray\": %s,\n  \"StartMinimized\": %s,\n  \"ScheduleEnabled\": %s,\n"
        L"  \"ScheduleMode\": %d,\n  \"IntervalMinutes\": %d,\n  \"IntervalUnit\": %d,\n  \"DailyHour\": %d,\n  \"DailyMinute\": %d,\n"
        L"  \"DailyDays\": %d,\n",
        bool_text(settings->minimize_to_tray), bool_text(settings->start_minimized), bool_text(settings->schedule_enabled),
        (int)settings->schedule_mode, settings->interval_minutes, (int)settings->interval_unit, settings->daily_hour,
        settings->daily_minute, settings->daily_days);
    write_time(&sb, L"LastScheduledRun", &settings->last_scheduled_run);
    write_time(&sb, L"NextRun", &settings->next_run);
    sb_appendf(&sb, L"  \"AppUpdateCheck\": %s,\n  \"AppUpdateInstall\": %s,\n",
        bool_text(settings->app_update_check), bool_text(settings->app_update_install));
    write_time(&sb, L"LastAppUpdateCheck", &settings->last_app_update_check);
    sb_append(&sb, L"  \"Programs\": [");
    for (int i = 0; i < settings->program_count && ok; i++)
    {
        const ProgramSettings *program = &settings->programs[i];
        sb_appendf(&sb, L"%s\n    {\n      \"Id\": %d,\n      \"Enabled\": %s,\n      \"AutoInstall\": %s,\n      \"ServerUrl\": ",
            i ? L"," : L"", (int)program->id, bool_text(program->enabled), bool_text(program->auto_install));
        json_write_string(&sb, program->server_url);
        sb_append(&sb, L",\n      \"ApiKey\": \"");
        if (program->api_key[0]) ok = protect_key(program->api_key, &sb);
        sb_appendf(&sb, L"\",\n      \"WaitWhileInUse\": %s\n    }", bool_text(program->wait_while_in_use));
    }
    sb_append(&sb, L"\n  ]\n}");

    if (!ok) str_copy(error, error_cap, L"Windows could not encrypt an API key.");
    else
    {
        wchar_t path[PATHN], temporary[PATHN];
        size_t length;
        char *utf8 = wide_to_utf8(sb.p, &length);
        path_join(path, COUNTOF(path), store->data_directory, L"settings.json");
        str_fmt(temporary, COUNTOF(temporary), L"%s.tmp", path);
        EnterCriticalSection(&store->lock);
        ok = file_write_all(temporary, utf8, length) && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING);
        DWORD code = GetLastError();
        LeaveCriticalSection(&store->lock);
        free(utf8);
        if (!ok)
        {
            wchar_t reason[300] = L"";
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, reason, COUNTOF(reason), NULL);
            str_trim(reason);
            str_fmt(error, error_cap, L"The settings file could not be written. %s", reason);
        }
    }
    SecureZeroMemory(sb.p, sb.len * sizeof(wchar_t));
    sb_free(&sb);
    return ok;
}

/* ---- Activity log ---- */

bool store_log(SettingsStore *store, const ActivityEntry *entry)
{
    wchar_t path[PATHN], previous[PATHN], time[64];
    StrBuf sb;
    sb_init(&sb);
    dto_format(&entry->time, time, COUNTOF(time));
    sb_append(&sb, L"{\"Time\":");
    json_write_string(&sb, time);
    sb_append(&sb, L",\"Application\":");
    json_write_string(&sb, entry->application);
    sb_append(&sb, L",\"Message\":");
    json_write_string(&sb, entry->message);
    sb_appendf(&sb, L",\"IsError\":%s}\r\n", bool_text(entry->is_error));
    size_t length;
    char *utf8 = wide_to_utf8(sb.p, &length);
    sb_free(&sb);

    path_join(path, COUNTOF(path), store->data_directory, L"activity.jsonl");
    path_join(previous, COUNTOF(previous), store->data_directory, L"activity.previous.jsonl");
    EnterCriticalSection(&store->lock);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) &&
        (((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow) > 2000000)
        MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
    bool ok = file_append(path, utf8, length);
    LeaveCriticalSection(&store->lock);
    free(utf8);
    return ok;
}

int store_read_recent_activity(SettingsStore *store, ActivityEntry *entries, int max_entries)
{
    enum { LINES = 100 };
    wchar_t path[PATHN];
    char *data;
    size_t length;
    path_join(path, COUNTOF(path), store->data_directory, L"activity.jsonl");
    if (file_read_all(path, 64 * 1024 * 1024, &data, &length) != FR_OK) return 0;

    /* Find where the last 100 lines start. */
    size_t start = length;
    int lines = 0;
    while (start > 0)
    {
        if (data[start - 1] == '\n' && start != length && ++lines == LINES) break;
        start--;
    }

    ActivityEntry *recent = xalloc(sizeof(ActivityEntry) * LINES);
    int count = 0;
    for (size_t at = start; at < length && count < LINES;)
    {
        size_t end = at;
        while (end < length && data[end] != '\n') end++;
        JsonDoc *doc = json_parse_utf8(data + at, end - at);
        if (doc && doc->root->kind == JSON_OBJECT)
        {
            ActivityEntry *entry = &recent[count];
            memset(entry, 0, sizeof(*entry));
            if (dto_parse(json_string(doc->root, L"Time"), &entry->time))
            {
                str_copy(entry->application, COUNTOF(entry->application), json_string(doc->root, L"Application"));
                str_copy(entry->message, COUNTOF(entry->message), json_string(doc->root, L"Message"));
                entry->is_error = json_is_true(doc->root, L"IsError");
                count++;
            }
        }
        json_free(doc);
        at = end + 1;
    }
    free(data);

    /* Newest first. Entries with the same time keep the order they were written in, latest first. */
    int written = 0;
    bool *used = xalloc(sizeof(bool) * LINES);
    while (written < max_entries && written < count)
    {
        int best = -1;
        for (int i = count - 1; i >= 0; i--)
            if (!used[i] && (best < 0 || recent[i].time.ticks > recent[best].time.ticks)) best = i;
        used[best] = true;
        entries[written++] = recent[best];
    }
    free(used);
    free(recent);
    return written;
}
