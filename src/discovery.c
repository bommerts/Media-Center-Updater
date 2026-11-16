#include "discovery.h"
#include "http.h"
#include "system.h"
#include "xml.h"

#define PULSARR_INSTALLER_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B8A42C5E-7D91-4F8C-B5E3-9A7C6D8E2F10}_is1"
#define MAXIMUM_CONFIG_LENGTH 1000000

static void missing(KeyDiscovery *out, const wchar_t *format, ...)
{
    va_list args;
    out->found = false;
    out->api_key[0] = 0;
    va_start(args, format);
    str_vfmt(out->message, COUNTOF(out->message), format, args);
    va_end(args);
}

static const wchar_t *child_text(const XmlNode *config, const wchar_t *name, wchar_t *dst, size_t cap)
{
    const XmlNode *child = xml_child(config, name);
    str_copy(dst, cap, child ? child->text : L"");
    str_trim(dst);
    return child ? dst : NULL;
}

static bool endpoint_matches(ApplicationId id, const Url *address, const XmlNode *config)
{
    wchar_t text[URLN], path[URLN];
    bool use_ssl = wcscmp(address->scheme, L"https") == 0;
    if (use_ssl && (!child_text(config, L"EnableSsl", text, COUNTOF(text)) || !str_ieq(text, L"true"))) return false;
    int port = id == APP_RADARR ? (use_ssl ? 9898 : 7878) : (use_ssl ? 9898 : 8989);
    if (child_text(config, use_ssl ? L"SslPort" : L"Port", text, COUNTOF(text)) && text[0])
    {
        port = 0;
        for (const wchar_t *p = text; *p; p++)
        {
            if (*p < L'0' || *p > L'9' || port > 65535) return false;
            port = port * 10 + (*p - L'0');
        }
        if (port < 1 || port > 65535) return false;
    }
    child_text(config, L"UrlBase", text, COUNTOF(text));
    str_trim_chars(text, L"/");
    str_copy(path, COUNTOF(path), address->path);
    str_trim_chars(path, L"/");
    return address->port == port && wcscmp(path, text) == 0;
}

void discover_api_key(ApplicationId id, const wchar_t *server_url, const wchar_t *data_root, KeyDiscovery *out)
{
    memset(out, 0, sizeof(*out));
    if (id != APP_RADARR && id != APP_SONARR)
    {
        missing(out, L"Automatic key retrieval is available for Radarr and Sonarr.");
        return;
    }
    const wchar_t *name = app_name(id);
    wchar_t trimmed[URLN], computer[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD computer_size = COUNTOF(computer);
    Url *address = xalloc(sizeof(Url));
    str_copy(trimmed, COUNTOF(trimmed), server_url);
    str_trim(trimmed);
    if (!url_parse(trimmed, address) || !url_is_http(address) || address->userinfo[0] || address->query[0] || address->fragment[0])
    {
        missing(out, L"Enter a valid HTTP or HTTPS address for %s, then try retrieving its key again.", name);
        free(address);
        return;
    }
    GetComputerNameW(computer, &computer_size);
    if (!url_is_loopback(address) && !str_ieq(address->host, computer))
    {
        missing(out, L"%s uses another computer's address. Automatic retrieval only reads keys on this PC; enter that application's key yourself.", name);
        free(address);
        return;
    }

    wchar_t root[PATHN], folder[PATHN], path[PATHN];
    if (data_root) str_copy(root, COUNTOF(root), data_root);
    else folder_program_data(root, COUNTOF(root));
    path_join(folder, COUNTOF(folder), root, name);
    path_join(path, COUNTOF(path), folder, L"config.xml");

    char *data;
    size_t length;
    FileRead read = file_read_all(path, MAXIMUM_CONFIG_LENGTH, &data, &length);
    if (read == FR_MISSING)
        missing(out, L"%s's settings were not found in the usual location on this PC. Start %s and try again, or enter its key yourself.", name, name);
    else if (read == FR_TOO_LARGE)
        missing(out, L"%s's settings file could not be read safely. Enter its key yourself.", name);
    else
    {
        /* File and XML problems are reported without detail: the text may hold configuration values or private paths. */
        XmlDoc *doc = read == FR_OK ? xml_parse(data, length) : NULL;
        wchar_t *key = xalloc(sizeof(wchar_t) * (MAXIMUM_CONFIG_LENGTH + 1));
        if (!doc)
            missing(out, L"Could not read %s's settings on this PC. Try retrieving its key again, or copy it from Settings > General > Security.", name);
        else if (!endpoint_matches(id, address, doc->root))
            missing(out, L"The %s address does not match the copy installed on this PC. Check its address or enter its key yourself.", name);
        else
        {
            child_text(doc->root, L"ApiKey", key, MAXIMUM_CONFIG_LENGTH + 1);
            if (!key[0])
                missing(out, L"%s's settings were found, but they do not contain an API key. Start %s and try again, or copy its key from Settings > General > Security.", name, name);
            else if (wcslen(key) > 1024 || str_has_control(key))
                missing(out, L"%s's saved API key could not be read safely. Copy its key from Settings > General > Security.", name);
            else
            {
                out->found = true;
                str_copy(out->api_key, COUNTOF(out->api_key), key);
                str_fmt(out->message, COUNTOF(out->message), L"%s's API key was found in its settings on this PC.", name);
            }
        }
        free(key);
        xml_free(doc);
    }
    free(data);
    free(address);
}

/* ---- Pulsarr ---- */

void pulsarr_data_directory(wchar_t *dst, size_t cap)
{
    wchar_t root[PATHN];
    folder_program_data(root, COUNTOF(root));
    path_join(dst, cap, root, L"Pulsarr");
}

bool pulsarr_env_value(const wchar_t *data_directory, const wchar_t *key, wchar_t *value, size_t cap, bool *failed)
{
    wchar_t path[PATHN];
    char *data;
    size_t length;
    bool found = false;
    if (failed) *failed = false;
    if (cap) value[0] = 0;
    path_join(path, COUNTOF(path), data_directory, L".env");
    FileRead read = file_read_all(path, 4 * 1024 * 1024, &data, &length);
    if (read != FR_OK)
    {
        if (failed) *failed = read != FR_MISSING;
        return false;
    }
    wchar_t *text = utf8_to_wide(data, (int)length), *context = NULL;
    free(data);
    for (wchar_t *line = wcstok_s(text, L"\r\n", &context); line; line = wcstok_s(NULL, L"\r\n", &context))
    {
        str_trim(line);
        if (*line == 0xFEFF) line++;
        if (*line == L'#') continue;
        wchar_t *equals = wcschr(line, L'=');
        if (!equals || equals == line) continue;
        *equals = 0;
        str_trim(line);
        if (!str_ieq(line, key)) continue;
        str_trim(equals + 1);
        str_trim_chars(equals + 1, L"\"'");
        str_copy(value, cap, equals + 1);
        found = true;       /* a later line with the same key replaces an earlier one */
    }
    free(text);
    return found;
}

bool pulsarr_requires_api_key(const wchar_t *data_directory)
{
    wchar_t directory[PATHN], method[64];
    bool failed;
    if (data_directory) str_copy(directory, COUNTOF(directory), data_directory);
    else pulsarr_data_directory(directory, COUNTOF(directory));
    pulsarr_env_value(directory, L"authenticationMethod", method, COUNTOF(method), &failed);
    /* Pulsarr requires sign-in unless its .env says otherwise. */
    return failed || (wcscmp(method, L"disabled") != 0 && wcscmp(method, L"requiredExceptLocal") != 0);
}

/* Read-only access to Pulsarr's SQLite database through the SQLite library built into Windows.
   Returns 1 with the key, 0 when there is no key, -1 when the database cannot be read. */
static int read_active_api_key(const wchar_t *database_path, wchar_t *key, size_t cap)
{
    typedef int (*OpenFn)(const char *, void **, int, const char *);
    typedef int (*TimeoutFn)(void *, int);
    typedef int (*PrepareFn)(void *, const char *, int, void **, const char **);
    typedef int (*StepFn)(void *);
    typedef const unsigned char *(*TextFn)(void *, int);
    typedef int (*BytesFn)(void *, int);
    typedef int (*CloseFn)(void *);
    enum { OPEN_READ_ONLY = 1, OK = 0, ROW = 100, DONE = 101 };
    /* Pulsarr only accepts active keys that belong to an admin account. */
    static const char query[] = "SELECT k.key FROM api_keys k JOIN admin_users u ON u.id = k.user_id "
        "WHERE k.is_active = 1 ORDER BY k.created_at DESC, k.id DESC LIMIT 1";

    HMODULE library = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return -1;
    OpenFn open = (OpenFn)GetProcAddress(library, "sqlite3_open_v2");
    TimeoutFn busy_timeout = (TimeoutFn)GetProcAddress(library, "sqlite3_busy_timeout");
    PrepareFn prepare = (PrepareFn)GetProcAddress(library, "sqlite3_prepare_v2");
    StepFn step = (StepFn)GetProcAddress(library, "sqlite3_step");
    TextFn column_text = (TextFn)GetProcAddress(library, "sqlite3_column_text");
    BytesFn column_bytes = (BytesFn)GetProcAddress(library, "sqlite3_column_bytes");
    StepFn finalize = (StepFn)GetProcAddress(library, "sqlite3_finalize");
    CloseFn close = (CloseFn)GetProcAddress(library, "sqlite3_close_v2");
    int result = -1;
    if (open && busy_timeout && prepare && step && column_text && column_bytes && finalize && close)
    {
        void *database = NULL, *statement = NULL;
        char *path = wide_to_utf8(database_path, NULL);
        if (open(path, &database, OPEN_READ_ONLY, NULL) == OK)
        {
            busy_timeout(database, 3000);
            if (prepare(database, query, -1, &statement, NULL) == OK)
            {
                int stepped = step(statement);
                if (stepped == DONE) result = 0;
                else if (stepped == ROW)
                {
                    const unsigned char *text = column_text(statement, 0);
                    if (!text) result = 0;
                    else
                    {
                        wchar_t *wide = utf8_to_wide((const char *)text, column_bytes(statement, 0));
                        str_copy(key, cap, wide);
                        /* A key too long for the buffer is reported as unreadable by the caller. */
                        if (wcslen(wide) >= cap && cap > 1) wmemset(key, L'x', cap - 1);
                        free(wide);
                        result = 1;
                    }
                }
            }
        }
        if (statement) finalize(statement);
        if (database) close(database);
        free(path);
    }
    FreeLibrary(library);
    return result;
}

void discover_pulsarr_api_key(const wchar_t *data_directory, KeyDiscovery *out)
{
    wchar_t directory[PATHN], value[PATHN], database[PATHN], folder[PATHN];
    bool failed;
    memset(out, 0, sizeof(*out));
    if (data_directory) str_copy(directory, COUNTOF(directory), data_directory);
    else pulsarr_data_directory(directory, COUNTOF(directory));

    if (!dir_exists(directory))
    {
        missing(out, L"Pulsarr's data folder was not found on this PC. Start Pulsarr and try again, or paste a key from Utilities \x2192 API Keys.");
        return;
    }
    if (pulsarr_env_value(directory, L"dbType", value, COUNTOF(value), &failed) && str_ieq(value, L"postgres"))
    {
        missing(out, L"Pulsarr keeps its settings in PostgreSQL, so its key cannot be read automatically. Paste a key from Pulsarr's Utilities \x2192 API Keys.");
        return;
    }
    if (!failed && pulsarr_env_value(directory, L"dbPath", value, COUNTOF(value), &failed) && path_is_absolute(value))
        str_copy(database, COUNTOF(database), value);
    else
    {
        path_join(folder, COUNTOF(folder), directory, L"db");
        path_join(database, COUNTOF(database), folder, L"pulsarr.db");
    }
    if (failed)
    {
        /* Database and file errors can include paths, so only a general message is shown. */
        missing(out, L"Could not read Pulsarr's database on this PC. Try Find key again, or paste a key from Utilities \x2192 API Keys.");
        return;
    }
    if (!file_exists(database))
    {
        missing(out, L"Pulsarr's database was not found on this PC. Start Pulsarr and try again, or paste a key from Utilities \x2192 API Keys.");
        return;
    }

    wchar_t key[KEYN + 8] = L"";
    int result = read_active_api_key(database, key, COUNTOF(key));
    str_trim(key);
    if (result < 0)
        missing(out, L"Could not read Pulsarr's database on this PC. Try Find key again, or paste a key from Utilities \x2192 API Keys.");
    else if (result == 0 || !key[0])
        missing(out, L"Pulsarr does not have an API key yet. In Pulsarr, open Utilities \x2192 API Keys and generate a key, then choose Find key again.");
    else if (wcslen(key) > 1024 || str_has_control(key))
        missing(out, L"Pulsarr's saved API key could not be read safely. Paste a key from Utilities \x2192 API Keys.");
    else
    {
        out->found = true;
        str_copy(out->api_key, COUNTOF(out->api_key), key);
        str_copy(out->message, COUNTOF(out->message), L"Pulsarr's API key was found on this PC and saved securely.");
    }
    SecureZeroMemory(key, sizeof(key));
}

/* The program path at the start of a command line, or "" when it cannot be told. */
static void executable_from_command(const wchar_t *command, wchar_t *dst, size_t cap)
{
    wchar_t text[PATHN * 2];
    str_copy(text, COUNTOF(text), command);
    str_trim(text);
    dst[0] = 0;
    if (text[0] == L'"')
    {
        wchar_t *closing = wcschr(text + 1, L'"');
        if (closing && closing > text + 1) str_copy_n(dst, cap, text + 1, (size_t)(closing - text - 1));
        return;
    }
    const wchar_t *end = str_ifind(text, L".exe");
    if (end && (!end[4] || iswspace(end[4]))) str_copy_n(dst, cap, text, (size_t)(end - text) + 4);
}

void evaluate_pulsarr_setup(bool registered, const wchar_t *expected_executable, bool executable_exists,
    bool service_exists, const wchar_t *service_command, const wchar_t *selected_tasks, PulsarrSetup *out)
{
    wchar_t actual[PATHN];
    const wchar_t *message;
    out->supported = false;
    executable_from_command(service_command ? service_command : L"", actual, COUNTOF(actual));
    if (!registered)
        message = L"Pulsarr's official Windows installation was not found on this PC. Automatic updates require its Windows installer with the service option selected. Copies started by hand or through Docker need a different update method.";
    else if (!executable_exists)
        message = L"Pulsarr's Windows installation was found, but its background service program is missing. Run its official Windows installer with the service option selected before using automatic updates.";
    else if (!service_exists)
        message = L"Pulsarr is installed, but its Windows service was not found. Run its official Windows installer with the service option selected before using automatic updates.";
    else if (!path_same(expected_executable, actual))
        message = L"Pulsarr's Windows service points to a different or unrecognized copy. Its setup needs to be corrected with the official Windows installer before automatic updates can be used.";
    else if (!selected_tasks)
        message = L"Pulsarr's Windows service was found, but its saved installation choices could not be read. Run its official Windows installer once to save those choices before using automatic updates.";
    else
    {
        out->supported = true;
        message = L"This PC already meets the requirement: Pulsarr is installed using its official Windows installer and is set up as a Windows service. Its saved installation choices were found, so no reinstall is needed for automatic updates.";
    }
    str_copy(out->message, COUNTOF(out->message), message);
}

void check_pulsarr_setup(PulsarrSetup *out)
{
    static const HKEY hives[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    static const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (int h = 0; h < 2; h++)
    for (int v = 0; v < 2; v++)
    {
        if (!reg_key_exists(hives[h], PULSARR_INSTALLER_KEY, views[v])) continue;
        wchar_t directory[PATHN], uninstall[PATHN * 2], executable[PATHN] = L"", command[PATHN * 2], tasks[1024];
        reg_read_string(hives[h], PULSARR_INSTALLER_KEY, views[v], L"InstallLocation", directory, COUNTOF(directory));
        str_trim_chars(directory, L"\"");
        if (!directory[0])
        {
            wchar_t program[PATHN];
            reg_read_string(hives[h], PULSARR_INSTALLER_KEY, views[v], L"UninstallString", uninstall, COUNTOF(uninstall));
            executable_from_command(uninstall, program, COUNTOF(program));
            if (program[0]) path_dirname(directory, COUNTOF(directory), program);
        }
        if (path_is_absolute(directory)) path_join(executable, COUNTOF(executable), directory, L"pulsarr-service.exe");
        const wchar_t *service_key = L"SYSTEM\\CurrentControlSet\\Services\\pulsarr";
        bool service = reg_key_exists(HKEY_LOCAL_MACHINE, service_key, 0);
        bool has_command = service && reg_read_string(HKEY_LOCAL_MACHINE, service_key, 0, L"ImagePath", command, COUNTOF(command));
        bool has_tasks = reg_read_string(hives[h], PULSARR_INSTALLER_KEY, views[v], L"Inno Setup: Selected Tasks", tasks, COUNTOF(tasks));
        evaluate_pulsarr_setup(true, executable, executable[0] && file_exists(executable), service,
            has_command ? command : NULL, has_tasks ? tasks : NULL, out);
        return;
    }
    evaluate_pulsarr_setup(false, L"", false, false, NULL, NULL, out);
}
