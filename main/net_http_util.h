#pragma once

#include <stddef.h>

#include "esp_http_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * esp_http_client_open() + esp_http_client_fetch_headers(), following up to
 * 5 redirects (301/302/303/307/308) via esp_http_client_set_redirection().
 *
 * The low-level open/fetch_headers/read sequence used by net_manifest.c and
 * net_ota.c (streaming reads into a bounded buffer or straight into
 * esp_ota_write, neither of which fits esp_http_client_perform()'s
 * all-at-once model) does NOT auto-follow redirects the way
 * esp_http_client_perform() does internally -- that only happens inside
 * esp_http_client_perform()'s own loop. This matters in practice: GitHub
 * Releases download URLs (a convenient place to host a manifest.json or
 * .bin) always respond with a 302 to a signed googleusercontent-style URL.
 *
 * On ESP_OK return, *out_status is the final HTTP status and the client is
 * positioned to read the body via esp_http_client_read() as usual. On
 * error, the caller is responsible for esp_http_client_close()/cleanup(),
 * same as a failed esp_http_client_open() today.
 */
esp_err_t net_http_open_and_follow_redirects(esp_http_client_handle_t client, int *out_status);

/*
 * GETs `url` (following redirects per net_http_open_and_follow_redirects()
 * above) into a malloc'd, NUL-terminated buffer capped at max_bytes.
 * Attaches the ESP-IDF cert bundle for https:// URLs. On ESP_OK, *out_buf is
 * malloc'd (caller must free()) and *out_len is the number of bytes read
 * (excluding the added NUL). Shared by net_manifest.c and net_github.c --
 * both fetch a small JSON document and want the exact same
 * redirect-following/TLS/buffering behavior.
 */
esp_err_t net_http_fetch_to_buffer(const char *url, size_t max_bytes, char **out_buf, int *out_len);

#ifdef __cplusplus
}
#endif
