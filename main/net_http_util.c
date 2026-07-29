#include "net_http_util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "net_http_util";

#define MAX_REDIRECTS 5

static bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t net_http_open_and_follow_redirects(esp_http_client_handle_t client, int *out_status) {
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    for (int i = 0; i < MAX_REDIRECTS && is_redirect_status(status); i++) {
        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) {
            return err;
        }
        esp_http_client_close(client);
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            return err;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }

    *out_status = status;
    return ESP_OK;
}

esp_err_t net_http_open(const char *url, esp_http_client_handle_t *out_client) {
    bool is_https = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
        /* See net_http_open_and_follow_redirects()'s doc comment -- GitHub
         * Releases redirect targets need more than the default 512 bytes. */
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
        esp_http_client_cleanup(client);
        return err;
    }
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    *out_client = client;
    return ESP_OK;
}

esp_err_t net_http_fetch_to_buffer(const char *url, size_t max_bytes, char **out_buf, int *out_len) {
    esp_http_client_handle_t client;
    esp_err_t err = net_http_open(url, &client);
    if (err != ESP_OK) {
        return err;
    }

    /* Catch an oversized response as early as possible when the server told
     * us its size upfront (issue #34) -- content_length is -1 if unknown
     * (e.g. chunked transfer encoding), in which case the post-read probe
     * below is the only way to detect it. */
    int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length > 0 && (size_t)content_length > max_bytes) {
        ESP_LOGE(TAG, "response for '%s' is %lld bytes, exceeds %u-byte buffer", url, (long long)content_length,
                 (unsigned)max_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(max_bytes + 1);
    if (buf == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < max_bytes) {
        int n = esp_http_client_read(client, buf + total, max_bytes - total);
        if (n < 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }

    if (total == max_bytes) {
        /* Buffer filled exactly -- ambiguous whether that's the whole body
         * or it got cut off (no Content-Length caught above, e.g. a chunked
         * response). One more small read disambiguates: anything still
         * pending means it was truncated. */
        char probe[1];
        int n = esp_http_client_read(client, probe, sizeof(probe));
        if (n > 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            ESP_LOGE(TAG, "response for '%s' exceeds %u-byte buffer (truncated)", url, (unsigned)max_bytes);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_buf = buf;
    *out_len = (int)total;
    return ESP_OK;
}
