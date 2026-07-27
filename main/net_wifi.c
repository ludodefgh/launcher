#include "net_wifi.h"

#include <string.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "net_wifi";

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_MAX_RETRY 5
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static bool s_stack_initialized;
static bool s_connected;
static int s_retry_num;
static char s_ip_str[16]; /* "255.255.255.255" + nul */

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retrying WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_str);
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size) {
    nvs_handle_t handle;
    bool found = false;
    if (nvs_open(CONFIG_LAUNCHER_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t s = ssid_size, p = pass_size;
        if (nvs_get_str(handle, NVS_KEY_SSID, ssid, &s) == ESP_OK && ssid[0] != '\0') {
            if (nvs_get_str(handle, NVS_KEY_PASS, pass, &p) != ESP_OK) {
                pass[0] = '\0';
            }
            found = true;
        }
        nvs_close(handle);
    }
    if (!found && CONFIG_LAUNCHER_NET_WIFI_SSID[0] != '\0') {
        strncpy(ssid, CONFIG_LAUNCHER_NET_WIFI_SSID, ssid_size - 1);
        ssid[ssid_size - 1] = '\0';
        strncpy(pass, CONFIG_LAUNCHER_NET_WIFI_PASSWORD, pass_size - 1);
        pass[pass_size - 1] = '\0';
        found = true;
        ESP_LOGI(TAG, "using CONFIG_LAUNCHER_NET_WIFI_SSID dev fallback (no NVS credentials yet)");
    }
    return found;
}

void net_wifi_save_credentials(const char *ssid, const char *password) {
    nvs_handle_t handle;
    if (nvs_open(CONFIG_LAUNCHER_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, password);
    nvs_commit(handle);
    nvs_close(handle);
}

bool net_wifi_is_connected(void) {
    return s_connected;
}

bool net_wifi_get_ip_string(char *buf, size_t buf_len) {
    if (!s_connected || s_ip_str[0] == '\0') {
        return false;
    }
    strncpy(buf, s_ip_str, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return true;
}

bool net_wifi_connect(void) {
    if (s_connected) {
        return true;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no WiFi credentials in NVS or Kconfig fallback -- network features unavailable this boot");
        return false;
    }

    if (!s_stack_initialized) {
        s_wifi_event_group = xEventGroupCreate();
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            return false;
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return false;
        }

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
        s_stack_initialized = true;
    }

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = pass[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to '%s'", ssid);
        return true;
    }

    ESP_LOGW(TAG, "WiFi connection to '%s' failed or timed out after %dms -- network features unavailable this boot",
             ssid, WIFI_CONNECT_TIMEOUT_MS);
    return false;
}
