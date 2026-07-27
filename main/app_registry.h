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

#ifdef __cplusplus
}
#endif
