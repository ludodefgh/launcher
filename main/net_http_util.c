#include "net_http_util.h"

#include <stdbool.h>

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
