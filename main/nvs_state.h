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

/* Crash-loop failsafe (issue #23). Records "now" as the start of a boot
 * attempt -- called by boot_into() right before handing control to an app,
 * so a later boot can tell how long that attempt lasted. Backed by
 * gettimeofday(), which ESP-IDF's default time source keeps ticking through
 * panic/watchdog resets (the RTC power domain stays up) and only resets on
 * an actual power-on -- exactly the boundary this feature cares about. */
esp_err_t nvs_state_mark_boot_attempt_started(void);

/* Elapsed microseconds since the last nvs_state_mark_boot_attempt_started()
 * call, as of now. *out_found is false if no attempt was ever recorded
 * (e.g. very first boot ever) -- caller should treat that as "not a fast
 * crash" rather than as 0 elapsed. */
esp_err_t nvs_state_get_boot_attempt_elapsed_us(int64_t *out_elapsed_us, bool *out_found);

/* Consecutive-fast-abnormal-reset counter for whatever is currently
 * "last_app" -- see boot_logic_next_crash_streak(). Reset to 0 only when the
 * user deliberately reselects an app from the menu (main.c), never merely
 * by the menu being shown. */
esp_err_t nvs_state_get_crash_streak(uint32_t *out_streak);
esp_err_t nvs_state_set_crash_streak(uint32_t streak);

/* Sized to comfortably hold a net_manifest_entry_t.name (NET_MANIFEST_NAME_LEN,
 * currently 64) -- duplicated rather than shared via a header include since
 * nvs_state.c/h must build unconditionally, while net_manifest.h only
 * compiles in when OTA/version-check networking is enabled. Keep >= that
 * value if it ever changes. */
#define NVS_STATE_SLOT_NAME_LEN 64
/* Matches CONFIG_LAUNCHER_APP_SLOT_COUNT's Kconfig range (1-8, see
 * Kconfig.projbuild) -- slot_index beyond this is rejected rather than
 * silently truncated/aliased. */
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

#ifdef __cplusplus
}
#endif
