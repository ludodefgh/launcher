#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compares each local slot's esp_app_desc_t.version (read straight from
 * flash, no boot required) against the remote manifest and caches the
 * result in memory. No automatic download -- purely informational, see
 * net_version_check_has_update() used by ui_menu.c to draw an indicator.
 * Requires WiFi already connected (see net_wifi_connect()); a fetch
 * failure just leaves every slot reporting "no update known", it does not
 * block boot or the menu.
 */
void net_version_check_run(void);

bool net_version_check_has_update(const char *partition_label);

#ifdef __cplusplus
}
#endif
