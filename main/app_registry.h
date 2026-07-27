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

/* Matches esp_app_desc_t.version's field size (see esp_app_format.h). */
#define APP_REGISTRY_VERSION_LEN 32

/* Reads slot i's locally-flashed esp_app_desc_t.version (NUL-terminated)
 * into out_version. False (out_version left untouched) if the slot isn't
 * flashed -- same underlying check as app_registry_slot_is_flashed(), just
 * also returning the version string when available. Used to show the same
 * "currently installed" version consistently on both the main menu and the
 * OTA target-slot picker (net_ota.c) -- see issue #22 follow-up. */
bool app_registry_get_version(size_t i, char *out_version, size_t out_version_len);

/* Matches app_registry_format_version_suffix()'s internal display cap
 * (%.15s on the version string) -- " v" (2) + 15 + NUL (1). Declared here,
 * not derived from APP_REGISTRY_VERSION_LEN, so callers' buffers are sized
 * to what the function actually ever writes: -Wformat-truncation reasons
 * from declared array sizes, not runtime content, so an oversized buffer
 * here would force every caller to also re-cap with an explicit %.Ns. */
#define APP_REGISTRY_VERSION_SUFFIX_LEN 18

/* Formats slot i's installed-version suffix for a menu/picker row: "" if
 * not flashed or the version string is empty, otherwise " v<version>"
 * (version display-capped at 15 chars -- real semver-ish strings are much
 * shorter, and screen space is limited). Centralized (rather than each
 * caller formatting its own) so the main menu (ui_menu.c) and the OTA
 * target-slot picker (net_ota.c) are guaranteed to render the exact same
 * thing for the exact same slot -- see issue #22 follow-up ("both screens
 * coherent"). out_suffix_len should be >= APP_REGISTRY_VERSION_SUFFIX_LEN. */
void app_registry_format_version_suffix(size_t i, char *out_suffix, size_t out_suffix_len);

#ifdef __cplusplus
}
#endif
