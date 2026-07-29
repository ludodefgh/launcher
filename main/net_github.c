#include "net_github.h"
#include "net_http_util.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "net_github";

/* Tags are name + commit sha/url only, tens of bytes each -- generous
 * headroom over NET_GITHUB_MAX_TAGS worth even so. */
#define TAGS_MAX_BYTES 4096
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

esp_err_t net_github_fetch_tags(const char *repo, net_github_tag_list_t *out) {
    memset(out, 0, sizeof(*out));

    char url[192];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/tags?per_page=%d", repo, NET_GITHUB_MAX_TAGS);

    char *body = NULL;
    int body_len = 0;
    esp_err_t err = net_http_fetch_to_buffer(url, TAGS_MAX_BYTES, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch '%s' failed: %s", url, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "GitHub tags response for '%s' is not a JSON array", repo);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && out->count < NET_GITHUB_MAX_TAGS; i++) {
        cJSON *tag = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(tag)) {
            continue;
        }
        char name[NET_GITHUB_TAG_LEN];
        copy_json_string(tag, "name", name, sizeof(name));
        if (name[0] == '\0') {
            continue;
        }
        strncpy(out->names[out->count], name, NET_GITHUB_TAG_LEN - 1);
        out->names[out->count][NET_GITHUB_TAG_LEN - 1] = '\0';
        out->count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "fetched %d tag(s) for '%s'", (int)out->count, repo);
    return ESP_OK;
}

/*
 * Streaming JSON scanner for the single-release response (issue #34, round
 * 2): even one 32KB buffered malloc for a release's full JSON was observed
 * failing outright on real hardware under heap pressure (WiFi + BLE +
 * display all resident at once) -- not a parse/network error, the
 * allocation itself. This reads the HTTP body through a small fixed chunk
 * buffer and extracts only "tag_name" and each asset's "name" /
 * "browser_download_url" / "size" as they're encountered, so the whole
 * serialized response is never held in memory at once, on any board
 * (PSRAM or not). Not a general-purpose JSON parser -- just enough of one
 * (generic recursive skip for anything not one of those fields) to walk
 * GitHub's actual release-object shape correctly regardless of exact field
 * order/whitespace.
 */

typedef struct {
    esp_http_client_handle_t client;
    char chunk[512];
    int chunk_len;
    int chunk_pos;
    int la; /* lookahead byte, -1 at EOF/error */
} json_reader_t;

static int reader_raw_next(json_reader_t *r) {
    if (r->chunk_pos >= r->chunk_len) {
        int n = esp_http_client_read(r->client, r->chunk, sizeof(r->chunk));
        if (n <= 0) {
            return -1;
        }
        r->chunk_len = n;
        r->chunk_pos = 0;
    }
    return (unsigned char)r->chunk[r->chunk_pos++];
}

static void reader_init(json_reader_t *r, esp_http_client_handle_t client) {
    r->client = client;
    r->chunk_len = 0;
    r->chunk_pos = 0;
    r->la = reader_raw_next(r);
}

static void reader_advance(json_reader_t *r) {
    r->la = reader_raw_next(r);
}

static void skip_ws(json_reader_t *r) {
    while (r->la == ' ' || r->la == '\t' || r->la == '\n' || r->la == '\r') {
        reader_advance(r);
    }
}

/* Reads a JSON string (reader must be positioned at the opening quote).
 * Writes up to out_size-1 decoded bytes into out (NUL-terminated); pass
 * out=NULL to just consume/discard the string. \uXXXX is consumed
 * correctly but not decoded (none of tag names / asset filenames / URLs
 * realistically contain non-ASCII escapes). */
static bool parse_string(json_reader_t *r, char *out, size_t out_size) {
    if (r->la != '"') {
        return false;
    }
    reader_advance(r);
    size_t len = 0;
    while (r->la != '"') {
        if (r->la < 0) {
            return false; /* EOF mid-string */
        }
        int c = r->la;
        if (c == '\\') {
            reader_advance(r);
            switch (r->la) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u':
                    reader_advance(r);
                    for (int i = 0; i < 4 && r->la >= 0; i++) {
                        reader_advance(r);
                    }
                    continue; /* already advanced past the whole escape */
                default:
                    return false;
            }
        }
        if (out != NULL && len + 1 < out_size) {
            out[len++] = (char)c;
        }
        reader_advance(r);
    }
    reader_advance(r); /* closing quote */
    if (out != NULL) {
        out[len] = '\0';
    }
    return true;
}

static bool parse_number(json_reader_t *r, uint32_t *out) {
    char buf[16];
    size_t len = 0;
    while (r->la >= 0 && (isdigit(r->la) || r->la == '-' || r->la == '+' || r->la == '.' || r->la == 'e' ||
                          r->la == 'E')) {
        if (len + 1 < sizeof(buf)) {
            buf[len++] = (char)r->la;
        }
        reader_advance(r);
    }
    buf[len] = '\0';
    if (len == 0) {
        return false;
    }
    *out = (uint32_t)strtoul(buf, NULL, 10);
    return true;
}

/* Generically consumes any JSON value (string/number/bool/null/object/
 * array) without capturing it -- used for every field this parser doesn't
 * specifically care about (uploader, timestamps, content_type, ...). */
static bool skip_value(json_reader_t *r) {
    skip_ws(r);
    if (r->la == '"') {
        return parse_string(r, NULL, 0);
    }
    if (r->la == '{') {
        reader_advance(r);
        skip_ws(r);
        if (r->la == '}') {
            reader_advance(r);
            return true;
        }
        while (true) {
            skip_ws(r);
            if (!parse_string(r, NULL, 0)) {
                return false;
            }
            skip_ws(r);
            if (r->la != ':') {
                return false;
            }
            reader_advance(r);
            if (!skip_value(r)) {
                return false;
            }
            skip_ws(r);
            if (r->la == ',') {
                reader_advance(r);
                continue;
            }
            if (r->la == '}') {
                reader_advance(r);
                return true;
            }
            return false;
        }
    }
    if (r->la == '[') {
        reader_advance(r);
        skip_ws(r);
        if (r->la == ']') {
            reader_advance(r);
            return true;
        }
        while (true) {
            if (!skip_value(r)) {
                return false;
            }
            skip_ws(r);
            if (r->la == ',') {
                reader_advance(r);
                continue;
            }
            if (r->la == ']') {
                reader_advance(r);
                return true;
            }
            return false;
        }
    }
    /* number / true / false / null -- a bare run up to the next structural
     * character or whitespace. */
    if (r->la < 0) {
        return false;
    }
    while (r->la >= 0 && r->la != ',' && r->la != '}' && r->la != ']' && r->la != ' ' && r->la != '\t' &&
           r->la != '\n' && r->la != '\r') {
        reader_advance(r);
    }
    return true;
}

/* Parses one asset object's fields (reader positioned right after its
 * opening '{'). */
static bool parse_asset_fields(json_reader_t *r, net_github_asset_t *out) {
    memset(out, 0, sizeof(*out));
    skip_ws(r);
    if (r->la == '}') {
        reader_advance(r);
        return true;
    }
    while (true) {
        skip_ws(r);
        char key[32];
        if (!parse_string(r, key, sizeof(key))) {
            return false;
        }
        skip_ws(r);
        if (r->la != ':') {
            return false;
        }
        reader_advance(r);
        skip_ws(r);

        if (strcmp(key, "name") == 0) {
            if (!parse_string(r, out->name, sizeof(out->name))) {
                return false;
            }
        } else if (strcmp(key, "browser_download_url") == 0) {
            if (!parse_string(r, out->download_url, sizeof(out->download_url))) {
                return false;
            }
        } else if (strcmp(key, "size") == 0) {
            if (!parse_number(r, &out->size)) {
                return false;
            }
        } else if (!skip_value(r)) {
            return false;
        }

        skip_ws(r);
        if (r->la == ',') {
            reader_advance(r);
            continue;
        }
        if (r->la == '}') {
            reader_advance(r);
            return true;
        }
        return false;
    }
}

static bool parse_assets_array(json_reader_t *r, net_github_release_t *dst) {
    skip_ws(r);
    if (r->la != '[') {
        return false;
    }
    reader_advance(r);
    skip_ws(r);
    if (r->la == ']') {
        reader_advance(r);
        return true;
    }
    while (true) {
        skip_ws(r);
        if (r->la != '{') {
            return false;
        }
        reader_advance(r);

        net_github_asset_t asset;
        if (!parse_asset_fields(r, &asset)) {
            return false;
        }
        if (asset.name[0] != '\0' && asset.download_url[0] != '\0' &&
            dst->asset_count < NET_GITHUB_MAX_ASSETS_PER_RELEASE) {
            dst->assets[dst->asset_count++] = asset;
        }

        skip_ws(r);
        if (r->la == ',') {
            reader_advance(r);
            continue;
        }
        if (r->la == ']') {
            reader_advance(r);
            return true;
        }
        return false;
    }
}

/* Top-level release object (reader positioned at the very start of the
 * response body). */
static bool parse_release_stream(json_reader_t *r, net_github_release_t *out) {
    memset(out, 0, sizeof(*out));
    skip_ws(r);
    if (r->la != '{') {
        return false;
    }
    reader_advance(r);
    skip_ws(r);
    if (r->la == '}') {
        reader_advance(r);
        return true; /* empty object -- caller treats an empty tag_name as not-found */
    }
    while (true) {
        skip_ws(r);
        char key[32];
        if (!parse_string(r, key, sizeof(key))) {
            return false;
        }
        skip_ws(r);
        if (r->la != ':') {
            return false;
        }
        reader_advance(r);
        skip_ws(r);

        if (strcmp(key, "tag_name") == 0) {
            if (!parse_string(r, out->tag_name, sizeof(out->tag_name))) {
                return false;
            }
        } else if (strcmp(key, "assets") == 0) {
            if (!parse_assets_array(r, out)) {
                return false;
            }
        } else if (!skip_value(r)) {
            return false;
        }

        skip_ws(r);
        if (r->la == ',') {
            reader_advance(r);
            continue;
        }
        if (r->la == '}') {
            reader_advance(r);
            return true;
        }
        return false;
    }
}

esp_err_t net_github_fetch_release_by_tag(const char *repo, const char *tag, net_github_release_t *out) {
    memset(out, 0, sizeof(*out));

    char url[224];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/tags/%s", repo, tag);

    esp_http_client_handle_t client;
    esp_err_t err = net_http_open(url, &client);
    if (err != ESP_OK) {
        /* Covers "no release for this tag" too -- GitHub answers that with
         * a plain HTTP 404, which net_http_open() already turns into
         * ESP_FAIL via its status-code check. */
        ESP_LOGE(TAG, "open '%s' failed: %s", url, esp_err_to_name(err));
        return err;
    }

    json_reader_t r;
    reader_init(&r, client);
    bool parsed_ok = parse_release_stream(&r, out);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!parsed_ok) {
        ESP_LOGE(TAG, "streaming parse of release '%s'/'%s' failed", repo, tag);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out->tag_name[0] == '\0') {
        ESP_LOGE(TAG, "release response for '%s'/'%s' has no tag_name", repo, tag);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "fetched release '%s' for '%s': %d asset(s)", out->tag_name, repo, (int)out->asset_count);
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
