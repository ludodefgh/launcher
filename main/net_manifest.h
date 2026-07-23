#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared by net_ota.c and net_version_check.c: fetches and parses the JSON
 * manifest listing downloadable programs. See README.md for the exact
 * format. Fetched from "<CONFIG_LAUNCHER_NET_OTA_URL_BASE>/manifest.json".
 */

#define NET_MANIFEST_MAX_ENTRIES 8
#define NET_MANIFEST_NAME_LEN 64
#define NET_MANIFEST_SLOT_LEN 32
#define NET_MANIFEST_VERSION_LEN 32
#define NET_MANIFEST_URL_LEN 256

typedef struct {
    char name[NET_MANIFEST_NAME_LEN];
    char slot[NET_MANIFEST_SLOT_LEN]; /* suggested destination partition label, e.g. "app_slot1" */
    char version[NET_MANIFEST_VERSION_LEN];
    char url[NET_MANIFEST_URL_LEN]; /* absolute URL of the .bin, may point anywhere */
    uint32_t size;                  /* informational only, not trusted for erase sizing */
} net_manifest_entry_t;

typedef struct {
    net_manifest_entry_t entries[NET_MANIFEST_MAX_ENTRIES];
    size_t count;
} net_manifest_t;

/* Requires WiFi already connected (see net_wifi_connect()). */
esp_err_t net_manifest_fetch(net_manifest_t *out);

#ifdef __cplusplus
}
#endif
