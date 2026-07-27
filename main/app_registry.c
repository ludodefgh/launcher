#include "app_registry.h"
#include "nvs_state.h"

#include <stdio.h>
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

const char *app_registry_resolve_label(size_t i, char *buf, size_t buf_len) {
    if (i >= kAppsCount) {
        return "";
    }
    bool found = false;
    if (nvs_state_get_slot_name(i, buf, buf_len, &found) == ESP_OK && found && buf[0] != '\0') {
        return buf;
    }
    return kApps[i].display_name;
}

bool app_registry_get_version(size_t i, char *out_version, size_t out_version_len) {
    if (i >= kAppsCount) {
        return false;
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, kApps[i].partition_label);
    esp_app_desc_t desc;
    if (part == NULL || esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        return false;
    }
    snprintf(out_version, out_version_len, "%s", desc.version);
    return true;
}

void app_registry_format_version_suffix(size_t i, char *out_suffix, size_t out_suffix_len) {
    if (out_suffix_len == 0) {
        return;
    }
    char version[APP_REGISTRY_VERSION_LEN];
    if (app_registry_get_version(i, version, sizeof(version)) && version[0] != '\0') {
        /* Capped display width (real semver-ish strings are short; a menu
         * row has limited screen space) -- also keeps this snprintf
         * statically provably within out_suffix_len for -Wformat-truncation,
         * see the doc comment's out_suffix_len contract. */
        snprintf(out_suffix, out_suffix_len, " v%.15s", version);
    } else {
        out_suffix[0] = '\0';
    }
}
