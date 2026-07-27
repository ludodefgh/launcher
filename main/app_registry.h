#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *display_name;
    const char *partition_label; /* must match the "Name" column in partitions.csv */
} launcher_app_entry_t;

/* Static slot registry for this project. A new project reusing the launcher
 * edits this array (and partitions.csv) to match its own slots -- see
 * README.md. Keep kAppsCount in sync with CONFIG_LAUNCHER_APP_SLOT_COUNT;
 * a mismatch is logged as a warning at boot (see boot_logic_slot_count_matches).
 * display_name is the maintainer-provided label shown as-is; it deliberately
 * does NOT bake in an empty/occupied qualifier -- see
 * app_registry_slot_is_flashed(), issue #22. */
extern const launcher_app_entry_t kApps[];
extern const size_t kAppsCount;

/* True if slot i's partition actually contains a valid flashed app image,
 * checked via esp_ota_get_partition_description() succeeding -- NOT via
 * that descriptor's project_name field, which is unreliable as display text
 * (PlatformIO's Arduino-as-ESP-IDF-component framework populates it with
 * its own internal build-system name, e.g. "arduino-lib-builder", never the
 * guest sketch's actual name -- see issue #22 comment). Callers combine
 * this with kApps[i].display_name to show/hide an empty-slot qualifier
 * without trusting anything inside the flashed image itself. */
bool app_registry_slot_is_flashed(size_t i);

/* Resolves the display label for slot i: prefers the name recorded by
 * net_ota.c when an OTA download last wrote into this slot (captured from
 * the OTA manifest at download time -- see nvs_state_set_slot_name()) over
 * the static kApps[i].display_name placeholder. Falls back to the static
 * label for factory-flashed slots (never went through the launcher's OTA
 * flow) or if nothing was ever recorded. buf/buf_len are scratch space
 * (>= NVS_STATE_SLOT_NAME_LEN) owned by the caller; the returned pointer
 * may point into buf or directly at kApps[i].display_name -- consume it
 * before reusing buf for anything else. Note: if a slot is OTA-downloaded
 * once and later reflashed by some means outside the launcher's OTA flow,
 * the recorded name can go stale (there is nothing in the flashed image
 * itself this can be cross-checked against, by design -- see issue #22
 * comment on why project_name isn't trustworthy). */
const char *app_registry_resolve_label(size_t i, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
