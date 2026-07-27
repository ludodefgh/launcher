#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the app-slot registry from the actual partition table at boot --
 * enumerates every ESP_PARTITION_TYPE_APP partition with a subtype in the
 * OTA range (ESP_PARTITION_SUBTYPE_APP_OTA_MIN..MAX), sorted by subtype for
 * deterministic (ota_0, ota_1, ...) ordering. Sorting is required, not just
 * defensive: verified against the actual ESP-IDF v5.5 esp_partition.c
 * source that esp_partition_find()'s own iteration order is NOT partition
 * table declaration order (its internal list is built via
 * SLIST_INSERT_HEAD, so iteration comes back reversed).
 *
 * Replaces the old hand-typed kApps[] static array (issue #24) --
 * partitions.csv is now the only place slot identity is declared; nothing
 * left to drift out of sync with it, and nothing to hand-edit when reusing
 * this launcher in a new project.
 *
 * Call once, early in app_main(), before any other function in this module
 * is used. Capped at NVS_STATE_MAX_APP_SLOTS entries -- logs a warning
 * (does not block boot) if the partition table has more OTA slots than
 * that; the excess are simply not registered. */
void app_registry_init(void);

/* Number of app slots found at app_registry_init() time. */
size_t app_registry_count(void);

/* Raw partition label for slot i (e.g. "app_slot1"), taken directly from
 * the partition table -- use for esp_partition_find_first(),
 * esp_ota_set_boot_partition(), NVS keys, etc. Empty string if i is out of
 * range; callers generally only need to guard with i < app_registry_count(). */
const char *app_registry_partition_label(size_t i);

/* True if slot i's partition actually contains a valid flashed app image,
 * checked via esp_ota_get_partition_description() succeeding -- NOT via
 * that descriptor's project_name field, which is unreliable as display text
 * (PlatformIO's Arduino-as-ESP-IDF-component framework populates it with
 * its own internal build-system name, e.g. "arduino-lib-builder", never the
 * guest sketch's actual name -- see issue #22 comment). */
bool app_registry_slot_is_flashed(size_t i);

/* Resolves the display label for slot i: prefers the name recorded by
 * net_ota.c when an OTA download last wrote into this slot (captured from
 * the OTA manifest at download time -- see nvs_state_set_slot_name()) over
 * the raw partition label. Falls back to the raw partition label (e.g.
 * "app_slot2") for slots that were never OTA-downloaded through the
 * launcher -- issue #24 traded the old hand-typed placeholder name for
 * this, so a factory-flashed slot shows an unstyled label until its first
 * OTA download (a friendlier fallback would need a "rename slot" menu
 * action, tracked as a possible follow-up, not needed here).
 *
 * buf/buf_len are scratch space (>= NVS_STATE_SLOT_NAME_LEN) owned by the
 * caller; the returned pointer may point into buf or directly at the
 * internal registry -- consume it before reusing buf for anything else.
 * Note: if a slot is OTA-downloaded once and later reflashed by some means
 * outside the launcher's OTA flow, the recorded name can go stale (there is
 * nothing in the flashed image itself this can be cross-checked against, by
 * design -- see issue #22 comment on why project_name isn't trustworthy). */
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
