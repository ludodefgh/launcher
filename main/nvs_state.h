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

/* Erases the "last_app_partition" key entirely (not just an empty string --
 * a later nvs_state_get_last_app() must see *out_found = false, the same as
 * if it had never been written). Used when the remembered app gets flagged
 * unhealthy by rollback (issue #27 follow-up): forgetting it outright is
 * what actually parks the launcher at the menu instead of re-checking and
 * re-forcing the menu for the same known-bad slot on every subsequent
 * boot. */
esp_err_t nvs_state_clear_last_app(void);

/* Reads "force_menu" and resets it to false in the same call (consumed-once
 * semantics per the launcher/guest-app contract, see launcher_client.h). */
esp_err_t nvs_state_consume_force_menu(bool *out_force_menu);

/* Reads the LAUNCHER_CLIENT_PROTOCOL_VERSION last written by a guest app via
 * launcher_client.h. 0 if never written (e.g. no guest app has requested the
 * menu yet) -- not treated as an error, just "unknown". */
esp_err_t nvs_state_get_protocol_version(uint32_t *out_version);

/* Sized to comfortably hold a net_manifest_entry_t.name (NET_MANIFEST_NAME_LEN,
 * currently 64) -- duplicated rather than shared via a header include since
 * nvs_state.c/h must build unconditionally, while net_manifest.h only
 * compiles in when OTA/version-check networking is enabled. Keep >= that
 * value if it ever changes. */
#define NVS_STATE_SLOT_NAME_LEN 64
/* Upper bound on how many app slots app_registry.c will register from the
 * partition table at boot (see app_registry_init(), issue #24) --
 * slot_index beyond this is rejected rather than silently
 * truncated/aliased. */
#define NVS_STATE_MAX_APP_SLOTS 8

/* Per-slot app name, keyed by slot index (not partition label -- keeps NVS
 * key length bounded regardless of BOOT_LOGIC_MAX_LABEL_LEN). Recorded by
 * net_ota.c from the OTA manifest entry's own name at the moment a download
 * to that slot succeeds -- never read out of the flashed image itself, so
 * it isn't subject to esp_app_desc_t.project_name's unreliability for
 * non-ESP-IDF-native build systems (see issue #22 comment). *out_found is
 * false if this slot was never OTA-downloaded through the launcher (e.g. a
 * factory-flashed slot, or a slot reflashed by some other means) -- callers
 * fall back to the static app_registry.h label in that case. */
esp_err_t nvs_state_get_slot_name(size_t slot_index, char *out_name, size_t out_name_size, bool *out_found);
esp_err_t nvs_state_set_slot_name(size_t slot_index, const char *name);

/* Sized to comfortably hold both a net_manifest_entry_t.version
 * (NET_MANIFEST_VERSION_LEN) and an esp_app_desc_t.version (both currently
 * 32) -- duplicated rather than shared via a header include for the same
 * reason as NVS_STATE_SLOT_NAME_LEN above. Keep >= both if either changes. */
#define NVS_STATE_SLOT_VERSION_LEN 32

/* Per-slot app version, keyed by slot index -- same shape and rationale as
 * nvs_state_get/set_slot_name() above, just for the version string.
 * esp_app_desc_t.version is just as unreliable as project_name for
 * non-ESP-IDF-native build systems: PlatformIO/Arduino-framework guests
 * populate it with an internal build-tool string, not the guest's own
 * version (see issue #26). Recorded by net_ota.c from the OTA manifest
 * entry's own version field at download time. *out_found is false if this
 * slot was never OTA-downloaded through the launcher -- callers fall back
 * to esp_app_desc_t.version in that case (see app_registry_get_version()). */
esp_err_t nvs_state_get_slot_version(size_t slot_index, char *out_version, size_t out_version_size, bool *out_found);
esp_err_t nvs_state_set_slot_version(size_t slot_index, const char *version);

#ifdef __cplusplus
}
#endif
