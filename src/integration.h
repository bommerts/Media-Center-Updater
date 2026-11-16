/* The notification area icon, the optional Windows startup task, and restarting as administrator. */
#ifndef MCU_INTEGRATION_H
#define MCU_INTEGRATION_H

#include "common.h"

typedef struct TrayIcon { HWND window; HICON icon; UINT message; bool added; } TrayIcon;

/* `message` is sent to `window` for clicks on the icon. Returns false when the icon cannot be shown. */
bool tray_add(TrayIcon *tray, HWND window, HICON icon, UINT message);
void tray_remove(TrayIcon *tray);

typedef enum { STARTUP_OFF, STARTUP_ON, STARTUP_UNKNOWN } StartupState;
/* `error` is filled in for STARTUP_UNKNOWN. */
StartupState startup_state(wchar_t *error, size_t error_cap);
bool startup_set(bool enabled, wchar_t *error, size_t error_cap);

/* Starts an elevated instance. The caller closes this instance after success. */
bool restart_elevated(bool in_tray, wchar_t *error, size_t error_cap);

#endif
