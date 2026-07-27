#include "nvs_state.h"

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_state";

#define NVS_NAMESPACE   CONFIG_LAUNCHER_NVS_NAMESPACE
#define KEY_LAST_APP    "last_app"
#define KEY_FORCE_MENU  "force_menu"
#define KEY_PROTO_VER   "proto_ver"

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

esp_err_t nvs_state_clear_last_app(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* nothing to clear */
    } else if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, KEY_LAST_APP);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* already cleared / never set */
    }
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

static void slot_name_key(size_t slot_index, char *out_key, size_t out_key_size) {
    snprintf(out_key, out_key_size, "slot_name%u", (unsigned)slot_index);
}

esp_err_t nvs_state_get_slot_name(size_t slot_index, char *out_name, size_t out_name_size, bool *out_found) {
    *out_found = false;
    if (slot_index >= NVS_STATE_MAX_APP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }

    char key[16];
    slot_name_key(slot_index, key, sizeof(key));
    err = nvs_get_str(handle, key, out_name, &out_name_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    *out_found = true;
    return ESP_OK;
}

esp_err_t nvs_state_set_slot_name(size_t slot_index, const char *name) {
    if (slot_index >= NVS_STATE_MAX_APP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    slot_name_key(slot_index, key, sizeof(key));
    err = nvs_set_str(handle, key, name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void slot_version_key(size_t slot_index, char *out_key, size_t out_key_size) {
    snprintf(out_key, out_key_size, "slot_ver%u", (unsigned)slot_index);
}

esp_err_t nvs_state_get_slot_version(size_t slot_index, char *out_version, size_t out_version_size, bool *out_found) {
    *out_found = false;
    if (slot_index >= NVS_STATE_MAX_APP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }

    char key[16];
    slot_version_key(slot_index, key, sizeof(key));
    err = nvs_get_str(handle, key, out_version, &out_version_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    *out_found = true;
    return ESP_OK;
}

esp_err_t nvs_state_set_slot_version(size_t slot_index, const char *version) {
    if (slot_index >= NVS_STATE_MAX_APP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    slot_version_key(slot_index, key, sizeof(key));
    err = nvs_set_str(handle, key, version);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
