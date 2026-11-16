/* The application's single window. */
#ifndef MCU_APP_H
#define MCU_APP_H

#include "coordinator.h"

#define APP_ICON 1

/* Creates the window and its notification area icon. With `start_in_tray` the window stays hidden. */
bool main_window_create(HINSTANCE instance, AppSettings *settings, SettingsStore *store, Coordinator *coordinator,
    bool start_in_tray, int show_command);
/* Brings the window back from the notification area. Safe to call from any thread. */
void main_window_show(void);
/* Gives the window first refusal on a message, for keyboard navigation. Returns true when it was handled. */
bool main_window_handle_message(MSG *message);

#endif
