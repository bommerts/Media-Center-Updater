/* Windows plumbing: processes, services, the registry, hashes and signatures. */
#ifndef MCU_SYSTEM_H
#define MCU_SYSTEM_H

#include "common.h"

/* ---- Processes ---- */
/* Ids of running processes whose program name (without ".exe") matches. Returns the total found. */
int proc_find(const wchar_t *name, DWORD *ids, int max_ids);
bool proc_image_path(DWORD id, wchar_t *dst, size_t cap);
/* True when the process runs under the same Windows account as this one. `known` is false when that cannot be told. */
bool proc_same_user(DWORD id, bool *known);
void proc_append_argument(StrBuf *command_line, const wchar_t *argument);
/* Splits a command line the way Windows does. Returns a block to free with LocalFree, or NULL. */
wchar_t **proc_split_command_line(const wchar_t *command_line, int *count);

typedef enum { PROC_OK, PROC_START_FAILED, PROC_TIMED_OUT } ProcStatus;
typedef struct ProcResult { DWORD exit_code; char *output; size_t output_length; } ProcResult;
/* Runs a hidden program and waits for it. With `capture`, its output (standard output and errors together) is
   returned in `result->output`, to free with free(). A program that times out is only ended when asked. */
ProcStatus proc_run(const wchar_t *program, const wchar_t *const *arguments, int count, DWORD timeout_ms,
    bool capture, bool kill_on_timeout, ProcResult *result);
/* Starts a hidden program without waiting for it. */
bool proc_start(const wchar_t *program, const wchar_t *const *arguments, int count, const wchar_t *directory);

bool is_administrator(void);
bool current_user_sid(wchar_t *dst, size_t cap);

/* ---- Services ---- */
/* The service's current state (SERVICE_RUNNING, SERVICE_STOPPED, ...), or 0 when it does not exist or cannot be queried. */
DWORD service_state(const wchar_t *name);
bool service_running(const wchar_t *name);
/* Both return 0 on success (or when the service is already in that state), otherwise the Windows error code. */
DWORD service_start(const wchar_t *name);
DWORD service_stop(const wchar_t *name);

/* ---- Registry ---- */
/* `view` is KEY_WOW64_64KEY, KEY_WOW64_32KEY or 0. A missing value leaves `dst` empty and returns false. */
bool reg_read_string(HKEY root, const wchar_t *key, REGSAM view, const wchar_t *value, wchar_t *dst, size_t cap);
bool reg_key_exists(HKEY root, const wchar_t *key, REGSAM view);

/* ---- Files ---- */
typedef enum { HASH_SHA1, HASH_SHA256 } HashKind;
/* Upper-case hex digest of a file. */
bool file_hash(const wchar_t *path, HashKind kind, wchar_t *hex, size_t cap);
/* True when Windows trusts the file's signature. `signer` receives the name the certificate was issued to. */
bool file_signature_trusted(const wchar_t *path, wchar_t *signer, size_t cap);
/* 1 for a 64-bit program, 0 for 32-bit, -1 when the file is not a readable Windows program. */
int file_is_64bit_program(const wchar_t *path);
void new_guid_text(wchar_t *dst, size_t cap);      /* 32 hex digits */
void win_error_text(DWORD code, wchar_t *dst, size_t cap);

#endif
