#include "nvs_state.h"

#include <string.h>
#include <sys/time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_state";

#define NVS_NAMESPACE       CONFIG_LAUNCHER_NVS_NAMESPACE
#define KEY_LAST_APP        "last_app"
#define KEY_FORCE_MENU      "force_menu"
#define KEY_PROTO_VER       "proto_ver"
#define KEY_BOOT_STARTED_US "boot_start_us"
#define KEY_CRASH_STREAK    "crash_streak"

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

esp_err_t nvs_state_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupted/outdated (%s), erasing and re-initializing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t nvs_state_get_last_app(char *out_label, size_t out_label_size, bool *out_found) {
    *out_found = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* namespace never created yet -- first boot */
    } else if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, KEY_LAST_APP, out_label, &out_label_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    *out_found = true;
    return ESP_OK;
}

esp_err_t nvs_state_set_last_app(const char *label) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_LAST_APP, label);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_state_consume_force_menu(bool *out_force_menu) {
    *out_force_menu = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, KEY_FORCE_MENU, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    if (value != 0) {
        *out_force_menu = true;
        esp_err_t clear_err = nvs_set_u8(handle, KEY_FORCE_MENU, 0);
        if (clear_err == ESP_OK) {
            clear_err = nvs_commit(handle);
        }
        nvs_close(handle);
        return clear_err;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_state_get_protocol_version(uint32_t *out_version) {
    *out_version = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u32(handle, KEY_PROTO_VER, out_version);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_version = 0;
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_state_mark_boot_attempt_started(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i64(handle, KEY_BOOT_STARTED_US, now_us());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_state_get_boot_attempt_elapsed_us(int64_t *out_elapsed_us, bool *out_found) {
    *out_elapsed_us = 0;
    *out_found = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }

    int64_t started_us = 0;
    err = nvs_get_i64(handle, KEY_BOOT_STARTED_US, &started_us);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }

    *out_elapsed_us = now_us() - started_us;
    *out_found = true;
    return ESP_OK;
}

esp_err_t nvs_state_get_crash_streak(uint32_t *out_streak) {
    *out_streak = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u32(handle, KEY_CRASH_STREAK, out_streak);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_streak = 0;
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_state_set_crash_streak(uint32_t streak) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(handle, KEY_CRASH_STREAK, streak);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
