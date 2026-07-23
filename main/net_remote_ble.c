#include "net_remote_ble.h"
#include "app_registry.h"
#include "boot_into.h"

#include <string.h>
#include <stdio.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "net_remote_ble";

/* Provided by the nimble host's store/config sources (already part of the
 * "bt" component's own build, not declared in a public header -- the
 * official bleprph example does the same inline forward declaration). */
void ble_store_config_init(void);

/* Arbitrary project-local 128-bit UUIDs (not registered with the Bluetooth SIG --
 * fine for a custom, non-published service on a trusted home network). */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x4c, 0x41, 0x55, 0x4e, 0x43, 0x48, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01);
static const ble_uuid128_t s_slots_chr_uuid =
    BLE_UUID128_INIT(0x4c, 0x41, 0x55, 0x4e, 0x43, 0x48, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02);
static const ble_uuid128_t s_select_chr_uuid =
    BLE_UUID128_INIT(0x4c, 0x41, 0x55, 0x4e, 0x43, 0x48, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03);

static uint16_t s_slots_val_handle;
static uint16_t s_select_val_handle;
static char s_slots_text[256];
static uint8_t s_own_addr_type;
static bool s_started;

#define SELECT_BUF_LEN 64

static void handle_select_write(const char *value) {
    char label[32];
    const char *pin = NULL;
    const char *sep = strchr(value, ':');
    if (sep != NULL) {
        size_t label_len = (size_t)(sep - value);
        if (label_len >= sizeof(label)) {
            label_len = sizeof(label) - 1;
        }
        memcpy(label, value, label_len);
        label[label_len] = '\0';
        pin = sep + 1;
    } else {
        strncpy(label, value, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
    }

    if (CONFIG_LAUNCHER_NET_REMOTE_PIN[0] != '\0') {
        if (pin == NULL || strcmp(pin, CONFIG_LAUNCHER_NET_REMOTE_PIN) != 0) {
            ESP_LOGW(TAG, "BLE select write rejected: invalid/missing PIN");
            return;
        }
    }

    bool known_slot = false;
    for (size_t i = 0; i < kAppsCount; i++) {
        if (strcmp(kApps[i].partition_label, label) == 0) {
            known_slot = true;
            break;
        }
    }
    if (!known_slot) {
        ESP_LOGW(TAG, "BLE select write rejected: unknown slot '%s'", label);
        return;
    }

    ESP_LOGI(TAG, "remote (BLE) boot request for '%s'", label);
    esp_err_t err = boot_into(label);
    /* Only reached if boot_into() failed -- esp_restart() never returns on success. */
    ESP_LOGW(TAG, "remote (BLE) boot into '%s' failed: %s", label, esp_err_to_name(err));
}

static int gatt_write_to_buf(struct os_mbuf *om, char *dst, uint16_t max_len, uint16_t *out_len) {
    uint16_t om_len = OS_MBUF_PKTLEN(om);
    if (om_len > max_len) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, dst, max_len, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return 0;
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;

    if (attr_handle == s_slots_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, s_slots_text, strlen(s_slots_text));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle == s_select_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[SELECT_BUF_LEN];
        uint16_t len = 0;
        int rc = gatt_write_to_buf(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) {
            return rc;
        }
        buf[len] = '\0';
        handle_select_write(buf);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_slots_chr_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_READ,
                    .val_handle = &s_slots_val_handle,
                },
                {
                    .uuid = &s_select_chr_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &s_select_val_handle,
                },
                {0},
            },
    },
    {0},
};

static void build_slots_text(void) {
    int len = 0;
    for (size_t i = 0; i < kAppsCount && len < (int)sizeof(s_slots_text) - 32; i++) {
        len += snprintf(s_slots_text + len, sizeof(s_slots_text) - len, "%s%s", i == 0 ? "" : ",", kApps[i].partition_label);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
#endif

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed; rc=%d", rc);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "BLE connection %s", event->connect.status == 0 ? "established" : "failed");
            if (event->connect.status != 0) {
                start_advertising();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected, resuming advertising");
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;
        default:
            return 0;
    }
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed; rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
        return;
    }
    start_advertising();
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void net_remote_ble_start(void) {
    if (s_started) {
        return;
    }

    build_slots_text();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
#endif
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed; rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed; rc=%d", rc);
        return;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_device_name_set("launcher");
#endif

    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE remote control started, advertising");
}
