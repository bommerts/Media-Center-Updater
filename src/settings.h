/* Loads and saves settings and the activity log in the app's data folder. */
#ifndef MCU_SETTINGS_H
#define MCU_SETTINGS_H

#include "models.h"

typedef struct SettingsStore
{
    wchar_t data_directory[PATHN];
    wchar_t load_warning[256];      /* empty when the last load had nothing to report */
    CRITICAL_SECTION lock;
} SettingsStore;

void store_init(SettingsStore *store, const wchar_t *data_directory);   /* NULL uses the app's data folder */
void store_destroy(SettingsStore *store);
void store_load(SettingsStore *store, AppSettings *settings);
/* API keys are encrypted for the current Windows user. On failure `error` says why. */
bool store_save(SettingsStore *store, const AppSettings *settings, wchar_t *error, size_t error_cap);
bool store_log(SettingsStore *store, const ActivityEntry *entry);
/* The most recent entries, newest first. Returns how many were read. */
int store_read_recent_activity(SettingsStore *store, ActivityEntry *entries, int max_entries);

#endif
