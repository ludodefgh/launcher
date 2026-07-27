#include "launcher_client.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "launcher_client";

esp_err_t launcher_request_menu_on_next_boot(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(LAUNCHER_CLIENT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, "force_menu", 1);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "proto_ver", LAUNCHER_CLIENT_PROTOCOL_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to write force_menu/proto_ver: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Writing force_menu alone is not enough: once otadata has been
     * written even once (i.e. as soon as any guest app has ever been
     * booted), the 2nd-stage bootloader boots straight from whatever
     * otadata points to and never falls back to checking "factory" on its
     * own -- the launcher's app_main() (and therefore the code that reads
     * force_menu) simply never runs again otherwise. Point the next boot
     * back at "factory" explicitly, the same way main/boot_into.c points
     * it at a guest slot when switching the other direction.
     */
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL) {
        ESP_LOGE(TAG, "factory partition not found -- can't return to launcher");
        return ESP_ERR_NOT_FOUND;
    }
    err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(factory) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "force_menu set, boot partition set to factory, restarting into launcher menu");
    esp_restart();
    return ESP_OK; /* unreachable */
}
