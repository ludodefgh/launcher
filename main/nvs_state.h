#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_STATE_MAX_LABEL_LEN 32

/* Initializes NVS flash, handling the standard ESP-IDF "no free pages" /
 * version-mismatch recovery pattern (erase + re-init) so a corrupted or
 * outdated NVS partition never blocks boot. */
esp_err_t nvs_state_init(void);

/* Reads "last_app_partition" from the CONFIG_LAUNCHER_NVS_NAMESPACE namespace.
 * *out_found is false (not an error) if the key has never been written. */
esp_err_t nvs_state_get_last_app(char *out_label, size_t out_label_size, bool *out_found);

esp_err_t nvs_state_set_last_app(const char *label);

/* Reads "force_menu" and resets it to false in the same call (consumed-once
 * semantics per the launcher/guest-app contract, see launcher_client.h). */
esp_err_t nvs_state_consume_force_menu(bool *out_force_menu);

/* Reads the LAUNCHER_CLIENT_PROTOCOL_VERSION last written by a guest app via
 * launcher_client.h. 0 if never written (e.g. no guest app has requested the
 * menu yet) -- not treated as an error, just "unknown". */
esp_err_t nvs_state_get_protocol_version(uint32_t *out_version);

#ifdef __cplusplus
}
#endif
