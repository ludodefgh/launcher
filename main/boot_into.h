#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware/ESP-IDF-dependent half of the boot sequence: finds the target
 * partition, refuses to jump into an unflashed/invalid slot (magic byte
 * check, see boot_logic_is_valid_app_magic), then calls
 * esp_ota_set_boot_partition() + esp_restart(). Returns an error instead of
 * restarting if the partition is missing or looks invalid, so the caller
 * can show a message and stay in the menu instead of crash-looping.
 *
 * fresh_selection: true for a deliberate new pick (menu selection, remote
 * control boot-by-label, a fresh OTA download destination) -- forces both
 * otadata sectors to converge on this partition with a clean state, to
 * compensate for a stale ESP_OTA_IMG_VALID record possibly left over from
 * before an in-place re-download of this same slot (see issue #23, README
 * "Design decisions"). false for the automatic direct-boot of the
 * remembered last app -- MUST leave otadata state undisturbed there, since
 * ESP-IDF's app-rollback crash-loop detection depends on the
 * PENDING_VERIFY/NEW state carrying over unchanged from the previous boot
 * attempt for the bootloader to ever see "this slot never confirmed
 * itself, roll back". Getting this wrong (fresh_selection behavior applied
 * unconditionally on every call, including direct-boot) silently defeated
 * rollback on every single cycle -- see issue #27.
 */
esp_err_t boot_into(const char *partition_label, bool fresh_selection);

#ifdef __cplusplus
}
#endif
