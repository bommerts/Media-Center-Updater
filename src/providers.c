#include "providers.h"
#include "discovery.h"
#include "system.h"

#define PULSARR_KEY L"{B8A42C5E-7D91-4F8C-B5E3-9A7C6D8E2F10}_is1"
#define UNINSTALL_ROOT L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
#define DOWNLOAD_LIMIT 1500000000ULL

void providers_init(Providers *providers, HttpSendFn send, void *send_context)
{
    memset(providers, 0, sizeof(*providers));
    providers->send = send ? send : http_send;
    providers->send_context = send_context;
    str_copy(providers->plex_address, COUNTOF(providers->plex_address), L"http://127.0.0.1:32400/");
}

/* ---- Versions ---- */

static bool is_digit(wchar_t ch) { return ch >= L'0' && ch <= L'9'; }
static bool is_alnum(wchar_t ch) { return is_digit(ch) || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'); }

bool version_parse(const wchar_t *text, Version *out)
{
    wchar_t buffer[VERN * 2];
    str_copy(buffer, COUNTOF(buffer), text);
    str_trim(buffer);
    const wchar_t *p = buffer;
    while (*p == L'v') p++;
    memset(out, 0, sizeof(*out));
    int parts = 0;
    for (;;)
    {
        if (!is_digit(*p)) return false;
        int64_t value = 0;
        for (; is_digit(*p); p++)
        {
            value = value * 10 + (*p - L'0');
            if (value > INT_MAX) return false;
        }
        out->part[parts++] = (int)value;
        if (*p == L'.' && parts < 4 && is_digit(p[1])) { p++; continue; }
        break;
    }
    if (parts < 2) return false;
    if (!*p) return true;
    /* An optional build label such as "-beta.1" or "+abc123". */
    if ((*p != L'-' && *p != L'+') || !is_alnum(p[1])) return false;
    for (p++; *p; p++) if (!is_alnum(*p) && *p != L'.' && *p != L'-') return false;
    return true;
}

int version_compare(const Version *a, const Version *b)
{
    for (int i = 0; i < 4; i++)
        if (a->part[i] != b->part[i]) return a->part[i] < b->part[i] ? -1 : 1;
    return 0;
}

static bool compare_versions(const wchar_t *left, const wchar_t *right, int *comparison, Err *err)
{
    Version a, b;
    if (!version_parse(left, &a) || !version_parse(right, &b))
        return err_set(err, ERR_PROVIDER, L"A version could not be compared safely.");
    *comparison = version_compare(&a, &b);
    return true;
}

/* ---- Requests ---- */

static bool send_request(Providers *providers, const HttpRequest *request, HttpResponse *response, Err *err)
{
    return http_check(providers->send(providers->send_context, request, response), err);
}

static bool require_success(int status, Err *err)
{
    if (status >= 200 && status < 300) return true;
    if (status >= 300 && status < 400)
        return err_set(err, ERR_PROVIDER, L"The application address redirected. Enter its direct address; credentials are not forwarded to redirects.");
    if (status == 401 || status == 403)
        return err_set(err, ERR_PROVIDER, L"Access was denied. Check the API key or wait for the publisher's request limit to reset.");
    return err_set(err, ERR_PROVIDER, L"The update service returned HTTP %d.", status);
}

/* Builds a "Name: value" header line. A value with control characters could smuggle extra headers, so it is refused. */
static bool header_line(wchar_t *dst, size_t cap, const wchar_t *name, const wchar_t *value, Err *err)
{
    if (str_has_control(value)) return err_set(err, ERR_OTHER, L"The request could not be built.");
    str_fmt(dst, cap, L"%s: %s\r\n", name, value);
    return true;
}

static bool get_json(Providers *providers, const wchar_t *url, const wchar_t *api_key, JsonDoc **doc, Err *err)
{
    wchar_t headers[KEYN + 32];
    HttpRequest request = { 0 };
    HttpResponse response;
    *doc = NULL;
    request.url = url;
    request.timeout_ms = 30000;
    if (api_key && api_key[0])
    {
        if (!header_line(headers, COUNTOF(headers), L"X-Api-Key", api_key, err)) return false;
        request.headers = headers;
    }
    if (!send_request(providers, &request, &response, err)) return false;
    bool ok = require_success(response.status, err);
    if (ok)
    {
        *doc = json_parse_utf8(response.body, response.body_length);
        if (!*doc) ok = err_set(err, ERR_OTHER, L"The response was not understood.");
    }
    http_response_free(&response);
    return ok;
}

static const wchar_t *failure_message(const Err *err)
{
    switch (err->kind)
    {
    case ERR_PROVIDER: return err->msg;
    case ERR_TIMEOUT: return L"The update service timed out. The next check can retry.";
    case ERR_NETWORK: case ERR_CONNECT:
        return L"The update service could not be reached. Check the address and network connection.";
    case ERR_ACCESS: return L"Windows denied access. Check the application installation and administrator permissions.";
    default: return L"The update could not be completed safely. Check the installation, settings, and publisher availability.";
    }
}

static void failure(ApplicationId id, const Err *err, UpdateResult *out)
{
    result_set(out, id, STATE_ERROR, L"", L"", failure_message(err));
}

static bool is_hex(const wchar_t *text, size_t length)
{
    if (wcslen(text) != length) return false;
    for (; *text; text++)
        if (!is_digit(*text) && !(*text >= L'a' && *text <= L'f') && !(*text >= L'A' && *text <= L'F')) return false;
    return true;
}

/* ---- Radarr and Sonarr ---- */

static bool arr_connection(const ProgramSettings *settings, Url *url, wchar_t *key, size_t cap, Err *err)
{
    wchar_t text[URLN + 2];
    str_copy(text, URLN, settings->server_url);
    size_t length = wcslen(text);
    while (length && text[length - 1] == L'/') text[--length] = 0;
    str_cat(text, COUNTOF(text), L"/");
    if (!url_parse(text, url) || !url_is_http(url) || url->userinfo[0] || url->query[0] || url->fragment[0])
        return err_set(err, ERR_PROVIDER, L"Enter an HTTP or HTTPS application address without embedded credentials, a query, or a fragment.");
    str_copy(key, cap, settings->api_key);
    str_trim(key);
    if (!key[0])
    {
        KeyDiscovery *found = xalloc(sizeof(KeyDiscovery));
        discover_api_key(settings->id, settings->server_url, NULL, found);
        str_copy(key, cap, found->api_key);
        SecureZeroMemory(found, sizeof(*found));
        free(found);
    }
    return true;
}

static bool endpoint(const Url *base, const wchar_t *relative, wchar_t *dst, size_t cap, Err *err)
{
    Url *resolved = xalloc(sizeof(Url));
    bool ok = url_resolve(base, relative, resolved);
    if (ok) url_format(resolved, dst, cap);
    free(resolved);
    return ok ? true : err_set(err, ERR_OTHER, L"The address could not be built.");
}

static bool check_arr(Providers *providers, const ProgramSettings *settings, UpdateResult *out, Err *err)
{
    wchar_t key[KEYN], address[URLN], installed[VERN], version[VERN];
    Url *url = xalloc(sizeof(Url));
    JsonDoc *status = NULL, *updates = NULL;
    Version parsed;
    bool ok = false;
    if (!arr_connection(settings, url, key, COUNTOF(key), err)) goto done;
    if (str_blank(key))
    {
        result_set(out, settings->id, STATE_NEEDS_SETUP, L"", L"", L"This application's API key was not found automatically. In Settings, choose Retrieve API keys, or copy its key from the application's Settings > General > Security page.");
        ok = true;
        goto done;
    }
    if (!endpoint(url, L"api/v3/system/status", address, COUNTOF(address), err) ||
        !get_json(providers, address, key, &status, err))
        goto done;
    str_copy(installed, COUNTOF(installed), json_text(status->root, L"version"));
    if (!version_parse(installed, &parsed))
    {
        err_set(err, ERR_PROVIDER, L"The application returned an unrecognized installed version.");
        goto done;
    }
    const wchar_t *mechanism = json_text(status->root, L"packageUpdateMechanism");
    if (json_is_true(status->root, L"isDocker") || (mechanism[0] && !str_ieq(mechanism, L"builtIn") && wcscmp(mechanism, L"0") != 0))
    {
        result_set(out, settings->id, STATE_NEEDS_SETUP, installed, L"", L"This installation uses an external package or container updater. Its installation is left unchanged.");
        ok = true;
        goto done;
    }
    if (!endpoint(url, L"api/v3/update", address, COUNTOF(address), err) ||
        !get_json(providers, address, key, &updates, err))
        goto done;
    if (updates->root->kind != JSON_ARRAY)
    {
        err_set(err, ERR_OTHER, L"The response was not understood.");
        goto done;
    }
    const JsonValue *available = NULL;
    for (const JsonValue *item = updates->root->first; item && !available; item = item->next)
        if (json_is_true(item, L"latest")) available = item;
    if (!available)
    {
        result_set(out, settings->id, STATE_UP_TO_DATE, installed, installed, L"The application's update service reports no newer release.");
        ok = true;
        goto done;
    }
    str_copy(version, COUNTOF(version), json_text(available, L"version"));
    Version latest;
    if (!version_parse(version, &latest))
    {
        err_set(err, ERR_PROVIDER, L"The application returned an unrecognized version.");
        goto done;
    }
    bool newer = version_compare(&latest, &parsed) > 0;
    bool installable = json_is_true(available, L"installable") && newer;
    if (newer && !installable)
        result_set(out, settings->id, STATE_NEEDS_SETUP, installed, version, L"A newer version exists, but the application's update service says it cannot be installed automatically. Check System > Updates in the application.");
    else
        result_set(out, settings->id, installable ? STATE_UPDATE_AVAILABLE : STATE_UP_TO_DATE, installed, version,
            installable ? L"An update is available on the application's configured release branch."
                : L"The application's configured release branch is current.");
    ok = true;

done:
    json_free(status);
    json_free(updates);
    SecureZeroMemory(key, sizeof(key));
    free(url);
    return ok;
}

static bool install_arr(Providers *providers, const ProgramSettings *settings, const UpdateResult *update,
    ULONGLONG deadline, UpdateResult *out, Err *err)
{
    static const char body[] = "{\"name\":\"ApplicationUpdate\",\"installMajorUpdate\":true}";
    wchar_t key[KEYN], address[URLN], headers[KEYN + 128], relative[64];
    Url *url = xalloc(sizeof(Url));
    HttpRequest request = { 0 };
    HttpResponse response = { 0 };
    JsonDoc *command = NULL;
    int command_id = 0;
    bool ok = false;

    if (!arr_connection(settings, url, key, COUNTOF(key), err) ||
        !endpoint(url, L"api/v3/command", address, COUNTOF(address), err) ||
        !header_line(headers, COUNTOF(headers), L"X-Api-Key", key, err))
        goto done;
    str_cat(headers, COUNTOF(headers), L"Content-Type: application/json; charset=utf-8\r\n");
    request.method = L"POST";
    request.url = address;
    request.headers = headers;
    request.body = body;
    request.body_length = sizeof(body) - 1;
    request.timeout_ms = 30000;
    if (!send_request(providers, &request, &response, err) || !require_success(response.status, err)) goto done;
    command = json_parse_utf8(response.body, response.body_length);
    if (!command || !json_int(json_get(command->root, L"id"), &command_id))
    {
        err_set(err, ERR_OTHER, L"The response was not understood.");
        goto done;
    }

    /* A successful POST only accepts work. The server restart and running version prove completion. */
    *out = *update;
    ULONGLONG poll_deadline = GetTickCount64() + 5 * 60 * 1000;
    while (GetTickCount64() < poll_deadline)
    {
        if (GetTickCount64() + 5000 >= deadline)
        {
            err_set(err, ERR_TIMEOUT, L"The update took too long.");
            goto done;
        }
        Sleep(5000);
        Err ignored = { 0 };
        JsonDoc *status = NULL, *running = NULL;
        int comparison;
        bool failed = false;
        /* While the application restarts, requests fail. Those failures are expected and skipped. */
        if (endpoint(url, L"api/v3/system/status", address, COUNTOF(address), &ignored) &&
            get_json(providers, address, key, &status, &ignored) &&
            compare_versions(json_text(status->root, L"version"), update->available_version, &comparison, &ignored))
        {
            if (comparison >= 0)
            {
                out->state = STATE_UPDATED;
                str_copy(out->installed_version, COUNTOF(out->installed_version), json_text(status->root, L"version"));
                str_copy(out->message, COUNTOF(out->message), L"Update installed and the running application responded with the new version.");
                json_free(status);
                ok = true;
                goto done;
            }
            str_fmt(relative, COUNTOF(relative), L"api/v3/command/%d", command_id);
            if (endpoint(url, relative, address, COUNTOF(address), &ignored) &&
                get_json(providers, address, key, &running, &ignored))
                failed = str_ieq(json_text(running->root, L"status"), L"failed");
        }
        json_free(status);
        json_free(running);
        if (failed)
        {
            out->state = STATE_ERROR;
            str_copy(out->message, COUNTOF(out->message), L"The application's updater failed. Check its System > Logs page for details.");
            ok = true;
            goto done;
        }
        if (ignored.kind == ERR_OTHER)
        {
            *err = ignored;
            goto done;
        }
    }
    out->state = STATE_ERROR;
    str_copy(out->message, COUNTOF(out->message), L"The update was requested, but the new running version was not verified within five minutes. It may still be applying. Check the application before retrying.");
    ok = true;

done:
    json_free(command);
    http_response_free(&response);
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(headers, sizeof(headers));
    free(url);
    return ok;
}

/* ---- Plex, SABnzbd and Pulsarr: installed copies ---- */

typedef struct Installation
{
    wchar_t version[VERN];
    wchar_t executable[PATHN];
    bool has_tasks;                 /* Inno Setup saved the options chosen at install time */
    wchar_t selected_tasks[1024];
} Installation;

typedef struct Pending { wchar_t version[VERN]; wchar_t installer_path[PATHN]; } Pending;

static void pending_path(ApplicationId id, wchar_t *dst, size_t cap)
{
    wchar_t base[PATHN], folder[PATHN], name[64];
    app_data_directory(base, COUNTOF(base));
    path_join(folder, COUNTOF(folder), base, L"Pending");
    str_fmt(name, COUNTOF(name), L"%s.json", app_key(id));
    path_join(dst, cap, folder, name);
}

/* Returns 1 with the record, 0 when there is none, -1 when it cannot be trusted. */
static int read_pending(ApplicationId id, Pending *out, Err *err)
{
    wchar_t path[PATHN];
    char *data;
    size_t length;
    pending_path(id, path, COUNTOF(path));
    FileRead read = file_read_all(path, 1024 * 1024, &data, &length);
    if (read == FR_MISSING) return 0;
    JsonDoc *doc = read == FR_OK ? json_parse_utf8(data, length) : NULL;
    free(data);
    const wchar_t *version = doc ? json_string(doc->root, L"Version") : L"";
    const wchar_t *installer = doc ? json_string(doc->root, L"InstallerPath") : L"";
    bool ok = version[0] && installer[0];
    if (ok)
    {
        str_copy(out->version, COUNTOF(out->version), version);
        str_copy(out->installer_path, COUNTOF(out->installer_path), installer);
    }
    json_free(doc);
    if (!ok)
    {
        err_set(err, ERR_PROVIDER, L"A previous installer record is invalid. Automatic updates are paused until its state is reviewed.");
        return -1;
    }
    return 1;
}

/* Returns 1 with the installation, 0 when the app is not installed, -1 on an unsafe registration. */
static int find_installation(ApplicationId id, Installation *out, Err *err)
{
    static const HKEY hives[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    static const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    memset(out, 0, sizeof(*out));
    for (int h = 0; h < 2; h++)
    for (int v = 0; v < 2; v++)
    {
        HKEY entries;
        if (RegOpenKeyExW(hives[h], UNINSTALL_ROOT, 0, KEY_READ | views[v], &entries) != ERROR_SUCCESS) continue;
        for (DWORD index = 0;; index++)
        {
            wchar_t name[256], display[512], directory[PATHN], uninstall[PATHN * 2];
            DWORD size = COUNTOF(name);
            LONG status = RegEnumKeyExW(entries, index, name, &size, NULL, NULL, NULL, NULL);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) continue;
            bool match;
            if (id == APP_PLEX)
            {
                reg_read_string(entries, name, 0, L"DisplayName", display, COUNTOF(display));
                match = str_istarts(display, L"Plex Media Server") && (display[17] == 0 || display[17] == L' ');
            }
            else match = str_ieq(name, id == APP_SABNZBD ? L"SABnzbd" : PULSARR_KEY);
            if (!match) continue;

            reg_read_string(entries, name, 0, L"InstallLocation", directory, COUNTOF(directory));
            str_trim_chars(directory, L"\"");
            if (id == APP_SABNZBD)
            {
                wchar_t registered[PATHN];
                if (reg_read_string(hives[h], L"SOFTWARE\\SABnzbd", views[v], NULL, registered, COUNTOF(registered)))
                    str_copy(directory, COUNTOF(directory), registered);
            }
            if (!directory[0])
            {
                reg_read_string(entries, name, 0, L"UninstallString", uninstall, COUNTOF(uninstall));
                wchar_t *program = uninstall;
                if (uninstall[0] == L'"')
                {
                    program++;
                    wchar_t *closing = wcschr(program, L'"');
                    if (closing) *closing = 0;
                }
                if (!str_blank(program) && str_iends(program, L".exe")) path_dirname(directory, COUNTOF(directory), program);
            }
            if (!directory[0] && id == APP_PLEX)
            {
                wchar_t program_files[PATHN] = L"", plex[PATHN];
                GetEnvironmentVariableW(views[v] == KEY_WOW64_64KEY ? L"ProgramW6432" : L"ProgramFiles(x86)", program_files, PATHN);
                path_join(plex, COUNTOF(plex), program_files, L"Plex");
                path_join(directory, COUNTOF(directory), plex, L"Plex Media Server");
            }
            if (!path_is_absolute(directory))
            {
                RegCloseKey(entries);
                err_set(err, ERR_PROVIDER, L"The application's registered installation directory is not an absolute path.");
                return -1;
            }
            reg_read_string(entries, name, 0, L"DisplayVersion", out->version, COUNTOF(out->version));
            path_join(out->executable, COUNTOF(out->executable), directory,
                id == APP_PLEX ? L"Plex Media Server.exe" : id == APP_SABNZBD ? L"SABnzbd.exe" : L"pulsarr-service.exe");
            out->has_tasks = reg_read_string(entries, name, 0, L"Inno Setup: Selected Tasks", out->selected_tasks, COUNTOF(out->selected_tasks));
            RegCloseKey(entries);
            return 1;
        }
        RegCloseKey(entries);
    }
    return 0;
}

static bool validate_download_url(ApplicationId id, const wchar_t *text, Err *err)
{
    const wchar_t *prefix = id == APP_PLEX ? L"https://downloads.plex.tv/plex-media-server-new/"
        : id == APP_SABNZBD ? L"https://github.com/sabnzbd/sabnzbd/releases/download/"
        : id == APP_PULSARR ? L"https://github.com/jamcalli/Pulsarr/releases/download/" : NULL;
    if (!prefix) return err_set(err, ERR_PROVIDER, L"Unsupported Windows installer.");
    Url *url = xalloc(sizeof(Url));
    bool parsed = url_parse(text, url);
    /* Path tricks such as "/../" could leave the publisher's folder while still matching the prefix. */
    bool ok = parsed && wcscmp(url->scheme, L"https") == 0 && !url->userinfo[0] && str_istarts(text, prefix) &&
        str_iends(url->path, L".exe") && !wcsstr(url->path, L"/.") && !wcschr(url->path, L'%');
    free(url);
    if (!parsed) return err_set(err, ERR_OTHER, L"The release address was not understood.");
    return ok ? true : err_set(err, ERR_PROVIDER, L"The release contained an unsupported installer URL.");
}

static bool check_installer(Providers *providers, ApplicationId id, UpdateResult *out, Err *err)
{
    Installation *install = xalloc(sizeof(Installation));
    Pending *pending = xalloc(sizeof(Pending));
    JsonDoc *feed = NULL;
    wchar_t version[VERN] = L"", url[URLN] = L"", digest[128] = L"", path[PATHN];
    Version installed, available;
    bool ok = false;

    int found = find_installation(id, install, err);
    if (found < 0) goto done;
    int has_pending = read_pending(id, pending, err);
    if (has_pending < 0) goto done;
    if (has_pending)
    {
        wchar_t program[PATHN];
        DWORD ids[1];
        int comparison = -1;
        str_copy(program, COUNTOF(program), path_filename(pending->installer_path));
        wchar_t *dot = wcsrchr(program, L'.');
        if (dot) *dot = 0;
        bool still_running = proc_find(program, ids, 1) > 0;
        bool comparable = !still_running && found && version_parse(install->version, &installed);
        if (comparable && !compare_versions(install->version, pending->version, &comparison, err)) goto done;
        if (comparable && comparison >= 0)
        {
            pending_path(id, path, COUNTOF(path));
            DeleteFileW(path);
        }
        else
        {
            result_set(out, id, STATE_NEEDS_SETUP, found ? install->version : L"", pending->version,
                L"A previous installer is running or its outcome is unverified. Automatic retries are paused to avoid overlapping installations. Check the application and its installer log.");
            ok = true;
            goto done;
        }
    }

#define NEEDS_SETUP(available_text, text) \
    do { result_set(out, id, STATE_NEEDS_SETUP, install->version, available_text, text); ok = true; goto done; } while (0)

    if (!found)
    {
        result_set(out, id, STATE_NOT_INSTALLED, L"", L"", L"No supported native Windows installation was found. Docker and source installations are not changed.");
        ok = true;
        goto done;
    }
    if (!version_parse(install->version, &installed))
        NEEDS_SETUP(L"", L"The installed version could not be determined safely.");
    if (!file_exists(install->executable))
        NEEDS_SETUP(L"", L"The registered application files were not found. Repair its Windows installation first.");
    int bits = id == APP_PULSARR ? 1 : file_is_64bit_program(install->executable);
    if (bits < 0)
    {
        err_set(err, ERR_PROVIDER, L"The installed application is not a recognized Windows executable.");
        goto done;
    }
    if (id == APP_SABNZBD && !bits)
        NEEDS_SETUP(L"", L"SABnzbd's current Windows installer is 64-bit. Migrate this 32-bit installation manually first.");
    if (id == APP_PULSARR)
    {
        PulsarrSetup setup;
        check_pulsarr_setup(&setup);
        if (!setup.supported) NEEDS_SETUP(L"", setup.message);
    }

    if (id == APP_PLEX)
    {
        if (!get_json(providers, L"https://plex.tv/api/downloads/5.json", NULL, &feed, err)) goto done;
        const JsonValue *releases = json_get(json_get(json_get(feed->root, L"computer"), L"Windows"), L"releases");
        if (!releases || releases->kind != JSON_ARRAY)
        {
            err_set(err, ERR_OTHER, L"The response was not understood.");
            goto done;
        }
        const wchar_t *build = bits ? L"windows-x86_64" : L"windows-x86";
        const JsonValue *release = NULL;
        for (const JsonValue *item = releases->first; item && !release; item = item->next)
            if (wcscmp(json_text(item, L"build"), build) == 0) release = item;
        if (!release) NEEDS_SETUP(L"", L"Plex has no public Windows release for this installed architecture.");
        str_copy(version, COUNTOF(version), json_text(release, L"version"));
        str_copy(url, COUNTOF(url), json_text(release, L"url"));
        str_copy(digest, COUNTOF(digest), json_text(release, L"checksum"));
        if (!is_hex(digest, 40))
        {
            err_set(err, ERR_PROVIDER, L"The official Plex feed did not provide a valid checksum.");
            goto done;
        }
        if (!validate_download_url(id, url, err)) goto done;
        /* Plex changed from the WiX bootstrapper to Inno Setup with its 1.43 series.
           Older x86 releases remain visible for status, but are not run with guessed switches. */
        int comparison;
        Version target;
        if (!compare_versions(version, install->version, &comparison, err)) goto done;
        if (comparison > 0 && (!version_parse(version, &target) || target.part[0] < 1 || (target.part[0] == 1 && target.part[1] < 43)))
            NEEDS_SETUP(version, L"This older Plex installer requires a manual upgrade. Current 64-bit releases are supported.");
    }
    else
    {
        wchar_t expected[160], tag[VERN];
        if (!get_json(providers, id == APP_SABNZBD ? L"https://api.github.com/repos/sabnzbd/sabnzbd/releases/latest"
                : L"https://api.github.com/repos/jamcalli/Pulsarr/releases/latest", NULL, &feed, err))
            goto done;
        const JsonValue *prerelease = json_get(feed->root, L"prerelease"), *draft = json_get(feed->root, L"draft");
        const JsonValue *assets = json_get(feed->root, L"assets");
        if (!prerelease || !draft || (prerelease->kind != JSON_TRUE && prerelease->kind != JSON_FALSE) ||
            (draft->kind != JSON_TRUE && draft->kind != JSON_FALSE) || !assets || assets->kind != JSON_ARRAY)
        {
            err_set(err, ERR_OTHER, L"The response was not understood.");
            goto done;
        }
        if (prerelease->kind == JSON_TRUE || draft->kind == JSON_TRUE)
        {
            err_set(err, ERR_PROVIDER, L"The release feed did not return a stable published release.");
            goto done;
        }
        str_copy(tag, COUNTOF(tag), json_text(feed->root, L"tag_name"));
        const wchar_t *bare = tag;
        while (*bare == L'v') bare++;
        str_copy(version, COUNTOF(version), bare);
        if (id == APP_SABNZBD) str_fmt(expected, COUNTOF(expected), L"SABnzbd-%s-win-setup.exe", version);
        else str_fmt(expected, COUNTOF(expected), L"pulsarr-v%s-windows-x64-setup.exe", version);
        const JsonValue *asset = NULL;
        for (const JsonValue *item = assets->first; item && !asset; item = item->next)
            if (wcscmp(json_text(item, L"name"), expected) == 0) asset = item;
        if (!asset)
        {
            err_set(err, ERR_PROVIDER, L"The latest stable release has no supported Windows installer.");
            goto done;
        }
        str_copy(url, COUNTOF(url), json_text(asset, L"browser_download_url"));
        const wchar_t *published = json_text(asset, L"digest");
        if (wcsncmp(published, L"sha256:", 7) != 0 || !is_hex(published + 7, 64))
        {
            err_set(err, ERR_PROVIDER, L"The official release did not provide a SHA-256 digest. Installation was disabled.");
            goto done;
        }
        str_copy(digest, COUNTOF(digest), published + 7);
        if (!validate_download_url(id, url, err)) goto done;
    }
    if (!version_parse(version, &available))
    {
        err_set(err, ERR_PROVIDER, L"The published version could not be compared safely.");
        goto done;
    }
    bool newer = version_compare(&available, &installed) > 0;
    result_set(out, id, newer ? STATE_UPDATE_AVAILABLE : STATE_UP_TO_DATE, install->version, version,
        newer ? L"A stable update is available from the official publisher."
            : L"The installed version is current or newer than the public release.");
    str_copy(out->download_url, COUNTOF(out->download_url), url);
    str_copy(out->checksum, COUNTOF(out->checksum), digest);
    ok = true;
#undef NEEDS_SETUP

done:
    json_free(feed);
    free(install);
    free(pending);
    return ok;
}

/* ---- Plex, SABnzbd and Pulsarr: installing ---- */

static bool download(Providers *providers, ApplicationId id, const wchar_t *address, const wchar_t *file, Err *err)
{
    if (!validate_download_url(id, address, err)) return false;
    Url *current = xalloc(sizeof(Url)), *next = xalloc(sizeof(Url));
    wchar_t *text = xalloc(sizeof(wchar_t) * URLN);
    ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    bool ok = false, finished = false;
    str_copy(text, URLN, address);
    for (int redirects = 0; redirects < 6 && !finished; redirects++)
    {
        HttpRequest request = { 0 };
        HttpResponse response;
        request.url = text;
        request.timeout_ms = 30000;
        request.save_path = file;
        request.save_create_new = true;
        request.max_bytes = DOWNLOAD_LIMIT;
        request.deadline = deadline;
        HttpStatus status = providers->send(providers->send_context, &request, &response);
        finished = true;
        if (status == HTTP_E_TOO_LARGE) err_set(err, ERR_PROVIDER, L"The installer exceeded the download size limit.");
        else if (!http_check(status, err)) { }
        else if (response.status >= 300 && response.status < 400)
        {
            if (!response.location[0]) err_set(err, ERR_PROVIDER, L"The download redirect had no destination.");
            else if (!url_parse(text, current) || !url_resolve(current, response.location, next) ||
                wcscmp(next->scheme, L"https") != 0 || next->userinfo[0] ||
                !(wcscmp(next->host, L"downloads.plex.tv") == 0 || str_iends(next->host, L".githubusercontent.com")))
                err_set(err, ERR_PROVIDER, L"The publisher redirected the download to an unsupported destination.");
            else
            {
                url_format(next, text, URLN);
                finished = false;
            }
        }
        else ok = require_success(response.status, err);
        if (status == HTTP_OK) http_response_free(&response);
    }
    if (!finished) err_set(err, ERR_PROVIDER, L"The download exceeded the redirect limit.");
    free(text);
    free(current);
    free(next);
    return ok;
}

static bool verify_download(ApplicationId id, const wchar_t *file, const wchar_t *expected, Err *err)
{
    wchar_t hash[80], signer[256];
    if (!file_hash(file, id == APP_PLEX ? HASH_SHA1 : HASH_SHA256, hash, COUNTOF(hash)))
        return err_set(err, ERR_OTHER, L"The download could not be read.");
    if (!str_ieq(hash, expected))
        return err_set(err, ERR_PROVIDER, L"The downloaded installer did not match the official checksum. It was not run.");
    /* Plex's feed provides SHA-1 only. Also require a trusted Authenticode chain and Plex publisher. */
    if (id == APP_PLEX)
    {
        if (!file_signature_trusted(file, signer, COUNTOF(signer)))
            return err_set(err, ERR_PROVIDER, L"The Plex installer did not pass Windows signature verification. It was not run.");
        if (wcscmp(signer, L"Plex, Inc.") != 0 && wcscmp(signer, L"Plex Inc.") != 0 && wcscmp(signer, L"Plex Inc") != 0)
            return err_set(err, ERR_PROVIDER, L"The installer signature was not issued to the expected Plex publisher. It was not run.");
    }
    return true;
}

static bool is_expected_process_running(const wchar_t *name, const wchar_t *executable)
{
    DWORD ids[64];
    wchar_t path[PATHN];
    int count = proc_find(name, ids, COUNTOF(ids));
    for (int i = 0; i < count && i < (int)COUNTOF(ids); i++)
        if (proc_image_path(ids[i], path, COUNTOF(path)) && str_ieq(path, executable)) return true;
    return false;
}

/* Finds the one running copy of an app and how it was started, so it can be started the same way afterwards.
   `arguments` (free with LocalFree) is NULL when the app is not running. */
static bool get_running_application(const wchar_t *name, const wchar_t *executable, wchar_t ***arguments, int *count, Err *err)
{
    DWORD ids[2];
    wchar_t path[PATHN], system[PATHN], powershell[PATHN], script[256];
    bool known;
    *arguments = NULL;
    *count = 0;
    int matches = proc_find(name, ids, COUNTOF(ids));
    if (matches == 0) return true;
    if (matches > 1)
        return err_set(err, ERR_PROVIDER, L"Multiple application instances are running. Automatic installation is disabled to preserve their separate settings.");
    if (!proc_image_path(ids[0], path, COUNTOF(path))) return err_set(err, ERR_OTHER, L"The running application could not be inspected.");
    if (!str_ieq(path, executable))
        return err_set(err, ERR_PROVIDER, L"The running application does not match its Windows installation. Close the separate copy before updating.");
    bool same = proc_same_user(ids[0], &known);
    if (!known)
        return err_set(err, ERR_PROVIDER, L"The running application's Windows account could not be verified. Close it before updating.");
    if (!same)
        return err_set(err, ERR_PROVIDER, L"The application runs under another Windows account. Run this updater in that account or use the application's Windows service.");

    /* Preserve command-line configuration such as SABnzbd's custom data-file argument.
       The only variable in this script is a process ID obtained from Windows. */
    str_fmt(script, COUNTOF(script), L"$p=Get-CimInstance Win32_Process -Filter 'ProcessId=%lu'; if($p){ $p.CommandLine | ConvertTo-Json -Compress }", ids[0]);
    folder_system(system, COUNTOF(system));
    path_join(powershell, COUNTOF(powershell), system, L"WindowsPowerShell\\v1.0\\powershell.exe");
    const wchar_t *shell_arguments[] = { L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-Command", script };
    ProcResult result;
    ProcStatus status = proc_run(powershell, shell_arguments, COUNTOF(shell_arguments), 20000, true, true, &result);
    if (status == PROC_TIMED_OUT) return err_set(err, ERR_PROVIDER, L"A required Windows operation timed out.");
    if (status != PROC_OK) return err_set(err, ERR_OTHER, L"Windows could not start the required process.");

    const wchar_t *preserve_failed = L"The running application's launch options could not be preserved. Close it before updating.";
    bool ok = false;
    wchar_t *output = NULL;
    JsonDoc *doc = NULL;
    if (result.exit_code == 0)
    {
        /* PowerShell writes redirected output in the console's code page. */
        int needed = MultiByteToWideChar(CP_OEMCP, 0, result.output, (int)result.output_length, NULL, 0);
        output = xalloc(((size_t)needed + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_OEMCP, 0, result.output, (int)result.output_length, output, needed);
        str_trim(output);
        doc = json_parse(output);
        if (!doc && output[0]) err_set(err, ERR_OTHER, L"The launch options were not understood.");
    }
    const wchar_t *line = doc && doc->root->kind == JSON_STRING ? doc->root->text : L"";
    if (doc || !output || !output[0])
    {
        if (str_blank(line)) err_set(err, ERR_PROVIDER, L"%s", preserve_failed);
        else
        {
            int total = 0;
            wchar_t **split = proc_split_command_line(line, &total);
            if (!split) err_set(err, ERR_PROVIDER, L"The application's launch options could not be read.");
            else
            {
                ok = true;
                for (int i = 1; i < total && ok; i++)
                {
                    bool option = wcscmp(split[i], L"-f") == 0 || wcscmp(split[i], L"--config-file") == 0;
                    if ((option && (i + 1 >= total || !path_is_absolute(split[i + 1]))) ||
                        (wcsncmp(split[i], L"--config-file=", 14) == 0 && !path_is_absolute(split[i] + 14)))
                        ok = err_set(err, ERR_PROVIDER, L"The application uses a relative configuration path. Close it before updating so its settings are not changed by a restart.");
                }
                if (ok)
                {
                    *arguments = split;
                    *count = total;
                }
                else LocalFree(split);
            }
        }
    }
    json_free(doc);
    free(output);
    free(result.output);
    return ok;
}

static bool install_windows(Providers *providers, ApplicationId id, const UpdateResult *update, UpdateResult *out, Err *err)
{
    Installation *install = xalloc(sizeof(Installation)), *after = xalloc(sizeof(Installation));
    wchar_t **running = NULL;
    int running_count = 0;
    wchar_t file[PATHN] = L"", pending[PATHN], folder[PATHN], base[PATHN], name[80], guid[40];
    wchar_t log_argument[PATHN + 16], tasks_argument[1100];
    const wchar_t *arguments[8];
    int argument_count = 0;
    bool ok = false, stopped_service = false, installer_running = false;
    const wchar_t *service_name = L"";
    *out = *update;

#define FINISH(new_state, installed, text) \
    do { out->state = new_state; if (installed) str_copy(out->installed_version, COUNTOF(out->installed_version), installed); \
        str_copy(out->message, COUNTOF(out->message), text); ok = true; goto done; } while (0)

    int found = find_installation(id, install, err);
    if (found < 0) goto done;
    if (!found)
    {
        err_set(err, ERR_PROVIDER, L"The Windows installation was removed before updating.");
        goto done;
    }
    service_name = id == APP_SABNZBD ? L"SABnzbd" : id == APP_PULSARR ? L"pulsarr" : L"";
    bool service_was_running = service_name[0] && service_running(service_name);
    const wchar_t *process_name = id == APP_PLEX ? L"Plex Media Server" : L"SABnzbd";
    if (id != APP_PULSARR && !service_was_running &&
        !get_running_application(process_name, install->executable, &running, &running_count, err))
        goto done;
    if (service_was_running && id == APP_SABNZBD)
    {
        DWORD ids[2];
        if (proc_find(process_name, ids, COUNTOF(ids)) > 1)
        {
            err_set(err, ERR_PROVIDER, L"Multiple SABnzbd instances are running. Automatic installation is disabled to preserve their separate settings.");
            goto done;
        }
    }

    app_data_directory(base, COUNTOF(base));
    path_join(folder, COUNTOF(folder), base, L"Downloads");
    if (!dir_create(folder))
    {
        err_set(err, ERR_ACCESS, L"The download folder could not be created.");
        goto done;
    }
    new_guid_text(guid, COUNTOF(guid));
    str_fmt(name, COUNTOF(name), L"%s-%s.exe", app_key(id), guid);
    path_join(file, COUNTOF(file), folder, name);

    if (!download(providers, id, update->download_url, file, err) || !verify_download(id, file, update->checksum, err)) goto done;

    if (id == APP_SABNZBD)
    {
        /* NSIS treats the last /D argument specially. Quote handling belongs to its parser,
           so let the official installer recover its registered existing directory instead. */
        arguments[argument_count++] = L"/S";
    }
    else
    {
        wchar_t log[PATHN];
        path_change_extension(log, COUNTOF(log), file, L".log");
        str_fmt(log_argument, COUNTOF(log_argument), L"/LOG=%s", log);
        arguments[argument_count++] = L"/VERYSILENT";
        arguments[argument_count++] = L"/SUPPRESSMSGBOXES";
        arguments[argument_count++] = L"/SP-";
        arguments[argument_count++] = L"/NORESTART";
        arguments[argument_count++] = L"/RESTARTEXITCODE=3010";
        arguments[argument_count++] = log_argument;
        if (id == APP_PULSARR)
        {
            /* Existing service and firewall options must survive an unattended upgrade. */
            arguments[argument_count++] = L"/COMPONENTS=main,service";
            if (!install->has_tasks)
            {
                err_set(err, ERR_PROVIDER, L"Pulsarr's saved installation options could not be read. Run its current installer once interactively before enabling automatic updates.");
                goto done;
            }
            wchar_t list[1024], *context = NULL;
            bool start_listed = false;
            str_copy(list, COUNTOF(list), install->selected_tasks);
            str_copy(tasks_argument, COUNTOF(tasks_argument), L"/TASKS=");
            for (wchar_t *task = wcstok_s(list, L",", &context); task; task = wcstok_s(NULL, L",", &context))
            {
                str_trim(task);
                bool is_start = str_ieq(task, L"startafterinstall");
                /* Start Pulsarr afterwards only if it was running before. */
                if (!*task || (is_start && (start_listed || !service_was_running))) continue;
                if (is_start) start_listed = true;
                if (tasks_argument[7]) str_cat(tasks_argument, COUNTOF(tasks_argument), L",");
                str_cat(tasks_argument, COUNTOF(tasks_argument), task);
            }
            if (service_was_running && !start_listed)
            {
                if (tasks_argument[7]) str_cat(tasks_argument, COUNTOF(tasks_argument), L",");
                str_cat(tasks_argument, COUNTOF(tasks_argument), L"startafterinstall");
            }
            arguments[argument_count++] = tasks_argument;
        }
    }

    if (id == APP_PULSARR && service_was_running)
    {
        /* Pulsarr's installer gives up with error 5 when its service is still shutting down,
           so stop the service and wait until Windows reports it stopped before running Setup. */
        stopped_service = true;
        DWORD stop_error = service_stop(service_name);
        if (stop_error)
        {
            err_set(err, ERR_PROVIDER, L"The application's Windows service could not be stopped for the update (error %lu). Nothing was installed.", stop_error);
            goto done;
        }
        for (int attempt = 0; attempt < 60 && service_state(service_name) != SERVICE_STOPPED; attempt++) Sleep(1000);
        if (service_state(service_name) != SERVICE_STOPPED)
        {
            err_set(err, ERR_PROVIDER, L"The application's Windows service did not finish stopping within a minute. Nothing was installed.");
            goto done;
        }
    }

    /* Once an installer starts it must finish even if the window closes.
       A timeout reports uncertainty and deliberately does not kill a live installer. */
    {
        StrBuf record;
        size_t length;
        pending_path(id, pending, COUNTOF(pending));
        path_dirname(folder, COUNTOF(folder), pending);
        sb_init(&record);
        sb_append(&record, L"{\"Version\":");
        json_write_string(&record, update->available_version);
        sb_append(&record, L",\"InstallerPath\":");
        json_write_string(&record, file);
        sb_append(&record, L"}");
        char *utf8 = wide_to_utf8(record.p, &length);
        bool written = dir_create(folder) && file_write_all(pending, utf8, length);
        free(utf8);
        sb_free(&record);
        if (!written)
        {
            err_set(err, ERR_ACCESS, L"The installer record could not be written.");
            goto done;
        }
    }
    ProcResult installer;
    installer_running = true;
    ProcStatus status = proc_run(file, arguments, argument_count, 15 * 60 * 1000, false, false, &installer);
    if (status == PROC_TIMED_OUT)
    {
        /* The record stays in place across restarts until the new version is verified. */
        err_set(err, ERR_PROVIDER, L"The installer is still running after fifteen minutes. It was left running; check its status before retrying.");
        goto done;
    }
    installer_running = false;
    /* A completed installer can be retried after a reported error, and one that never started changed nothing. */
    DeleteFileW(pending);
    if (status != PROC_OK)
    {
        err_set(err, ERR_OTHER, L"Windows could not start the installer.");
        goto done;
    }
    if (installer.exit_code != 0 && installer.exit_code != 3010)
    {
        err_set(err, ERR_PROVIDER, L"The official installer returned error %lu. Check the application's installation log.", installer.exit_code);
        goto done;
    }

    int comparison = -1;
    found = find_installation(id, after, err);
    if (found < 0) goto done;
    if (found && !compare_versions(after->version, update->available_version, &comparison, err)) goto done;
    if (!found || comparison < 0)
        FINISH(STATE_ERROR, NULL, L"The installer finished, but Windows did not report the expected new version. Check the application before retrying.");
    if (installer.exit_code == 3010)
        FINISH(STATE_RESTART_REQUIRED, after->version, L"The new version is installed. Windows reports that a restart is required; no restart was performed.");

    if (service_was_running && !service_running(service_name))
    {
        if (service_start(service_name) != 0)
            FINISH(STATE_ERROR, after->version, L"The update installed, but its previously running Windows service could not be restarted.");
    }
    else if (running && !is_expected_process_running(process_name, after->executable))
    {
        wchar_t directory[PATHN];
        path_dirname(directory, COUNTOF(directory), after->executable);
        proc_start(after->executable, (const wchar_t *const *)(running + 1), running_count - 1, directory);
    }
    if (service_was_running)
    {
        for (int attempt = 0; attempt < 20 && !service_running(service_name); attempt++) Sleep(1500);
        if (!service_running(service_name))
            FINISH(STATE_ERROR, after->version, L"The new version installed, but the Windows service has not recovered.");
    }
    else if (running)
    {
        Sleep(3000);
        if (!is_expected_process_running(process_name, after->executable))
            FINISH(STATE_ERROR, after->version, L"The new version installed, but the previously running application did not stay running.");
    }
    FINISH(STATE_UPDATED, after->version, L"The new installed version was verified. Previously running applications or services were restored.");
#undef FINISH

done:
    /* A failed update must not leave a service off that this updater stopped.
       Skip this while an installer may still be running (timeout), so the two never race. */
    if (!ok && stopped_service && !installer_running && !service_running(service_name) &&
        service_start(service_name) != 0 && err->kind != ERR_TIMEOUT)
    {
        wchar_t reason[MSGN];
        str_copy(reason, COUNTOF(reason), failure_message(err));
        err_set(err, ERR_PROVIDER, L"%s Its Windows service could not be restarted. Start the %s service from Windows Services.", reason, service_name);
    }
    /* Keep the small installation log, but remove this invocation's downloaded installer. */
    if (file[0]) DeleteFileW(file);
    if (running) LocalFree(running);
    free(install);
    free(after);
    return ok;
}

/* ---- Checks of whether an app is busy, so an update can wait instead of interrupting it ---- */

static bool in_list(const wchar_t *text, const wchar_t *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) if (str_ieq(text, list[i])) return true;
    return false;
}

static void usage_set(UsageResult *out, UsageState state, const wchar_t *format, ...)
{
    va_list args;
    out->state = state;
    va_start(args, format);
    str_vfmt(out->message, COUNTOF(out->message), format, args);
    va_end(args);
}

bool count_plex_sessions(const JsonValue *root, int *count)
{
    const JsonValue *container = json_get(root, L"MediaContainer");
    if (!container) return false;
    const JsonValue *items = json_get(container, L"Metadata");
    if (!json_int(json_get(container, L"size"), count)) *count = items && items->kind == JSON_ARRAY ? items->count : 0;
    return true;
}

static bool check_plex_usage(Providers *providers, UsageResult *out, Err *err)
{
    wchar_t address[256], token[512], headers[640] = L"Accept: application/json\r\n", line[560];
    HttpRequest request = { 0 };
    HttpResponse response;
    str_fmt(address, COUNTOF(address), L"%sstatus/sessions", providers->plex_address);
    /* Plex keeps its server token in the registry of the Windows account it runs under. */
    if (reg_read_string(HKEY_CURRENT_USER, L"Software\\Plex, Inc.\\Plex Media Server", 0, L"PlexOnlineToken", token, COUNTOF(token)) && token[0])
    {
        if (!header_line(line, COUNTOF(line), L"X-Plex-Token", token, err)) return false;
        str_cat(headers, COUNTOF(headers), line);
    }
    request.url = address;
    request.headers = headers;
    request.timeout_ms = 30000;
    if (!send_request(providers, &request, &response, err)) return false;
    bool ok;
    int streams = 0;
    if (response.status == 401 || response.status == 403)
        ok = err_set(err, ERR_PROVIDER, L"Plex did not allow the check. Make sure Plex Media Server is signed in on this PC under the same Windows account.");
    else ok = require_success(response.status, err);
    if (ok)
    {
        JsonDoc *body = json_parse_utf8(response.body, response.body_length);
        if (!body) ok = err_set(err, ERR_OTHER, L"The response was not understood.");
        else if (!count_plex_sessions(body->root, &streams)) ok = err_set(err, ERR_PROVIDER, L"Plex returned an unexpected response.");
        json_free(body);
    }
    http_response_free(&response);
    if (!ok) return false;
    if (streams == 1) usage_set(out, USAGE_IN_USE, L"Plex is streaming 1 item.");
    else if (streams > 0) usage_set(out, USAGE_IN_USE, L"Plex is streaming %d items.", streams);
    else usage_set(out, USAGE_IDLE, L"Plex has no active streams.");
    return true;
}

void read_sabnzbd_config(const wchar_t *ini, SabnzbdConfig *out)
{
    /* sabnzbd.ini is a ConfigObj file. Read the top-level [misc] section only. */
    static const wchar_t *const names[] = { L"host", L"enable_https", L"https_port", L"port", L"url_base", L"api_key" };
    wchar_t values[6][KEYN];
    wchar_t *text = xwcsdup(ini), *context = NULL;
    bool in_misc = false;
    memset(values, 0, sizeof(values));
    for (wchar_t *line = wcstok_s(text, L"\n", &context); line; line = wcstok_s(NULL, L"\n", &context))
    {
        str_trim(line);
        if (*line == L'[')
        {
            in_misc = str_ieq(line, L"[misc]");
            continue;
        }
        if (!in_misc || !*line || *line == L'#') continue;
        wchar_t *equals = wcschr(line, L'=');
        if (!equals || equals == line) continue;
        *equals = 0;
        str_trim(line);
        str_trim(equals + 1);
        str_trim_chars(equals + 1, L"\"");
        for (size_t i = 0; i < COUNTOF(names); i++)
            if (str_ieq(line, names[i])) str_copy(values[i], KEYN, equals + 1);
    }
    free(text);

    memset(out, 0, sizeof(*out));
    str_copy(out->host, COUNTOF(out->host), values[0][0] ? values[0] : L"127.0.0.1");
    if (wcscmp(out->host, L"0.0.0.0") == 0 || wcscmp(out->host, L"::") == 0 || wcscmp(out->host, L"[::]") == 0 ||
        wcscmp(out->host, L"localhost") == 0)
        str_copy(out->host, COUNTOF(out->host), L"127.0.0.1");
    /* With HTTPS on and no separate HTTPS port, the main port serves HTTPS only. */
    out->https = wcscmp(values[1], L"1") == 0 && !values[2][0];
    out->port = 0;
    const wchar_t *p = values[3];
    for (; is_digit(*p) && out->port <= 65535; p++) out->port = out->port * 10 + (*p - L'0');
    if (*p || !values[3][0] || out->port < 1 || out->port > 65535) out->port = 8080;
    wchar_t base[256];
    str_copy(base, COUNTOF(base), values[4][0] ? values[4] : L"/sabnzbd");
    str_trim_chars(base, L"/");
    str_fmt(out->url_base, COUNTOF(out->url_base), L"/%s", base);
    str_copy(out->api_key, COUNTOF(out->api_key), values[5]);
}

static void sabnzbd_api(const SabnzbdConfig *config, const wchar_t *mode, wchar_t *dst, size_t cap)
{
    wchar_t base[256], key[KEYN * 3];
    str_copy(base, COUNTOF(base), config->url_base);
    size_t length = wcslen(base);
    while (length && base[length - 1] == L'/') base[--length] = 0;
    url_escape_data(config->api_key, key, COUNTOF(key));
    bool bare_ipv6 = wcschr(config->host, L':') && config->host[0] != L'[';
    str_fmt(dst, cap, L"%s://%s%s%s:%d%s/api?mode=%s&output=json&limit=20&apikey=%s", config->https ? L"https" : L"http",
        bare_ipv6 ? L"[" : L"", config->host, bare_ipv6 ? L"]" : L"", config->port, base, mode, key);
}

static void find_sabnzbd_config(wchar_t *dst, size_t cap)
{
    /* A SABnzbd Windows service is started with "-f <path to sabnzbd.ini>". */
    wchar_t parameters[PATHN * 2], command[PATHN * 2 + 16], base[PATHN], folder[PATHN];
    if (reg_read_string(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\SABnzbd\\Parameters", 0, L"AppParameters",
            parameters, COUNTOF(parameters)) && !str_blank(parameters))
    {
        int count = 0;
        str_fmt(command, COUNTOF(command), L"sabnzbd %s", parameters);
        wchar_t **arguments = proc_split_command_line(command, &count);
        for (int i = 1; arguments && i < count - 1; i++)
        {
            if ((wcscmp(arguments[i], L"-f") == 0 || wcscmp(arguments[i], L"--config-file") == 0) && path_is_absolute(arguments[i + 1]))
            {
                str_copy(dst, cap, arguments[i + 1]);
                LocalFree(arguments);
                return;
            }
        }
        if (arguments) LocalFree(arguments);
    }
    folder_local_app_data(base, COUNTOF(base));
    path_join(folder, COUNTOF(folder), base, L"sabnzbd");
    path_join(dst, cap, folder, L"sabnzbd.ini");
}

static bool check_sabnzbd_usage(Providers *providers, UsageResult *out, Err *err)
{
    static const wchar_t *const busy_history[] =
        { L"Queued", L"QuickCheck", L"Verifying", L"Repairing", L"Fetching", L"Extracting", L"Moving", L"Running" };
    wchar_t path[PATHN], address[URLN];
    char *data;
    size_t length;
    if (providers->sabnzbd_config_override) str_copy(path, COUNTOF(path), providers->sabnzbd_config_override);
    else find_sabnzbd_config(path, COUNTOF(path));
    FileRead read = file_read_all(path, 16 * 1024 * 1024, &data, &length);
    if (read == FR_MISSING) return err_set(err, ERR_PROVIDER, L"SABnzbd's settings file (sabnzbd.ini) was not found.");
    if (read != FR_OK) return err_set(err, ERR_OTHER, L"SABnzbd's settings file could not be read.");

    SabnzbdConfig *config = xalloc(sizeof(SabnzbdConfig));
    wchar_t *ini = utf8_to_wide(data, (int)length);
    JsonDoc *queue = NULL, *history = NULL;
    bool ok = false;
    free(data);
    read_sabnzbd_config(ini, config);
    SecureZeroMemory(ini, wcslen(ini) * sizeof(wchar_t));
    free(ini);
    if (!config->api_key[0])
    {
        err_set(err, ERR_PROVIDER, L"SABnzbd's settings file has no API key.");
        goto done;
    }

    sabnzbd_api(config, L"queue", address, COUNTOF(address));
    if (!get_json(providers, address, NULL, &queue, err)) goto done;
    const JsonValue *q = json_get(queue->root, L"queue");
    if (!q || q->kind != JSON_OBJECT)
    {
        err_set(err, ERR_OTHER, L"The response was not understood.");
        goto done;
    }
    int slots = 0;
    if (!json_int(json_get(q, L"noofslots_total"), &slots) && !json_int(json_get(q, L"noofslots"), &slots)) slots = 0;
    if (!json_is_true(q, L"paused") && slots > 0)
    {
        if (slots == 1) usage_set(out, USAGE_IN_USE, L"SABnzbd is downloading 1 item.");
        else usage_set(out, USAGE_IN_USE, L"SABnzbd is downloading %d items.", slots);
        ok = true;
        goto done;
    }

    sabnzbd_api(config, L"history", address, COUNTOF(address));
    if (!get_json(providers, address, NULL, &history, err)) goto done;
    const JsonValue *h = json_get(history->root, L"history");
    if (!h || h->kind != JSON_OBJECT)
    {
        err_set(err, ERR_OTHER, L"The response was not understood.");
        goto done;
    }
    const JsonValue *items = json_get(h, L"slots");
    int processing = 0;
    for (const JsonValue *item = items && items->kind == JSON_ARRAY ? items->first : NULL; item; item = item->next)
        if (in_list(json_text(item, L"status"), busy_history, COUNTOF(busy_history))) processing++;
    if (processing > 0) usage_set(out, USAGE_IN_USE, L"SABnzbd is checking or unpacking a finished download.");
    else usage_set(out, USAGE_IDLE, L"SABnzbd has no active downloads.");
    ok = true;

done:
    json_free(queue);
    json_free(history);
    SecureZeroMemory(config, sizeof(*config));
    SecureZeroMemory(address, sizeof(address));
    free(config);
    return ok;
}

static bool check_arr_usage(Providers *providers, const ProgramSettings *settings, UsageResult *out, Err *err)
{
    /* Frequent, short housekeeping that Radarr and Sonarr run on their own. These do not count as "in use". */
    static const wchar_t *const routine[] =
        { L"RefreshMonitoredDownloads", L"CheckHealth", L"MessagingCleanup", L"Housekeeping", L"RssSync",
          L"ApplicationUpdate", L"ApplicationUpdateCheck", L"ImportListSync", L"CleanUpRecycleBin", L"Backup" };
    const wchar_t *name = app_name(settings->id);
    wchar_t key[KEYN], address[URLN];
    Url *url = xalloc(sizeof(Url));
    JsonDoc *commands = NULL;
    bool ok = false;
    if (!arr_connection(settings, url, key, COUNTOF(key), err)) goto done;
    if (str_blank(key))
    {
        err_set(err, ERR_PROVIDER, L"%s's API key is missing.", name);
        goto done;
    }
    if (!endpoint(url, L"api/v3/command", address, COUNTOF(address), err) ||
        !get_json(providers, address, key, &commands, err))
        goto done;
    if (commands->root->kind != JSON_ARRAY)
    {
        err_set(err, ERR_OTHER, L"The response was not understood.");
        goto done;
    }
    const wchar_t *labels[16];
    int count = 0;
    for (const JsonValue *command = commands->root->first; command; command = command->next)
    {
        if (!str_ieq(json_text(command, L"status"), L"started") || in_list(json_text(command, L"name"), routine, COUNTOF(routine)))
            continue;
        const wchar_t *label = json_text(command, L"commandName");
        if (!label[0]) label = json_text(command, L"name");
        bool listed = false;
        for (int i = 0; i < count; i++) if (wcscmp(labels[i], label) == 0) listed = true;
        if (!listed && count < (int)COUNTOF(labels)) labels[count++] = label;
    }
    if (count > 0)
    {
        wchar_t list[600] = L"";
        for (int i = 0; i < count; i++)
        {
            if (i) str_cat(list, COUNTOF(list), L", ");
            str_cat(list, COUNTOF(list), labels[i]);
        }
        usage_set(out, USAGE_IN_USE, L"%s is running: %s.", name, list);
    }
    else usage_set(out, USAGE_IDLE, L"%s is not running any tasks.", name);
    ok = true;

done:
    json_free(commands);
    SecureZeroMemory(key, sizeof(key));
    free(url);
    return ok;
}

void read_pulsarr_address(const wchar_t *data_directory, int *port, wchar_t *base_path, size_t cap)
{
    /* listenPort is what the server binds to; port is the public port and the usual fallback. */
    static const wchar_t *const keys[] = { L"listenPort", L"port" };
    wchar_t value[256];
    *port = 3003;
    for (size_t i = 0; i < COUNTOF(keys); i++)
    {
        if (!pulsarr_env_value(data_directory, keys[i], value, COUNTOF(value), NULL) || !value[0]) continue;
        int parsed = 0;
        const wchar_t *p = value;
        for (; is_digit(*p) && parsed < 65536; p++) parsed = parsed * 10 + (*p - L'0');
        if (!*p && parsed > 0 && parsed < 65536)
        {
            *port = parsed;
            break;
        }
    }
    base_path[0] = 0;
    if (pulsarr_env_value(data_directory, L"basePath", value, COUNTOF(value), NULL) && value[0])
    {
        str_trim_chars(value, L"/");
        str_fmt(base_path, cap, L"/%s", value);
    }
}

static bool check_pulsarr_usage(Providers *providers, const ProgramSettings *settings, UsageResult *out, Err *err)
{
    wchar_t directory[PATHN], base[256], address[URLN], key[KEYN], headers[KEYN + 32];
    int port;
    HttpRequest request = { 0 };
    HttpResponse response;
    if (providers->pulsarr_data_override) str_copy(directory, COUNTOF(directory), providers->pulsarr_data_override);
    else pulsarr_data_directory(directory, COUNTOF(directory));
    read_pulsarr_address(directory, &port, base, COUNTOF(base));
    size_t length = wcslen(base);
    while (length && base[length - 1] == L'/') base[--length] = 0;
    str_fmt(address, COUNTOF(address), L"http://127.0.0.1:%d%s/v1/watchlist-workflow/status", port, base);
    str_copy(key, COUNTOF(key), settings->api_key);
    str_trim(key);
    request.url = address;
    request.timeout_ms = 30000;
    if (key[0])
    {
        if (!header_line(headers, COUNTOF(headers), L"X-API-Key", key, err)) return false;
        request.headers = headers;
    }
    bool sent = send_request(providers, &request, &response, err);
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(headers, sizeof(headers));
    if (!sent) return false;
    bool ok;
    if (response.status == 401 || response.status == 403)
        ok = err_set(err, ERR_PROVIDER, L"Pulsarr turned the check away because it needs an API key. In Settings under Wait while apps are in use, choose Find key, or paste a key from Pulsarr's Utilities \x2192 API Keys.");
    else ok = require_success(response.status, err);
    if (ok)
    {
        JsonDoc *body = json_parse_utf8(response.body, response.body_length);
        if (!body) ok = err_set(err, ERR_OTHER, L"The response was not understood.");
        else
        {
            const wchar_t *status = json_text(body->root, L"status");
            if (wcscmp(status, L"starting") == 0 || wcscmp(status, L"stopping") == 0)
                usage_set(out, USAGE_IN_USE, L"Pulsarr is syncing watchlists.");
            else usage_set(out, USAGE_IDLE, L"Pulsarr is not syncing.");
        }
        json_free(body);
    }
    http_response_free(&response);
    return ok;
}

/* ---- The provider interface ---- */

static CallStatus provider_check(void *context, const ProgramSettings *settings, ULONGLONG deadline, UpdateResult *result)
{
    Providers *providers = context;
    Err err = { 0 };
    bool ok = settings->id == APP_RADARR || settings->id == APP_SONARR ? check_arr(providers, settings, result, &err)
        : check_installer(providers, settings->id, result, &err);
    if (ok) return CALL_OK;
    if (err.kind == ERR_TIMEOUT && GetTickCount64() >= deadline) return CALL_TIMED_OUT;
    failure(settings->id, &err, result);
    return CALL_OK;
}

static CallStatus provider_install(void *context, const ProgramSettings *settings, const UpdateResult *update,
    ULONGLONG deadline, UpdateResult *result)
{
    Providers *providers = context;
    Err err = { 0 };
    UpdateResult *current = xalloc(sizeof(UpdateResult));
    CallStatus status = CALL_OK;
    bool ok = true;
    /* Recheck immediately before changing an application. Never trust a stale result or URL. */
    status = provider_check(context, settings, deadline, current);
    if (status == CALL_OK)
    {
        if (current->state != STATE_UPDATE_AVAILABLE) *result = *current;
        else if (settings->id == APP_RADARR || settings->id == APP_SONARR)
            ok = install_arr(providers, settings, current, deadline, result, &err);
        else if (!is_administrator())
        {
            *result = *current;
            result->state = STATE_NEEDS_SETUP;
            str_copy(result->message, COUNTOF(result->message), L"Restart Media Center Updater as administrator to install Windows updates automatically.");
        }
        else ok = install_windows(providers, settings->id, current, result, &err);
        if (!ok)
        {
            if (err.kind == ERR_TIMEOUT && GetTickCount64() >= deadline) status = CALL_TIMED_OUT;
            else
            {
                failure(settings->id, &err, result);
                str_copy(result->installed_version, COUNTOF(result->installed_version), update->installed_version);
                str_copy(result->available_version, COUNTOF(result->available_version), update->available_version);
            }
        }
    }
    free(current);
    return status;
}

static CallStatus provider_check_usage(void *context, const ProgramSettings *settings, ULONGLONG deadline, UsageResult *result)
{
    Providers *providers = context;
    const wchar_t *name = app_name(settings->id);
    Err err = { 0 };
    bool ok;
    switch (settings->id)
    {
    case APP_PLEX: ok = check_plex_usage(providers, result, &err); break;
    case APP_SABNZBD: ok = check_sabnzbd_usage(providers, result, &err); break;
    case APP_RADARR: case APP_SONARR: ok = check_arr_usage(providers, settings, result, &err); break;
    default: ok = check_pulsarr_usage(providers, settings, result, &err);
    }
    if (ok) return CALL_OK;
    /* Nothing is listening, so the app is not running and nothing can be interrupted. */
    if (err.kind == ERR_CONNECT) usage_set(result, USAGE_IDLE, L"%s is not running.", name);
    else if (err.kind == ERR_TIMEOUT && GetTickCount64() >= deadline) return CALL_TIMED_OUT;
    else
        usage_set(result, USAGE_UNKNOWN, L"Could not check whether %s is in use, so the update was not installed.%s%s", name,
            err.kind == ERR_PROVIDER ? L" " : L"", err.kind == ERR_PROVIDER ? err.msg : L"");
    return CALL_OK;
}

UpdateProvider providers_interface(Providers *providers)
{
    UpdateProvider provider = { providers, provider_check, provider_install, provider_check_usage };
    return provider;
}
