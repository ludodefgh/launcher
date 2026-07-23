#include "net_version_check.h"
#include "net_manifest.h"
#include "net_wifi.h"

#include <string.h>

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "net_version_check";

typedef struct {
    char label[NET_MANIFEST_SLOT_LEN];
    bool has_update;
} slot_result_t;

static slot_result_t s_results[NET_MANIFEST_MAX_ENTRIES];
static size_t s_count;

void net_version_check_run(void) {
    s_count = 0;

    if (!net_wifi_connect()) {
        ESP_LOGW(TAG, "no WiFi -- skipping version check this boot");
        return;
    }

    net_manifest_t manifest;
    if (net_manifest_fetch(&manifest) != ESP_OK) {
        ESP_LOGW(TAG, "manifest fetch failed -- skipping version check this boot");
        return;
    }

    for (size_t i = 0; i < manifest.count && s_count < NET_MANIFEST_MAX_ENTRIES; i++) {
        const net_manifest_entry_t *entry = &manifest.entries[i];
        if (entry->slot[0] == '\0') {
            continue;
        }

        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, entry->slot);
        if (part == NULL) {
            continue;
        }

        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
            continue; /* likely unflashed/invalid slot, nothing to compare against */
        }

        bool has_update = strncmp(desc.version, entry->version, sizeof(desc.version)) != 0;
        strncpy(s_results[s_count].label, entry->slot, sizeof(s_results[s_count].label) - 1);
        s_results[s_count].label[sizeof(s_results[s_count].label) - 1] = '\0';
        s_results[s_count].has_update = has_update;
        s_count++;

        if (has_update) {
            ESP_LOGI(TAG, "update available for '%s': local=%s remote=%s", entry->slot, desc.version, entry->version);
        }
    }
}

bool net_version_check_has_update(const char *partition_label) {
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_results[i].label, partition_label) == 0) {
            return s_results[i].has_update;
        }
    }
    return false;
}
