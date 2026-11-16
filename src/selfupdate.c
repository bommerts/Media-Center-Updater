#include "selfupdate.h"
#include "providers.h"
#include "system.h"
#include <wincrypt.h>

#define REPOSITORY L"bommerts/Media-Center-Updater"
#define DOWNLOAD_PREFIX L"https://github.com/" REPOSITORY L"/releases/download/"
/* Matches AppId in Installer\MediaCenterUpdater.iss. Inno Setup adds the "_is1" suffix. */
#define UNINSTALL_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6F1B8C2E-4A3D-4E8B-9C71-2D5A0E7F3B94}_is1"
#define MAX_INSTALLER_BYTES 500000000ULL

bool self_update_is_installed_copy(void)
{
    static volatile LONG cached = -1;
    if (cached < 0)
    {
        wchar_t location[PATHN], directory[PATHN];
        app_exe_directory(directory, COUNTOF(directory));
        bool installed = reg_read_string(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, KEY_WOW64_64KEY, L"InstallLocation",
            location, COUNTOF(location)) && location[0] && path_same(location, directory);
        InterlockedExchange(&cached, installed ? 1 : 0);
    }
    return cached == 1;
}

static SelfUpdateCheck fail(wchar_t *error, size_t cap, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    str_vfmt(error, cap, format, args);
    va_end(args);
    return SELF_UPDATE_FAILED;
}

static bool is_hex64(const wchar_t *text)
{
    if (wcslen(text) != 64) return false;
    for (; *text; text++)
        if (!((*text >= L'0' && *text <= L'9') || (*text >= L'a' && *text <= L'f') || (*text >= L'A' && *text <= L'F'))) return false;
    return true;
}

bool self_update_is_allowed_download(const wchar_t *text)
{
    Url *url = xalloc(sizeof(Url));
    bool ok = url_parse(text, url) && wcscmp(url->scheme, L"https") == 0 && !url->userinfo[0] &&
        str_istarts(text, DOWNLOAD_PREFIX) && str_iends(url->path, L"-Setup.exe") &&
        !wcsstr(url->path, L"/.") && !wcschr(url->path, L'%');
    free(url);
    return ok;
}

SelfUpdateCheck self_update_parse_release(const JsonValue *release, const wchar_t *current_version,
    SelfUpdateInfo *info, wchar_t *error, size_t error_cap)
{
    Version current, latest;
    wchar_t tag[VERN], expected[VERN + 64];
    if (!version_parse(current_version, &current)) return fail(error, error_cap, L"This app's own version could not be read.");
    if (json_is_true(release, L"prerelease") || json_is_true(release, L"draft"))
        return fail(error, error_cap, L"GitHub did not return a stable release.");
    str_copy(tag, COUNTOF(tag), json_string(release, L"tag_name"));
    str_trim(tag);
    const wchar_t *version = tag;
    while (*version == L'v') version++;
    if (!version_parse(version, &latest)) return fail(error, error_cap, L"The latest release has a version that could not be compared.");
    if (version_compare(&latest, &current) <= 0) return SELF_UPDATE_CURRENT;

    str_fmt(expected, COUNTOF(expected), L"MediaCenterUpdater-%s-Setup.exe", version);
    const JsonValue *assets = json_get(release, L"assets");
    if (!assets || assets->kind != JSON_ARRAY) return fail(error, error_cap, L"The latest release has no setup program.");
    for (const JsonValue *asset = assets->first; asset; asset = asset->next)
    {
        if (wcscmp(json_string(asset, L"name"), expected) != 0) continue;
        const wchar_t *digest = json_string(asset, L"digest"), *url = json_string(asset, L"browser_download_url");
        if (wcsncmp(digest, L"sha256:", 7) != 0 || !is_hex64(digest + 7))
            return fail(error, error_cap, L"The release did not include a SHA-256 checksum, so it cannot be installed automatically.");
        if (!self_update_is_allowed_download(url))
            return fail(error, error_cap, L"The release's setup program has an unexpected download address.");
        memset(info, 0, sizeof(*info));
        str_copy(info->version, COUNTOF(info->version), version);
        str_copy(info->download_url, COUNTOF(info->download_url), url);
        str_copy(info->sha256, COUNTOF(info->sha256), digest + 7);
        CharUpperW(info->sha256);
        return SELF_UPDATE_FOUND;
    }
    return fail(error, error_cap, L"The latest release (version %s) has no %s.", version, expected);
}

static const wchar_t *transport_problem(HttpStatus status, const wchar_t *timed_out)
{
    return status == HTTP_E_TIMEOUT ? timed_out : L"GitHub could not be reached. Check the network connection.";
}

SelfUpdateCheck self_update_check(HttpSendFn send, void *send_context, const wchar_t *current_version,
    SelfUpdateInfo *info, wchar_t *error, size_t error_cap)
{
    HttpRequest request = { 0 };
    HttpResponse response;
    request.url = L"https://api.github.com/repos/" REPOSITORY L"/releases/latest";
    request.headers = L"Accept: application/vnd.github+json\r\n";
    request.timeout_ms = 60000;
    HttpStatus status = (send ? send : http_send)(send_context, &request, &response);
    if (status != HTTP_OK) return fail(error, error_cap, L"%s", transport_problem(status, L"GitHub did not respond in time."));
    SelfUpdateCheck result;
    if (response.status < 200 || response.status >= 300)
        result = fail(error, error_cap, L"GitHub returned HTTP %d while checking for a new version.", response.status);
    else
    {
        JsonDoc *release = json_parse_utf8(response.body, response.body_length);
        result = release ? self_update_parse_release(release->root, current_version, info, error, error_cap)
            : fail(error, error_cap, L"GitHub's answer could not be read.");
        json_free(release);
    }
    http_response_free(&response);
    return result;
}

bool self_update_download(HttpSendFn send, void *send_context, const SelfUpdateInfo *info, const wchar_t *data_directory,
    wchar_t *file, size_t file_cap, wchar_t *error, size_t error_cap)
{
    wchar_t folder[PATHN], pattern[PATHN], name[VERN + 64], path[PATHN], hash[80];
    path_join(folder, COUNTOF(folder), data_directory, L"AppUpdates");
    if (!dir_create(folder))
    {
        str_copy(error, error_cap, L"The download folder could not be created.");
        return false;
    }
    /* Only keep the setup program for this attempt. */
    path_join(pattern, COUNTOF(pattern), folder, L"*.exe");
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            wchar_t old[PATHN];
            path_join(old, COUNTOF(old), folder, found.cFileName);
            DeleteFileW(old);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    str_fmt(name, COUNTOF(name), L"MediaCenterUpdater-%s-Setup.exe", info->version);
    path_join(path, COUNTOF(path), folder, name);

    Url *current = xalloc(sizeof(Url)), *next = xalloc(sizeof(Url));
    wchar_t *address = xalloc(sizeof(wchar_t) * URLN);
    ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    bool ok = false, finished = false;
    str_copy(address, URLN, info->download_url);
    error[0] = 0;
    for (int redirects = 0; redirects <= 5 && !finished; redirects++)
    {
        HttpRequest request = { 0 };
        HttpResponse response;
        request.url = address;
        request.timeout_ms = 60000;
        request.save_path = path;
        request.max_bytes = MAX_INSTALLER_BYTES;
        request.deadline = deadline;
        HttpStatus status = (send ? send : http_send)(send_context, &request, &response);
        finished = true;
        if (status == HTTP_E_TOO_LARGE) str_copy(error, error_cap, L"The setup program is larger than expected.");
        else if (status == HTTP_E_FILE) str_copy(error, error_cap, L"The setup program could not be saved.");
        else if (status != HTTP_OK) str_copy(error, error_cap, transport_problem(status, L"The download took too long."));
        else if (response.status >= 300 && response.status < 400)
        {
            /* GitHub serves release files from its own content host. */
            if (!response.location[0]) str_copy(error, error_cap, L"The download redirect had no destination.");
            else if (!url_parse(address, current) || !url_resolve(current, response.location, next) ||
                wcscmp(next->scheme, L"https") != 0 || next->userinfo[0] || !str_iends(next->host, L".githubusercontent.com"))
                str_copy(error, error_cap, L"GitHub redirected the download to an unexpected address.");
            else
            {
                url_format(next, address, URLN);
                finished = false;
            }
        }
        else if (response.status < 200 || response.status >= 300)
            str_fmt(error, error_cap, L"The download failed with HTTP %d.", response.status);
        else ok = true;
        if (status == HTTP_OK) http_response_free(&response);
    }
    if (!finished) str_copy(error, error_cap, L"The download redirected too many times.");
    free(address);
    free(current);
    free(next);

    if (ok && !file_hash(path, HASH_SHA256, hash, COUNTOF(hash)))
    {
        ok = false;
        str_copy(error, error_cap, L"The downloaded setup program could not be read.");
    }
    if (ok && !str_ieq(hash, info->sha256))
    {
        ok = false;
        str_copy(error, error_cap, L"The downloaded setup program did not match the release checksum. It was not run.");
    }
    if (ok) str_copy(file, file_cap, path);
    else DeleteFileW(path);
    return ok;
}

void self_update_prune(const wchar_t *data_directory)
{
    wchar_t folder[PATHN];
    path_join(folder, COUNTOF(folder), data_directory, L"AppUpdates");
    /* The setup program that installed this version is still needed for a moment after the app reopens. */
    files_prune(folder, L"*.exe", 0, 24);
    files_prune(folder, L"*.log", INSTALLER_LOGS_KEPT, 0);
}

/* A PowerShell single-quoted string. */
static void append_quoted(StrBuf *script, const wchar_t *value)
{
    sb_append_ch(script, L'\'');
    for (; *value; value++)
    {
        if (*value == L'\'') sb_append_ch(script, L'\'');
        sb_append_ch(script, *value);
    }
    sb_append_ch(script, L'\'');
}

void self_update_installer_script(StrBuf *script, const wchar_t *installer_path, const wchar_t *sha256,
    bool reopen_in_tray, DWORD process_id)
{
    wchar_t log[PATHN], log_argument[PATHN + 16], checksum[80];
    path_change_extension(log, COUNTOF(log), installer_path, L".log");
    str_fmt(log_argument, COUNTOF(log_argument), L"/LOG=\"%s\"", log);
    str_copy(checksum, COUNTOF(checksum), sha256);
    CharUpperW(checksum);

    /* When the app was open on screen, the setup program shows a progress window (no questions asked)
       so the user can see the update happening. From the tray it runs with no window at all. */
    sb_appendf(script, L"$ErrorActionPreference = 'Stop'\nWait-Process -Id %lu -Timeout 120 -ErrorAction SilentlyContinue\n$file = ",
        process_id);
    append_quoted(script, installer_path);
    /* Check again right before running, in case the file changed after the app verified it. */
    sb_append(script, L"\nif ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne ");
    append_quoted(script, checksum);
    sb_appendf(script, L") { exit 1 }\nStart-Process -FilePath $file -ArgumentList @('%s', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/RELAUNCH=%s', ",
        reopen_in_tray ? L"/VERYSILENT" : L"/SILENT", reopen_in_tray ? L"tray" : L"window");
    append_quoted(script, log_argument);
    sb_append(script, L")");
}

bool self_update_start_installer(const wchar_t *installer_path, const wchar_t *sha256, bool reopen_in_tray,
    wchar_t *error, size_t error_cap)
{
    if (!is_hex64(sha256))
    {
        str_copy(error, error_cap, L"Invalid checksum.");
        return false;
    }
    wchar_t system[PATHN], powershell[PATHN], directory[PATHN];
    StrBuf script;
    sb_init(&script);
    self_update_installer_script(&script, installer_path, sha256, reopen_in_tray, GetCurrentProcessId());

    DWORD chars = 0;
    DWORD bytes = (DWORD)(script.len * sizeof(wchar_t));
    CryptBinaryToStringW((const BYTE *)script.p, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
    wchar_t *encoded = xalloc(((size_t)chars + 1) * sizeof(wchar_t));
    bool ok = CryptBinaryToStringW((const BYTE *)script.p, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &chars) != 0;
    sb_free(&script);

    folder_system(system, COUNTOF(system));
    path_join(powershell, COUNTOF(powershell), system, L"WindowsPowerShell\\v1.0\\powershell.exe");
    path_dirname(directory, COUNTOF(directory), installer_path);
    const wchar_t *arguments[] = { L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-WindowStyle", L"Hidden",
        L"-EncodedCommand", encoded };
    ok = ok && proc_start(powershell, arguments, COUNTOF(arguments), directory);
    free(encoded);
    if (!ok) str_copy(error, error_cap, L"Windows could not start the update.");
    return ok;
}
