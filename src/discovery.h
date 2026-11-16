/* Reads this computer's existing setup without contacting or changing any application. */
#ifndef MCU_DISCOVERY_H
#define MCU_DISCOVERY_H

#include "models.h"

/* `message` never contains the key itself. */
typedef struct KeyDiscovery { bool found; wchar_t api_key[KEYN]; wchar_t message[MSGN]; } KeyDiscovery;
typedef struct PulsarrSetup { bool supported; wchar_t message[MSGN]; } PulsarrSetup;

/* Finds Radarr's or Sonarr's API key in its settings file. `data_root` is NULL except in tests. */
void discover_api_key(ApplicationId id, const wchar_t *server_url, const wchar_t *data_root, KeyDiscovery *out);

void pulsarr_data_directory(wchar_t *dst, size_t cap);
/* One value from Pulsarr's .env file. Returns false when the key is absent. `failed` reports an unreadable file. */
bool pulsarr_env_value(const wchar_t *data_directory, const wchar_t *key, wchar_t *value, size_t cap, bool *failed);
/* True when Pulsarr turns away requests from this PC that have no API key. `data_directory` is NULL except in tests. */
bool pulsarr_requires_api_key(const wchar_t *data_directory);
/* Reads an active API key from Pulsarr's own database on this PC. Never changes the database. */
void discover_pulsarr_api_key(const wchar_t *data_directory, KeyDiscovery *out);

void check_pulsarr_setup(PulsarrSetup *out);
/* `service_command` and `selected_tasks` are NULL when Windows has no such value. */
void evaluate_pulsarr_setup(bool registered, const wchar_t *expected_executable, bool executable_exists,
    bool service_exists, const wchar_t *service_command, const wchar_t *selected_tasks, PulsarrSetup *out);

#endif
