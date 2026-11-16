/* Shared includes, limits and small helpers used by every module. */
#ifndef MCU_COMMON_H
#define MCU_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PATHN 1024   /* file paths */
#define URLN 2084    /* URLs */
#define KEYN 1100    /* API keys (limit is 1024 characters) */
#define MSGN 1024    /* messages shown to the user */
#define VERN 96      /* version strings */

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* ---- Memory. Allocation failure ends the process, so callers never see NULL. ---- */
void *xalloc(size_t size);
void *xrealloc(void *block, size_t size);
wchar_t *xwcsdup(const wchar_t *text);

typedef struct Arena { struct ArenaBlock *head; } Arena;
void *arena_alloc(Arena *arena, size_t size);
void arena_free(Arena *arena);

/* ---- Strings. Every copy truncates rather than overflowing. ---- */
void str_copy(wchar_t *dst, size_t cap, const wchar_t *src);
void str_copy_n(wchar_t *dst, size_t cap, const wchar_t *src, size_t count);
void str_cat(wchar_t *dst, size_t cap, const wchar_t *src);
void str_fmt(wchar_t *dst, size_t cap, const wchar_t *format, ...);
void str_vfmt(wchar_t *dst, size_t cap, const wchar_t *format, va_list args);
void str_trim(wchar_t *text);
void str_trim_chars(wchar_t *text, const wchar_t *chars);
bool str_ieq(const wchar_t *a, const wchar_t *b);
bool str_istarts(const wchar_t *text, const wchar_t *prefix);
bool str_iends(const wchar_t *text, const wchar_t *suffix);
bool str_blank(const wchar_t *text);
bool str_has_control(const wchar_t *text);
const wchar_t *str_ifind(const wchar_t *text, const wchar_t *needle);

typedef struct StrBuf { wchar_t *p; size_t len, cap; } StrBuf;
void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_append(StrBuf *sb, const wchar_t *text);
void sb_append_n(StrBuf *sb, const wchar_t *text, size_t count);
void sb_append_ch(StrBuf *sb, wchar_t ch);
void sb_appendf(StrBuf *sb, const wchar_t *format, ...);

wchar_t *utf8_to_wide(const char *text, int length);          /* length -1: NUL terminated */
char *wide_to_utf8(const wchar_t *text, size_t *length);      /* length may be NULL */

/* ---- Files and folders. ---- */
typedef enum { FR_OK, FR_MISSING, FR_TOO_LARGE, FR_ERROR } FileRead;
FileRead file_read_all(const wchar_t *path, size_t max_bytes, char **data, size_t *length);
bool file_write_all(const wchar_t *path, const void *data, size_t length);
bool file_append(const wchar_t *path, const void *data, size_t length);
bool file_exists(const wchar_t *path);
bool dir_exists(const wchar_t *path);
bool dir_create(const wchar_t *path);
void path_join(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b);
void path_dirname(wchar_t *dst, size_t cap, const wchar_t *path);
const wchar_t *path_filename(const wchar_t *path);
void path_change_extension(wchar_t *dst, size_t cap, const wchar_t *path, const wchar_t *extension);
bool path_is_absolute(const wchar_t *path);
bool path_same(const wchar_t *a, const wchar_t *b);
void folder_local_app_data(wchar_t *dst, size_t cap);
void folder_program_data(wchar_t *dst, size_t cap);
void folder_system(wchar_t *dst, size_t cap);
void app_data_directory(wchar_t *dst, size_t cap);   /* %LOCALAPPDATA%\MediaCenterUpdater */
void app_exe_path(wchar_t *dst, size_t cap);
void app_exe_directory(wchar_t *dst, size_t cap);

/* ---- Time. Ticks are 100 ns units since 1601 UTC (the FILETIME scale). ---- */
#define TICKS_PER_SECOND 10000000LL
#define TICKS_PER_MINUTE (60 * TICKS_PER_SECOND)
#define TICKS_PER_HOUR (60 * TICKS_PER_MINUTE)
#define TICKS_PER_DAY (24 * TICKS_PER_HOUR)

typedef struct DateTimeOffset { bool has; int64_t ticks; int offset_minutes; } DateTimeOffset;
int64_t time_now(void);
int64_t time_from_parts(const SYSTEMTIME *parts);
void time_to_parts(int64_t ticks, SYSTEMTIME *parts);
void time_to_local(int64_t ticks, SYSTEMTIME *local);
/* The UTC instants a local wall-clock time maps to: none in a spring-forward gap, two in a repeated hour. */
int time_local_to_utc(const SYSTEMTIME *local, int64_t out[2]);
DateTimeOffset dto_from_ticks(int64_t ticks);
DateTimeOffset dto_now(void);
bool dto_parse(const wchar_t *text, DateTimeOffset *out);
void dto_format(const DateTimeOffset *value, wchar_t *dst, size_t cap);
/* Formats in local time using the user's month names, for example "MMM d" and "h:mm tt". */
void time_format_local(int64_t ticks, const wchar_t *date_format, const wchar_t *joiner,
    const wchar_t *time_format, wchar_t *dst, size_t cap);

/* ---- Errors. Only ERR_PROVIDER messages are safe to show as written. ---- */
typedef enum { ERR_NONE, ERR_PROVIDER, ERR_TIMEOUT, ERR_NETWORK, ERR_CONNECT, ERR_ACCESS, ERR_OTHER } ErrKind;
typedef struct Err { ErrKind kind; wchar_t msg[MSGN]; } Err;
bool err_set(Err *err, ErrKind kind, const wchar_t *format, ...);   /* always returns false */

#endif
