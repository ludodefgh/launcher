#include "boot_into.h"
#include "boot_logic.h"

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

    ESP_LOGI(TAG, "booting into '%s'", partition_label);
    esp_restart();
    return ESP_OK; /* unreachable */
}
