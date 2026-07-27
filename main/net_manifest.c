#include "net_manifest.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "net_http_util.h"

static const char *TAG = "net_manifest";

#define MANIFEST_MAX_BYTES 8192

static esp_err_t fetch_to_buffer(const char *url, char **out_buf, int *out_len) {
    bool is_https = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
        /* GitHub Releases redirects (see net_http_util.c) land on a
         * presigned URL with a long query string (signature, expiry, ...),
         * easily 400-800+ chars -- the default 512-byte buffer isn't
         * enough to build the follow-up request and esp_http_client_open()
         * fails with "Out of buffer" before ever reaching the server. */
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = net_http_open_and_follow_redirects(client, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "manifest GET '%s' returned HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(MANIFEST_MAX_BYTES + 1);
    if (buf == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < MANIFEST_MAX_BYTES) {
        int n = esp_http_client_read(client, buf + total, MANIFEST_MAX_BYTES - total);
        if (n < 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

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
    esp_err_t err = fetch_to_buffer(url, &body, &body_len);
    if (err != ESP_OK) {
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
        if (dst->url[0] == '\0') {
            continue; /* url is mandatory, skip malformed entries */
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
