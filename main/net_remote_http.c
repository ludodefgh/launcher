#include "net_remote_http.h"
#include "app_registry.h"
#include "boot_into.h"
#include "nvs_state.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "net_remote_http";
static httpd_handle_t s_server;

static bool pin_ok(httpd_req_t *req) {
    if (CONFIG_LAUNCHER_NET_REMOTE_PIN[0] == '\0') {
        return true; /* no PIN configured -- deterrent disabled, see Kconfig help */
    }
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char pin[64];
    if (httpd_query_key_value(query, "pin", pin, sizeof(pin)) != ESP_OK) {
        return false;
    }
    return strcmp(pin, CONFIG_LAUNCHER_NET_REMOTE_PIN) == 0;
}

static esp_err_t index_handler(httpd_req_t *req) {
    static char body[2048];
    int len = 0;
    len += snprintf(body + len, sizeof(body) - len, "<html><body><h1>launcher</h1><ul>");
    for (size_t i = 0; i < app_registry_count() && len < (int)sizeof(body) - 300; i++) {
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *display_name = app_registry_resolve_label(i, name_buf, sizeof(name_buf));
        len += snprintf(
            body + len, sizeof(body) - len,
            "<li><form action=\"/boot\" method=\"get\">"
            "<input type=\"hidden\" name=\"slot\" value=\"%s\">%s "
            "%s<button type=\"submit\">Boot</button></form></li>",
            app_registry_partition_label(i), display_name,
            CONFIG_LAUNCHER_NET_REMOTE_PIN[0] != '\0'
                ? "<input type=\"password\" name=\"pin\" placeholder=\"PIN\">"
                : "");
    }
    len += snprintf(body + len, sizeof(body) - len, "</ul></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

static esp_err_t boot_handler(httpd_req_t *req) {
    if (!pin_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid or missing PIN");
        return ESP_OK;
    }

    char query[128];
    char slot[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot, sizeof(slot)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot' parameter");
        return ESP_OK;
    }

    bool known_slot = false;
    for (size_t i = 0; i < app_registry_count(); i++) {
        if (strcmp(app_registry_partition_label(i), slot) == 0) {
            known_slot = true;
            break;
        }
    }
    if (!known_slot) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown slot");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "remote boot request for '%s'", slot);
    /* fresh_selection=true: a deliberate pick, same as a menu selection --
     * see boot_into.h, issue #27. */
    esp_err_t err = boot_into(slot, true);
    /* Only reached if boot_into() failed -- esp_restart() never returns on success,
     * so the client will just see the connection drop, which is expected. */
    ESP_LOGW(TAG, "remote boot into '%s' failed: %s", slot, esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "boot_into failed, slot may be empty/invalid");
    return ESP_OK;
}

void net_remote_http_start(void) {
    if (s_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return;
    }

    static const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    static const httpd_uri_t boot_uri = {.uri = "/boot", .method = HTTP_GET, .handler = boot_handler};
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &boot_uri);

    ESP_LOGI(TAG, "remote control HTTP server started on port %d", config.server_port);
}
