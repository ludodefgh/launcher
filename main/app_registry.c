#include "app_registry.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"

const launcher_app_entry_t kApps[] = {
    {"ASCII Aquarium", "app_slot1"},
    {"Slot 2", "app_slot2"},
    {"Slot 3", "app_slot3"},
};

const size_t kAppsCount = sizeof(kApps) / sizeof(kApps[0]);

bool app_registry_slot_is_flashed(size_t i) {
    if (i >= kAppsCount) {
        return false;
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, kApps[i].partition_label);
    esp_app_desc_t desc;
    return part != NULL && esp_ota_get_partition_description(part, &desc) == ESP_OK;
}
