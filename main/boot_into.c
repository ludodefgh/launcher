#include "boot_into.h"
#include "boot_logic.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "boot_into";

esp_err_t boot_into(const char *partition_label, bool fresh_selection) {
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found in partition table", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t first_byte = 0;
    esp_err_t err = esp_partition_read(part, 0, &first_byte, sizeof(first_byte));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read partition '%s': %s", partition_label, esp_err_to_name(err));
        return err;
    }
    if (!boot_logic_is_valid_app_magic(first_byte)) {
        ESP_LOGE(TAG, "partition '%s' does not contain a valid app image (magic=0x%02x) -- never flashed?",
                 partition_label, first_byte);
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition('%s') failed: %s", partition_label, esp_err_to_name(err));
        return err;
    }

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Crash-loop recovery (issue #23), part of the compensating fix for a
     * structural mismatch between ESP-IDF's app-rollback design (built for
     * alternating ota_0/ota_1 writes of the SAME program) and this
     * launcher's model (N fixed slots, each an independent program,
     * re-downloaded in place -- see README "Design decisions").
     *
     * esp_ota_set_boot_partition() only rewrites whichever of the two
     * otadata sectors is currently *inactive*, leaving the other one
     * untouched. If that other sector still holds an old ESP_OTA_IMG_VALID
     * record for this SAME partition (from before a re-download overwrote
     * it), a crash-loop can "roll back" to that record -- which the
     * bootloader trusts as already-confirmed and boots straight into
     * *without* going through PENDING_VERIFY again, even though the actual
     * flash contents are the new, broken image. That's a silent, permanent
     * false positive: it looks like a legitimate rollback but really just
     * reboots the same crash-looping bytes forever, marked as if trusted.
     *
     * Calling esp_ota_set_boot_partition() a second time (verified against
     * the ESP-IDF v5.5 bootloader/app_update source, not just assumed)
     * writes into the OTHER sector too, so both converge on this same
     * partition with a fresh ESP_OTA_IMG_NEW state. Either a stale VALID
     * record for this exact slot can no longer survive to deceive a future
     * rollback, and a genuine fast crash-loop correctly exhausts both
     * records within two cycles and falls through to the factory partition
     * (this launcher). The guest app must still call
     * esp_ota_mark_app_valid_cancel_rollback() early in its own startup --
     * see README.md.
     *
     * ONLY do this on a fresh_selection, not on an automatic direct-boot --
     * issue #27, tested on real hardware: applying it unconditionally on
     * every boot_into() call, including the direct-boot that runs right
     * after a crash, wiped out the exact PENDING_VERIFY/NEW state the
     * bootloader's own rollback logic depends on to notice a crash-loop in
     * the first place, silently defeating rollback on every single cycle.
     * A direct-boot must leave otadata completely undisturbed. */
    if (fresh_selection) {
        esp_err_t second_err = esp_ota_set_boot_partition(part);
        if (second_err != ESP_OK) {
            ESP_LOGW(TAG, "second esp_ota_set_boot_partition('%s') failed: %s -- rollback safety net may be weaker "
                          "this boot (a stale otadata record could survive), continuing anyway",
                     partition_label, esp_err_to_name(second_err));
        }
    }
#endif

    ESP_LOGI(TAG, "booting into '%s'", partition_label);
    esp_restart();
    return ESP_OK; /* unreachable */
}
