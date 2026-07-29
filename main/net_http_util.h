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
 * Inits an esp_http_client for `url` (TLS cert bundle attached automatically
 * for https://), opens it via net_http_open_and_follow_redirects() above,
 * and confirms the final status is 200. On ESP_OK, *out_client is open and
 * positioned to read the body via esp_http_client_read() -- caller owns
 * esp_http_client_close()/cleanup() once done reading. On error, the client
 * has already been torn down. Shared connection-setup used both by
 * net_http_fetch_to_buffer() below (buffers the whole body) and by callers
 * that stream the body incrementally instead (net_github.c's release fetch,
 * issue #34 round 2 -- buffering a whole GitHub release's JSON can fail to
 * even malloc under real-world heap pressure, regardless of buffer size).
 */
esp_err_t net_http_open(const char *url, esp_http_client_handle_t *out_client);

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
