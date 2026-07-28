#include "net_github.h"
#include "net_http_util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "net_github";

#define RELEASES_MAX_BYTES 16384
#define RELEASE_MANIFEST_MAX_BYTES 2048

static void copy_json_string(const cJSON *obj, const char *key, char *dst, size_t dst_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

esp_err_t net_github_fetch_releases(const char *repo, net_github_release_list_t *out) {
    memset(out, 0, sizeof(*out));

    char url[192];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=%d", repo,
              NET_GITHUB_MAX_RELEASES);

    char *body = NULL;
    int body_len = 0;
    esp_err_t err = net_http_fetch_to_buffer(url, RELEASES_MAX_BYTES, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch '%s' failed: %s", url, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "GitHub releases response for '%s' is not a JSON array", repo);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && out->count < NET_GITHUB_MAX_RELEASES; i++) {
        cJSON *rel = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(rel)) {
            continue;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rel, "draft"))) {
            continue; /* no stable assets to offer yet */
        }

        net_github_release_t *dst = &out->releases[out->count];
        copy_json_string(rel, "tag_name", dst->tag_name, sizeof(dst->tag_name));
        if (dst->tag_name[0] == '\0') {
            continue;
        }

        cJSON *assets = cJSON_GetObjectItemCaseSensitive(rel, "assets");
        if (cJSON_IsArray(assets)) {
            int asset_count = cJSON_GetArraySize(assets);
            for (int j = 0; j < asset_count && dst->asset_count < NET_GITHUB_MAX_ASSETS_PER_RELEASE; j++) {
                cJSON *a = cJSON_GetArrayItem(assets, j);
                if (!cJSON_IsObject(a)) {
                    continue;
                }
                net_github_asset_t *asset_dst = &dst->assets[dst->asset_count];
                copy_json_string(a, "name", asset_dst->name, sizeof(asset_dst->name));
                copy_json_string(a, "browser_download_url", asset_dst->download_url, sizeof(asset_dst->download_url));
                if (asset_dst->name[0] == '\0' || asset_dst->download_url[0] == '\0') {
                    continue;
                }
                cJSON *size_item = cJSON_GetObjectItemCaseSensitive(a, "size");
                asset_dst->size = cJSON_IsNumber(size_item) ? (uint32_t)size_item->valuedouble : 0;
                dst->asset_count++;
            }
        }
        out->count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "fetched %d release(s) for '%s'", (int)out->count, repo);
    return ESP_OK;
}

esp_err_t net_github_resolve_target_asset(const net_github_release_t *release, const net_github_asset_t **out_asset) {
    *out_asset = NULL;

    const net_github_asset_t *manifest_asset = NULL;
    for (size_t i = 0; i < release->asset_count; i++) {
        if (strcmp(release->assets[i].name, "launcher.manifest.json") == 0) {
            manifest_asset = &release->assets[i];
            break;
        }
    }
    if (manifest_asset == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *body = NULL;
    int body_len = 0;
    esp_err_t err = net_http_fetch_to_buffer(manifest_asset->download_url, RELEASE_MANIFEST_MAX_BYTES, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch launcher.manifest.json failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "launcher.manifest.json parse failed for release '%s'", release->tag_name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    cJSON *target_entry = cJSON_IsObject(targets) ? cJSON_GetObjectItemCaseSensitive(targets, CONFIG_IDF_TARGET) : NULL;
    if (cJSON_IsString(target_entry) && target_entry->valuestring != NULL) {
        for (size_t i = 0; i < release->asset_count; i++) {
            if (strcmp(release->assets[i].name, target_entry->valuestring) == 0) {
                *out_asset = &release->assets[i];
                result = ESP_OK;
                break;
            }
        }
        if (*out_asset == NULL) {
            ESP_LOGW(TAG, "launcher.manifest.json names asset '%s' for target '%s', not found among release '%s' assets",
                     target_entry->valuestring, CONFIG_IDF_TARGET, release->tag_name);
        }
    } else {
        ESP_LOGW(TAG, "launcher.manifest.json (release '%s') has no entry for target '%s'", release->tag_name,
                 CONFIG_IDF_TARGET);
    }

    cJSON_Delete(root);
    return result;
}
