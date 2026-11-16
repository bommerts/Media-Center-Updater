/* Strings, files, folders and time. */
#include "common.h"
#include <shlobj.h>
#include <stdio.h>

/* ---- Memory ---- */

static void out_of_memory(void)
{
    FatalAppExitW(0, L"Media Center Updater ran out of memory and has to close.");
    ExitProcess(3);
}

void *xalloc(size_t size)
{
    void *block = calloc(1, size ? size : 1);
    if (!block) out_of_memory();
    return block;
}

void *xrealloc(void *block, size_t size)
{
    void *grown = realloc(block, size ? size : 1);
    if (!grown) out_of_memory();
    return grown;
}

wchar_t *xwcsdup(const wchar_t *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    wchar_t *copy = xalloc(bytes);
    memcpy(copy, text, bytes);
    return copy;
}

typedef struct ArenaBlock { struct ArenaBlock *next; size_t used, cap; } ArenaBlock;

void *arena_alloc(Arena *arena, size_t size)
{
    size = (size + 15) & ~(size_t)15;
    ArenaBlock *block = arena->head;
    if (!block || block->cap - block->used < size)
    {
        size_t cap = size > 65536 ? size : 65536;
        block = xalloc(sizeof(ArenaBlock) + cap);
        block->cap = cap;
        block->next = arena->head;
        arena->head = block;
    }
    void *result = (char *)(block + 1) + block->used;
    block->used += size;
    return result;
}

void arena_free(Arena *arena)
{
    ArenaBlock *block = arena->head;
    while (block)
    {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

/* ---- Strings ---- */

void str_copy_n(wchar_t *dst, size_t cap, const wchar_t *src, size_t count)
{
    if (!cap) return;
    size_t i = 0;
    for (; i < count && i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void str_copy(wchar_t *dst, size_t cap, const wchar_t *src)
{
    str_copy_n(dst, cap, src ? src : L"", (size_t)-1);
}

void str_cat(wchar_t *dst, size_t cap, const wchar_t *src)
{
    size_t used = wcsnlen(dst, cap);
    if (used < cap) str_copy(dst + used, cap - used, src);
}

void str_vfmt(wchar_t *dst, size_t cap, const wchar_t *format, va_list args)
{
    if (!cap) return;
    _vsnwprintf_s(dst, cap, _TRUNCATE, format, args);
}

void str_fmt(wchar_t *dst, size_t cap, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    str_vfmt(dst, cap, format, args);
    va_end(args);
}

void str_trim_chars(wchar_t *text, const wchar_t *chars)
{
    size_t start = 0, end = wcslen(text);
    while (start < end && wcschr(chars, text[start])) start++;
    while (end > start && wcschr(chars, text[end - 1])) end--;
    if (start) memmove(text, text + start, (end - start) * sizeof(wchar_t));
    text[end - start] = 0;
}

void str_trim(wchar_t *text)
{
    size_t start = 0, end = wcslen(text);
    while (start < end && iswspace(text[start])) start++;
    while (end > start && iswspace(text[end - 1])) end--;
    if (start) memmove(text, text + start, (end - start) * sizeof(wchar_t));
    text[end - start] = 0;
}

bool str_ieq(const wchar_t *a, const wchar_t *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

bool str_istarts(const wchar_t *text, const wchar_t *prefix)
{
    size_t n = wcslen(prefix);
    return wcslen(text) >= n && CompareStringOrdinal(text, (int)n, prefix, (int)n, TRUE) == CSTR_EQUAL;
}

bool str_iends(const wchar_t *text, const wchar_t *suffix)
{
    size_t n = wcslen(suffix), length = wcslen(text);
    return length >= n && CompareStringOrdinal(text + length - n, (int)n, suffix, (int)n, TRUE) == CSTR_EQUAL;
}

bool str_blank(const wchar_t *text)
{
    for (; *text; text++) if (!iswspace(*text)) return false;
    return true;
}

bool str_has_control(const wchar_t *text)
{
    for (; *text; text++) if (*text < 0x20 || (*text >= 0x7F && *text <= 0x9F)) return true;
    return false;
}

const wchar_t *str_ifind(const wchar_t *text, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    if (!n) return text;
    for (; *text; text++)
        if (CompareStringOrdinal(text, (int)wcsnlen(text, n), needle, (int)n, TRUE) == CSTR_EQUAL) return text;
    return NULL;
}

void sb_init(StrBuf *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->p = xalloc(sb->cap * sizeof(wchar_t));
}

void sb_free(StrBuf *sb)
{
    free(sb->p);
    sb->p = NULL;
    sb->len = sb->cap = 0;
}

void sb_append_n(StrBuf *sb, const wchar_t *text, size_t count)
{
    if (sb->len + count + 1 > sb->cap)
    {
        while (sb->len + count + 1 > sb->cap) sb->cap *= 2;
        sb->p = xrealloc(sb->p, sb->cap * sizeof(wchar_t));
    }
    memcpy(sb->p + sb->len, text, count * sizeof(wchar_t));
    sb->len += count;
    sb->p[sb->len] = 0;
}

void sb_append(StrBuf *sb, const wchar_t *text) { sb_append_n(sb, text, wcslen(text)); }
void sb_append_ch(StrBuf *sb, wchar_t ch) { sb_append_n(sb, &ch, 1); }

void sb_appendf(StrBuf *sb, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed <= 0) return;
    wchar_t *text = xalloc(((size_t)needed + 1) * sizeof(wchar_t));
    va_start(args, format);
    _vsnwprintf_s(text, (size_t)needed + 1, _TRUNCATE, format, args);
    va_end(args);
    sb_append_n(sb, text, (size_t)needed);
    free(text);
}

wchar_t *utf8_to_wide(const char *text, int length)
{
    if (length < 0) length = (int)strlen(text);
    int needed = length ? MultiByteToWideChar(CP_UTF8, 0, text, length, NULL, 0) : 0;
    wchar_t *wide = xalloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (needed) MultiByteToWideChar(CP_UTF8, 0, text, length, wide, needed);
    wide[needed] = 0;
    return wide;
}

char *wide_to_utf8(const wchar_t *text, size_t *length)
{
    int count = (int)wcslen(text);
    int needed = count ? WideCharToMultiByte(CP_UTF8, 0, text, count, NULL, 0, NULL, NULL) : 0;
    char *utf8 = xalloc((size_t)needed + 1);
    if (needed) WideCharToMultiByte(CP_UTF8, 0, text, count, utf8, needed, NULL, NULL);
    utf8[needed] = 0;
    if (length) *length = (size_t)needed;
    return utf8;
}

/* ---- Files and folders ---- */

FileRead file_read_all(const wchar_t *path, size_t max_bytes, char **data, size_t *length)
{
    *data = NULL;
    *length = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FR_MISSING : FR_ERROR;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return FR_ERROR; }
    if ((uint64_t)size.QuadPart > max_bytes) { CloseHandle(file); return FR_TOO_LARGE; }
    char *buffer = xalloc((size_t)size.QuadPart + 2);
    size_t total = 0;
    while (total < (size_t)size.QuadPart)
    {
        DWORD read = 0;
        if (!ReadFile(file, buffer + total, (DWORD)((size_t)size.QuadPart - total), &read, NULL))
        {
            free(buffer);
            CloseHandle(file);
            return FR_ERROR;
        }
        if (!read) break;
        total += read;
    }
    CloseHandle(file);
    buffer[total] = buffer[total + 1] = 0;
    *data = buffer;
    *length = total;
    return FR_OK;
}

static bool write_handle(HANDLE file, const void *data, size_t length)
{
    const char *bytes = data;
    while (length)
    {
        DWORD chunk = length > 0x100000 ? 0x100000 : (DWORD)length, written = 0;
        if (!WriteFile(file, bytes, chunk, &written, NULL) || !written) return false;
        bytes += written;
        length -= written;
    }
    return true;
}

bool file_write_all(const wchar_t *path, const void *data, size_t length)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = write_handle(file, data, length);
    DWORD error = GetLastError();
    CloseHandle(file);
    SetLastError(error);
    return ok;
}

bool file_append(const wchar_t *path, const void *data, size_t length)
{
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = write_handle(file, data, length);
    CloseHandle(file);
    return ok;
}

bool file_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_create(const wchar_t *path)
{
    if (dir_exists(path)) return true;
    int result = SHCreateDirectoryExW(NULL, path, NULL);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

void path_join(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b)
{
    wchar_t joined[PATHN];
    str_copy(joined, COUNTOF(joined), a);
    size_t length = wcslen(joined);
    if (length && joined[length - 1] != L'\\' && joined[length - 1] != L'/') str_cat(joined, COUNTOF(joined), L"\\");
    while (*b == L'\\' || *b == L'/') b++;
    str_cat(joined, COUNTOF(joined), b);
    str_copy(dst, cap, joined);
}

const wchar_t *path_filename(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; p++) if (*p == L'\\' || *p == L'/' || *p == L':') name = p + 1;
    return name;
}

void path_dirname(wchar_t *dst, size_t cap, const wchar_t *path)
{
    const wchar_t *name = path_filename(path);
    size_t length = (size_t)(name - path);
    /* Keep the separator of a drive root ("C:\"), drop it otherwise. */
    if (length > 3 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) length--;
    wchar_t result[PATHN];
    str_copy_n(result, COUNTOF(result), path, length);
    str_copy(dst, cap, result);
}

void path_change_extension(wchar_t *dst, size_t cap, const wchar_t *path, const wchar_t *extension)
{
    wchar_t result[PATHN];
    str_copy(result, COUNTOF(result), path);
    wchar_t *name = (wchar_t *)path_filename(result);
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot) *dot = 0;
    str_cat(result, COUNTOF(result), extension);
    str_copy(dst, cap, result);
}

bool path_is_absolute(const wchar_t *path)
{
    if (!path || !path[0]) return false;
    bool sep0 = path[0] == L'\\' || path[0] == L'/';
    if (sep0) return path[1] == L'\\' || path[1] == L'/' || path[1] == L'?';
    return iswalpha(path[0]) && path[0] < 128 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

static bool full_path(const wchar_t *path, wchar_t *dst, DWORD cap)
{
    DWORD length = GetFullPathNameW(path, cap, dst, NULL);
    if (!length || length >= cap) return false;
    while (length > 3 && (dst[length - 1] == L'\\' || dst[length - 1] == L'/')) dst[--length] = 0;
    return true;
}

bool path_same(const wchar_t *a, const wchar_t *b)
{
    wchar_t full_a[PATHN], full_b[PATHN];
    return path_is_absolute(a) && path_is_absolute(b) && full_path(a, full_a, PATHN) && full_path(b, full_b, PATHN) &&
        str_ieq(full_a, full_b);
}

static void special_folder(int csidl, wchar_t *dst, size_t cap)
{
    wchar_t path[MAX_PATH] = L"";
    SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, path);
    str_copy(dst, cap, path);
}

void folder_local_app_data(wchar_t *dst, size_t cap) { special_folder(CSIDL_LOCAL_APPDATA, dst, cap); }
void folder_program_data(wchar_t *dst, size_t cap) { special_folder(CSIDL_COMMON_APPDATA, dst, cap); }
void folder_system(wchar_t *dst, size_t cap) { special_folder(CSIDL_SYSTEM, dst, cap); }

void app_data_directory(wchar_t *dst, size_t cap)
{
#ifdef MCU_DEV
    /* Development builds can keep their settings away from the real ones. */
    wchar_t override[PATHN];
    DWORD length = GetEnvironmentVariableW(L"MCU_DEV_DATA", override, PATHN);
    if (length && length < PATHN) { str_copy(dst, cap, override); return; }
#endif
    wchar_t base[PATHN];
    folder_local_app_data(base, COUNTOF(base));
    path_join(dst, cap, base, L"MediaCenterUpdater");
}

void app_exe_path(wchar_t *dst, size_t cap)
{
    wchar_t path[PATHN] = L"";
    GetModuleFileNameW(NULL, path, PATHN);
    str_copy(dst, cap, path);
}

void app_exe_directory(wchar_t *dst, size_t cap)
{
    wchar_t path[PATHN];
    app_exe_path(path, COUNTOF(path));
    path_dirname(dst, cap, path);
}

/* ---- Time ---- */

int64_t time_now(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (int64_t)(((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime);
}

int64_t time_from_parts(const SYSTEMTIME *parts)
{
    FILETIME time;
    if (!SystemTimeToFileTime(parts, &time)) return 0;
    return (int64_t)(((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime);
}

void time_to_parts(int64_t ticks, SYSTEMTIME *parts)
{
    FILETIME time = { (DWORD)((uint64_t)ticks & 0xFFFFFFFF), (DWORD)((uint64_t)ticks >> 32) };
    if (!FileTimeToSystemTime(&time, parts)) memset(parts, 0, sizeof(*parts));
}

void time_to_local(int64_t ticks, SYSTEMTIME *local)
{
    SYSTEMTIME utc;
    time_to_parts(ticks, &utc);
    if (!SystemTimeToTzSpecificLocalTimeEx(NULL, &utc, local)) *local = utc;
}

int time_local_to_utc(const SYSTEMTIME *local, int64_t out[2])
{
    TIME_ZONE_INFORMATION zone;
    if (!GetTimeZoneInformationForYear(local->wYear, NULL, &zone)) GetTimeZoneInformation(&zone);
    int offsets[2] = { -(int)(zone.Bias + zone.StandardBias), -(int)(zone.Bias + zone.DaylightBias) };
    int candidates = zone.StandardDate.wMonth && offsets[0] != offsets[1] ? 2 : 1;
    SYSTEMTIME wall = *local;
    wall.wSecond = 0;
    wall.wMilliseconds = 0;
    int64_t wall_ticks = time_from_parts(&wall);
    int count = 0;
    for (int i = 0; i < candidates; i++)
    {
        int64_t utc = wall_ticks - offsets[i] * TICKS_PER_MINUTE;
        SYSTEMTIME back;
        time_to_local(utc, &back);
        if (back.wYear == wall.wYear && back.wMonth == wall.wMonth && back.wDay == wall.wDay &&
            back.wHour == wall.wHour && back.wMinute == wall.wMinute)
            out[count++] = utc;
    }
    if (count == 2 && out[0] > out[1]) { int64_t swap = out[0]; out[0] = out[1]; out[1] = swap; }
    return count;
}

DateTimeOffset dto_from_ticks(int64_t ticks)
{
    SYSTEMTIME local;
    time_to_local(ticks, &local);
    int64_t difference = time_from_parts(&local) - ticks;
    int64_t half = TICKS_PER_MINUTE / 2;
    DateTimeOffset value = { true, ticks, (int)((difference + (difference >= 0 ? half : -half)) / TICKS_PER_MINUTE) };
    return value;
}

DateTimeOffset dto_now(void) { return dto_from_ticks(time_now()); }

static bool read_digits(const wchar_t **cursor, int count, int *value)
{
    int result = 0;
    for (int i = 0; i < count; i++)
    {
        wchar_t ch = (*cursor)[i];
        if (ch < L'0' || ch > L'9') return false;
        result = result * 10 + (ch - L'0');
    }
    *cursor += count;
    *value = result;
    return true;
}

bool dto_parse(const wchar_t *text, DateTimeOffset *out)
{
    const wchar_t *p = text;
    int year, month, day, hour, minute, second = 0;
    if (!read_digits(&p, 4, &year) || *p++ != L'-' || !read_digits(&p, 2, &month) || *p++ != L'-' ||
        !read_digits(&p, 2, &day) || (*p != L'T' && *p != L't') || (p++, !read_digits(&p, 2, &hour)) ||
        *p++ != L':' || !read_digits(&p, 2, &minute))
        return false;
    if (*p == L':') { p++; if (!read_digits(&p, 2, &second)) return false; }
    int64_t fraction = 0;
    if (*p == L'.')
    {
        p++;
        int digits = 0;
        while (*p >= L'0' && *p <= L'9')
        {
            if (digits < 7) { fraction = fraction * 10 + (*p - L'0'); digits++; }
            p++;
        }
        if (!digits) return false;
        for (; digits < 7; digits++) fraction *= 10;
    }
    SYSTEMTIME parts = { (WORD)year, (WORD)month, 0, (WORD)day, (WORD)hour, (WORD)minute, (WORD)second, 0 };
    FILETIME check;
    if (!SystemTimeToFileTime(&parts, &check)) return false;
    int64_t wall = time_from_parts(&parts) + fraction;
    int offset;
    if (*p == L'Z' || *p == L'z') { offset = 0; p++; }
    else if (*p == L'+' || *p == L'-')
    {
        int sign = *p++ == L'-' ? -1 : 1, hours, minutes;
        if (!read_digits(&p, 2, &hours) || *p++ != L':' || !read_digits(&p, 2, &minutes) || hours > 14 || minutes > 59) return false;
        offset = sign * (hours * 60 + minutes);
    }
    else
    {
        /* No offset means local time. */
        int64_t utc[2];
        if (!time_local_to_utc(&parts, utc)) return false;
        offset = (int)((wall - fraction - utc[0]) / TICKS_PER_MINUTE);
    }
    if (*p) return false;
    out->has = true;
    out->ticks = wall - offset * TICKS_PER_MINUTE;
    out->offset_minutes = offset;
    return true;
}

void dto_format(const DateTimeOffset *value, wchar_t *dst, size_t cap)
{
    int64_t wall = value->ticks + value->offset_minutes * TICKS_PER_MINUTE;
    SYSTEMTIME parts;
    time_to_parts(wall, &parts);
    int offset = value->offset_minutes < 0 ? -value->offset_minutes : value->offset_minutes;
    str_fmt(dst, cap, L"%04d-%02d-%02dT%02d:%02d:%02d.%07d%c%02d:%02d", parts.wYear, parts.wMonth, parts.wDay,
        parts.wHour, parts.wMinute, parts.wSecond, (int)(wall % TICKS_PER_SECOND),
        value->offset_minutes < 0 ? L'-' : L'+', offset / 60, offset % 60);
}

void time_format_local(int64_t ticks, const wchar_t *date_format, const wchar_t *joiner,
    const wchar_t *time_format, wchar_t *dst, size_t cap)
{
    SYSTEMTIME local;
    time_to_local(ticks, &local);
    wchar_t date[80] = L"", time[80] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, date_format, date, COUNTOF(date), NULL);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, time_format, time, COUNTOF(time));
    str_fmt(dst, cap, L"%s%s%s", date, joiner, time);
}

/* ---- Errors ---- */

bool err_set(Err *err, ErrKind kind, const wchar_t *format, ...)
{
    if (!err) return false;
    err->kind = kind;
    va_list args;
    va_start(args, format);
    str_vfmt(err->msg, COUNTOF(err->msg), format, args);
    va_end(args);
    return false;
}
