#include "system.h"
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <wintrust.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

/* ---- Processes ---- */

int proc_find(const wchar_t *name, DWORD *ids, int max_ids)
{
    int found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        wchar_t program[MAX_PATH];
        str_copy(program, COUNTOF(program), entry.szExeFile);
        if (str_iends(program, L".exe")) program[wcslen(program) - 4] = 0;
        if (!str_ieq(program, name)) continue;
        if (found < max_ids) ids[found] = entry.th32ProcessID;
        found++;
    }
    CloseHandle(snapshot);
    return found;
}

bool proc_image_path(DWORD id, wchar_t *dst, size_t cap)
{
    wchar_t path[PATHN];
    DWORD size = PATHN;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!process) return false;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (ok) str_copy(dst, cap, path);
    return ok;
}

static TOKEN_USER *token_user(HANDLE process)
{
    HANDLE token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return NULL;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = size ? xalloc(size) : NULL;
    if (user && !GetTokenInformation(token, TokenUser, user, size, &size))
    {
        free(user);
        user = NULL;
    }
    CloseHandle(token);
    return user;
}

bool proc_same_user(DWORD id, bool *known)
{
    *known = false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!process) return false;
    TOKEN_USER *owner = token_user(process), *current = token_user(GetCurrentProcess());
    CloseHandle(process);
    bool same = owner && current && EqualSid(owner->User.Sid, current->User.Sid);
    *known = owner && current;
    free(owner);
    free(current);
    return same;
}

void proc_append_argument(StrBuf *command_line, const wchar_t *argument)
{
    if (command_line->len) sb_append_ch(command_line, L' ');
    if (*argument && !wcspbrk(argument, L" \t\n\v\""))
    {
        sb_append(command_line, argument);
        return;
    }
    sb_append_ch(command_line, L'"');
    for (const wchar_t *p = argument;; p++)
    {
        size_t backslashes = 0;
        while (*p == L'\\') { p++; backslashes++; }
        if (!*p)
        {
            /* Backslashes before the closing quote are doubled so the quote still closes. */
            for (size_t i = 0; i < backslashes * 2; i++) sb_append_ch(command_line, L'\\');
            break;
        }
        if (*p == L'"') backslashes = backslashes * 2 + 1;
        for (size_t i = 0; i < backslashes; i++) sb_append_ch(command_line, L'\\');
        sb_append_ch(command_line, *p);
    }
    sb_append_ch(command_line, L'"');
}

wchar_t **proc_split_command_line(const wchar_t *command_line, int *count)
{
    return CommandLineToArgvW(command_line, count);
}

static void build_command_line(StrBuf *command_line, const wchar_t *program, const wchar_t *const *arguments, int count)
{
    sb_init(command_line);
    proc_append_argument(command_line, program);
    for (int i = 0; i < count; i++) proc_append_argument(command_line, arguments[i]);
}

static void drain_pipe(HANDLE pipe, ProcResult *result, size_t *capacity)
{
    for (;;)
    {
        DWORD available = 0, read = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) || !available) return;
        if (result->output_length + available + 1 > *capacity)
        {
            *capacity = (result->output_length + available + 1) * 2;
            result->output = xrealloc(result->output, *capacity);
        }
        if (!ReadFile(pipe, result->output + result->output_length, available, &read, NULL) || !read) return;
        result->output_length += read;
        result->output[result->output_length] = 0;
    }
}

ProcStatus proc_run(const wchar_t *program, const wchar_t *const *arguments, int count, DWORD timeout_ms,
    bool capture, bool kill_on_timeout, ProcResult *result)
{
    memset(result, 0, sizeof(*result));
    StrBuf command_line;
    build_command_line(&command_line, program, arguments, count);

    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE read_pipe = NULL, write_pipe = NULL, nul = INVALID_HANDLE_VALUE;
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = { 0 };
    if (capture)
    {
        if (!CreatePipe(&read_pipe, &write_pipe, &inherit, 0)) { sb_free(&command_line); return PROC_START_FAILED; }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul;
        startup.hStdOutput = write_pipe;
        startup.hStdError = write_pipe;
    }
    BOOL started = CreateProcessW(program, command_line.p, NULL, NULL, capture, CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
    sb_free(&command_line);
    if (write_pipe) CloseHandle(write_pipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started)
    {
        if (read_pipe) CloseHandle(read_pipe);
        return PROC_START_FAILED;
    }
    CloseHandle(process.hThread);

    ProcStatus status = PROC_OK;
    size_t capacity = 0;
    ULONGLONG started_at = GetTickCount64();
    for (;;)
    {
        if (capture) drain_pipe(read_pipe, result, &capacity);
        if (WaitForSingleObject(process.hProcess, capture ? 50 : 500) == WAIT_OBJECT_0)
        {
            if (capture) drain_pipe(read_pipe, result, &capacity);
            break;
        }
        if (GetTickCount64() - started_at >= timeout_ms)
        {
            if (kill_on_timeout) TerminateProcess(process.hProcess, 1);
            status = PROC_TIMED_OUT;
            break;
        }
    }
    if (status == PROC_OK) GetExitCodeProcess(process.hProcess, &result->exit_code);
    CloseHandle(process.hProcess);
    if (read_pipe) CloseHandle(read_pipe);
    if (capture && !result->output) result->output = xalloc(1);
    return status;
}

bool proc_start(const wchar_t *program, const wchar_t *const *arguments, int count, const wchar_t *directory)
{
    StrBuf command_line;
    build_command_line(&command_line, program, arguments, count);
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = { 0 };
    BOOL started = CreateProcessW(program, command_line.p, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, directory, &startup, &process);
    sb_free(&command_line);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool is_administrator(void)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(sid);
    BOOL member = FALSE;
    return CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &size) &&
        CheckTokenMembership(NULL, sid, &member) && member;
}

bool current_user_sid(wchar_t *dst, size_t cap)
{
    TOKEN_USER *user = token_user(GetCurrentProcess());
    wchar_t *text = NULL;
    bool ok = user && ConvertSidToStringSidW(user->User.Sid, &text);
    if (ok)
    {
        str_copy(dst, cap, text);
        LocalFree(text);
    }
    free(user);
    return ok;
}

/* ---- Services ---- */

static SC_HANDLE open_service(const wchar_t *name, DWORD access, SC_HANDLE *manager)
{
    *manager = name && *name ? OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT) : NULL;
    if (!*manager) return NULL;
    SC_HANDLE service = OpenServiceW(*manager, name, access);
    if (!service)
    {
        DWORD error = GetLastError();
        CloseServiceHandle(*manager);
        *manager = NULL;
        SetLastError(error);
    }
    return service;
}

DWORD service_state(const wchar_t *name)
{
    SC_HANDLE manager, service = open_service(name, SERVICE_QUERY_STATUS, &manager);
    if (!service) return 0;
    SERVICE_STATUS status;
    DWORD state = QueryServiceStatus(service, &status) ? status.dwCurrentState : 0;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return state;
}

bool service_running(const wchar_t *name) { return service_state(name) == SERVICE_RUNNING; }

DWORD service_stop(const wchar_t *name)
{
    SC_HANDLE manager, service = open_service(name, SERVICE_STOP, &manager);
    if (!service) return GetLastError() ? GetLastError() : ERROR_SERVICE_DOES_NOT_EXIST;
    SERVICE_STATUS status;
    DWORD error = ControlService(service, SERVICE_CONTROL_STOP, &status) ? 0 : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error == ERROR_SERVICE_NOT_ACTIVE ? 0 : error;
}

DWORD service_start(const wchar_t *name)
{
    SC_HANDLE manager, service = open_service(name, SERVICE_START, &manager);
    if (!service) return GetLastError() ? GetLastError() : ERROR_SERVICE_DOES_NOT_EXIST;
    DWORD error = StartServiceW(service, 0, NULL) ? 0 : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error == ERROR_SERVICE_ALREADY_RUNNING ? 0 : error;
}

/* ---- Registry ---- */

bool reg_read_string(HKEY root, const wchar_t *key, REGSAM view, const wchar_t *value, wchar_t *dst, size_t cap)
{
    HKEY handle;
    if (cap) dst[0] = 0;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &handle) != ERROR_SUCCESS) return false;
    wchar_t *buffer = xalloc(sizeof(wchar_t) * 8192);
    DWORD size = sizeof(wchar_t) * 8192;
    bool ok = RegGetValueW(handle, NULL, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, buffer, &size) == ERROR_SUCCESS;
    RegCloseKey(handle);
    if (ok) str_copy(dst, cap, buffer);
    free(buffer);
    return ok;
}

bool reg_key_exists(HKEY root, const wchar_t *key, REGSAM view)
{
    HKEY handle;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}

/* ---- Files ---- */

bool file_hash(const wchar_t *path, HashKind kind, wchar_t *hex, size_t cap)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE digest[32];
    DWORD digest_size = kind == HASH_SHA1 ? 20 : 32;
    bool ok = false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    BYTE *buffer = xalloc(1 << 16);
    if (NT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, kind == HASH_SHA1 ? BCRYPT_SHA1_ALGORITHM : BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
        NT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0)))
    {
        ok = true;
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file, buffer, 1 << 16, &read, NULL)) { ok = false; break; }
            if (!read) break;
            if (!NT_SUCCESS(BCryptHashData(hash, buffer, read, 0))) { ok = false; break; }
        }
        ok = ok && NT_SUCCESS(BCryptFinishHash(hash, digest, digest_size, 0));
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    free(buffer);
    if (!ok || cap < digest_size * 2 + 1) return false;
    for (DWORD i = 0; i < digest_size; i++) str_fmt(hex + i * 2, cap - i * 2, L"%02X", digest[i]);
    return true;
}

bool file_signature_trusted(const wchar_t *path, wchar_t *signer, size_t cap)
{
    WINTRUST_FILE_INFO file = { sizeof(file), path, NULL, NULL };
    WINTRUST_DATA data;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    memset(&data, 0, sizeof(data));
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    if (cap) signer[0] = 0;
    bool trusted = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data) == ERROR_SUCCESS;
    if (trusted)
    {
        /* Read the name from the same certificate Windows just verified. */
        CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR *chain = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT *certificate = chain ? WTHelperGetProvCertFromChain(chain, 0) : NULL;
        if (certificate && certificate->pCert)
            CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, signer, (DWORD)cap);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
    return trusted;
}

int file_is_64bit_program(const wchar_t *path)
{
    BYTE header[64], pe[6];
    DWORD read = 0;
    int result = -1;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    if (ReadFile(file, header, sizeof(header), &read, NULL) && read == sizeof(header) && header[0] == 'M' && header[1] == 'Z')
    {
        LARGE_INTEGER offset;
        offset.QuadPart = (LONG)(header[0x3C] | (header[0x3D] << 8) | (header[0x3E] << 16) | ((DWORD)header[0x3F] << 24));
        if (offset.QuadPart >= 0 && SetFilePointerEx(file, offset, NULL, FILE_BEGIN) &&
            ReadFile(file, pe, sizeof(pe), &read, NULL) && read == sizeof(pe) &&
            pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)
            result = (pe[4] | (pe[5] << 8)) == 0x8664 ? 1 : 0;
    }
    CloseHandle(file);
    return result;
}

void new_guid_text(wchar_t *dst, size_t cap)
{
    BYTE bytes[16];
    if (!NT_SUCCESS(BCryptGenRandom(NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    {
        /* Uniqueness is all that is needed here, so the clock is an acceptable fallback. */
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        memset(bytes, 0, sizeof(bytes));
        memcpy(bytes, &counter, sizeof(counter));
        bytes[8] = (BYTE)GetCurrentProcessId();
        bytes[9] = (BYTE)GetCurrentThreadId();
    }
    if (cap) dst[0] = 0;
    for (int i = 0; i < 16 && (size_t)i * 2 + 3 <= cap; i++) str_fmt(dst + i * 2, cap - (size_t)i * 2, L"%02x", bytes[i]);
}

void win_error_text(DWORD code, wchar_t *dst, size_t cap)
{
    wchar_t text[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text, COUNTOF(text), NULL);
    str_trim(text);
    str_copy(dst, cap, text);
}
