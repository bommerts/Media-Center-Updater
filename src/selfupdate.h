/* Finds, downloads and starts new versions of this app's own setup program from GitHub releases. */
#ifndef MCU_SELFUPDATE_H
#define MCU_SELFUPDATE_H

#include "http.h"
#include "json.h"

#define SELF_UPDATE_RELEASES_PAGE L"https://github.com/bommerts/Media-Center-Updater/releases/latest"

typedef struct SelfUpdateInfo
{
    wchar_t version[VERN];
    wchar_t download_url[URLN];
    wchar_t sha256[80];             /* upper-case hex */
} SelfUpdateInfo;

typedef enum { SELF_UPDATE_CURRENT, SELF_UPDATE_FOUND, SELF_UPDATE_FAILED } SelfUpdateCheck;

/* True when this copy is the one the setup program installed, so the setup program can replace it. */
bool self_update_is_installed_copy(void);
/* `error` is filled in for SELF_UPDATE_FAILED. `send` is NULL except in tests. */
SelfUpdateCheck self_update_check(HttpSendFn send, void *send_context, const wchar_t *current_version,
    SelfUpdateInfo *info, wchar_t *error, size_t error_cap);
SelfUpdateCheck self_update_parse_release(const JsonValue *release, const wchar_t *current_version,
    SelfUpdateInfo *info, wchar_t *error, size_t error_cap);
bool self_update_is_allowed_download(const wchar_t *url);
/* Downloads the setup program and checks it against the release checksum. `file` receives its path. */
bool self_update_download(HttpSendFn send, void *send_context, const SelfUpdateInfo *info, const wchar_t *data_directory,
    wchar_t *file, size_t file_cap, wchar_t *error, size_t error_cap);
/* Starts a hidden helper that waits for this process to exit, checks the file again, then runs the setup
   program silently. The caller must exit right after this returns, because the setup program will not
   replace a running copy. The setup program reopens the app when it finishes. */
bool self_update_start_installer(const wchar_t *installer_path, const wchar_t *sha256, bool reopen_in_tray,
    wchar_t *error, size_t error_cap);
/* The PowerShell script that helper runs. `process_id` is the process it waits for. */
void self_update_installer_script(StrBuf *script, const wchar_t *installer_path, const wchar_t *sha256,
    bool reopen_in_tray, DWORD process_id);

#endif
