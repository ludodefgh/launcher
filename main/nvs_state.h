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

#ifdef __cplusplus
}
#endif
