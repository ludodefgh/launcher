#include "app_registry.h"
#include "nvs_state.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "app_registry";

#define APP_REGISTRY_MAX_SLOTS NVS_STATE_MAX_APP_SLOTS

static char s_partition_labels[APP_REGISTRY_MAX_SLOTS][sizeof(((esp_partition_t *)0)->label)];
static size_t s_count;

void app_registry_init(void) {
    s_count = 0;

    /* esp_partition_find()'s iteration order is not partition table
     * declaration order (its internal list is built via
     * SLIST_INSERT_HEAD, so iteration comes back reversed) -- collect
     * matching partitions first, then sort by subtype below for
     * deterministic (ota_0, ota_1, ...) slot ordering. */
    const esp_partition_t *found[APP_REGISTRY_MAX_SLOTS];
    size_t found_count = 0;
    int total_ota_slots = 0;

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && part->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            total_ota_slots++;
            if (found_count < APP_REGISTRY_MAX_SLOTS) {
                found[found_count++] = part;
            }
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (total_ota_slots > (int)APP_REGISTRY_MAX_SLOTS) {
        ESP_LOGW(TAG,
                 "partition table has %d ota_* slot(s), only the first %d (by subtype) are usable -- "
                 "see NVS_STATE_MAX_APP_SLOTS",
                 total_ota_slots, (int)APP_REGISTRY_MAX_SLOTS);
    }

    /* Insertion sort by subtype -- found_count is small (<= 8), not worth
     * pulling in qsort() for this. */
    for (size_t i = 1; i < found_count; i++) {
        const esp_partition_t *key = found[i];
        size_t j = i;
        while (j > 0 && found[j - 1]->subtype > key->subtype) {
            found[j] = found[j - 1];
            j--;
        }
        found[j] = key;
    }

    for (size_t i = 0; i < found_count; i++) {
        snprintf(s_partition_labels[i], sizeof(s_partition_labels[i]), "%s", found[i]->label);
    }
    s_count = found_count;

    ESP_LOGI(TAG, "found %u app slot(s) in the partition table", (unsigned)s_count);
}

size_t app_registry_count(void) {
    return s_count;
}

const char *app_registry_partition_label(size_t i) {
    if (i >= s_count) {
        return "";
    }
    return s_partition_labels[i];
}

bool app_registry_slot_is_flashed(size_t i) {
    if (i >= s_count) {
        return false;
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, s_partition_labels[i]);
    esp_app_desc_t desc;
    return part != NULL && esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

const char *app_registry_resolve_label(size_t i, char *buf, size_t buf_len) {
    if (i >= s_count) {
        return "";
    }
    bool found = false;
    if (nvs_state_get_slot_name(i, buf, buf_len, &found) == ESP_OK && found && buf[0] != '\0') {
        return buf;
    }
    return s_partition_labels[i];
}

bool app_registry_get_version(size_t i, char *out_version, size_t out_version_len) {
    if (i >= s_count) {
        return false;
    }

    /* Prefer the version recorded from the OTA manifest at download time
     * over esp_app_desc_t.version -- that field is just as unreliable for
     * non-ESP-IDF-native build systems as project_name was (issue #22):
     * PlatformIO/Arduino-framework guests populate it with an internal
     * build-tool string (e.g. a short git hash), not the guest's own
     * version -- see issue #26. Falls back to esp_app_desc_t.version only
     * for slots never OTA-downloaded through the launcher. */
    bool found = false;
    if (nvs_state_get_slot_version(i, out_version, out_version_len, &found) == ESP_OK && found &&
        out_version[0] != '\0') {
        return true;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, s_partition_labels[i]);
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
