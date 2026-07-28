#include "net_manifest.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "net_http_util.h"

static const char *TAG = "net_manifest";

#define MANIFEST_MAX_BYTES 8192

static void copy_json_string(cJSON *obj, const char *key, char *dst, size_t dst_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

esp_err_t net_manifest_fetch(net_manifest_t *out) {
    memset(out, 0, sizeof(*out));

    if (CONFIG_LAUNCHER_NET_OTA_URL_BASE[0] == '\0') {
        ESP_LOGE(TAG, "CONFIG_LAUNCHER_NET_OTA_URL_BASE is not set");
        return ESP_ERR_INVALID_STATE;
    }

    char url[300];
    snprintf(url, sizeof(url), "%s/manifest.json", CONFIG_LAUNCHER_NET_OTA_URL_BASE);

    char *body = NULL;
    int body_len = 0;
    esp_err_t err = net_http_fetch_to_buffer(url, MANIFEST_MAX_BYTES, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest GET '%s' failed: %s", url, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "manifest JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        ESP_LOGE(TAG, "manifest missing 'apps' array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize(apps);
    for (int i = 0; i < count && out->count < NET_MANIFEST_MAX_ENTRIES; i++) {
        cJSON *entry = cJSON_GetArrayItem(apps, i);
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        net_manifest_entry_t *dst = &out->entries[out->count];
        copy_json_string(entry, "name", dst->name, sizeof(dst->name));
        copy_json_string(entry, "slot", dst->slot, sizeof(dst->slot));
        copy_json_string(entry, "version", dst->version, sizeof(dst->version));
        copy_json_string(entry, "url", dst->url, sizeof(dst->url));
        copy_json_string(entry, "github_repo", dst->github_repo, sizeof(dst->github_repo));
        if (dst->url[0] == '\0' && dst->github_repo[0] == '\0') {
            continue; /* need at least one of url or github_repo, skip malformed entries */
        }
        cJSON *size_item = cJSON_GetObjectItemCaseSensitive(entry, "size");
        dst->size = cJSON_IsNumber(size_item) ? (uint32_t)size_item->valuedouble : 0;
        out->count++;
    }

    cJSON_Delete(root);

    if (count > (int)NET_MANIFEST_MAX_ENTRIES) {
        ESP_LOGW(TAG, "manifest has %d entries, only first %d kept", count, NET_MANIFEST_MAX_ENTRIES);
    }

    ESP_LOGI(TAG, "fetched manifest: %d app(s)", (int)out->count);
    return ESP_OK;
}
