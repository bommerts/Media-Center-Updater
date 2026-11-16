/* Uses official public release feeds and each application's own Windows updater. */
#ifndef MCU_PROVIDERS_H
#define MCU_PROVIDERS_H

#include "http.h"
#include "json.h"
#include "models.h"

typedef struct Providers
{
    HttpSendFn send;                            /* how requests are sent; tests supply canned responses */
    void *send_context;
    wchar_t plex_address[128];
    const wchar_t *sabnzbd_config_override;     /* tests only */
    const wchar_t *pulsarr_data_override;       /* tests only */
} Providers;

void providers_init(Providers *providers, HttpSendFn send, void *send_context);   /* send NULL: real requests */
UpdateProvider providers_interface(Providers *providers);

/* How many installation logs are kept for each app, and for this app's own setup program. Pulsarr's installer
   writes several megabytes on every update, so keeping them all fills the data folder. */
#define INSTALLER_LOGS_KEPT 3
/* Removes old installation logs and any installer left behind in the data folder's Downloads folder. */
void providers_prune_downloads(const wchar_t *data_directory);

typedef struct Version { int part[4]; } Version;
/* Accepts "1.2", "v1.2.3", "1.2.3.4-beta" and similar. Missing parts count as zero. */
bool version_parse(const wchar_t *text, Version *out);
int version_compare(const Version *a, const Version *b);

typedef struct SabnzbdConfig
{
    wchar_t host[256];
    int port;
    bool https;
    wchar_t url_base[256];
    wchar_t api_key[KEYN];
} SabnzbdConfig;

void read_sabnzbd_config(const wchar_t *ini, SabnzbdConfig *out);
bool count_plex_sessions(const JsonValue *root, int *count);
void read_pulsarr_address(const wchar_t *data_directory, int *port, wchar_t *base_path, size_t cap);

#endif
