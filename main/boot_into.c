#include "boot_into.h"
#include "boot_logic.h"
#include "nvs_state.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "boot_into";

esp_err_t boot_into(const char *partition_label) {
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

    /* Crash-loop failsafe (issue #23): record when this attempt started so
     * that if the app crashes fast, the next boot can tell. Best-effort --
     * a failure here just means the next boot can't measure elapsed time
     * (treated as "not a fast crash", see nvs_state_get_boot_attempt_elapsed_us),
     * not worth aborting the boot over. */
    esp_err_t mark_err = nvs_state_mark_boot_attempt_started();
    if (mark_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_state_mark_boot_attempt_started failed: %s", esp_err_to_name(mark_err));
    }

    ESP_LOGI(TAG, "booting into '%s'", partition_label);
    esp_restart();
    return ESP_OK; /* unreachable */
}

void boot_check_slot_count_consistency(void) {
    int found = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            part->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            found++;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (!boot_logic_slot_count_matches(found, CONFIG_LAUNCHER_APP_SLOT_COUNT)) {
        ESP_LOGW(TAG,
                 "partition table has %d ota_* slot(s) but CONFIG_LAUNCHER_APP_SLOT_COUNT=%d -- "
                 "check partitions.csv / app_registry.c / menuconfig are in sync",
                 found, CONFIG_LAUNCHER_APP_SLOT_COUNT);
    }
}
