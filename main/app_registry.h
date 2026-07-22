#pragma once

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
 * a mismatch is logged as a warning at boot (see boot_logic_slot_count_matches). */
extern const launcher_app_entry_t kApps[];
extern const size_t kAppsCount;

#ifdef __cplusplus
}
#endif
