#include "net_remote_ble.h"
#include "app_registry.h"
#include "boot_into.h"
#include "nvs_state.h"

#include <stdlib.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
#include "net_wifi.h"
#endif
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
#include "net_ota.h"
#include "net_github.h"
#endif

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
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
/* Triggers net_ota_update_slot_from_manifest() for a slot (issue #33's
 * companion feature) -- not part of #33 itself, which is about pushing a
 * raw binary with no manifest involved; this is "go fetch and install
 * whatever the manifest currently says for this slot," headless, no local
 * UI. Only compiled in when OTA is (net_ota_update_slot_from_manifest()
 * lives in net_ota.c, itself gated the same way -- see CMakeLists.txt). */
static const ble_uuid128_t s_ota_update_chr_uuid =
    BLE_UUID128_INIT(0x4c, 0x41, 0x55, 0x4e, 0x43, 0x48, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04);
#endif
#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
/* Persists WiFi credentials (issue #32). Unlike the HTTP transport's
 * /wifi endpoint, this one *can* bootstrap a device with no working WiFi
 * yet -- BLE has no dependency on WiFi already being up (see
 * net_remote_ble_start() below), the HTTP server does. Only compiled in
 * when WiFi support itself is (no point exposing this otherwise). */
static const ble_uuid128_t s_wifi_chr_uuid =
    BLE_UUID128_INIT(0x4c, 0x41, 0x55, 0x4e, 0x43, 0x48, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x05);
#endif

static uint16_t s_slots_val_handle;
static uint16_t s_select_val_handle;
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
static uint16_t s_ota_update_val_handle;
#endif
#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
static uint16_t s_wifi_val_handle;
#endif
static char s_slots_text[512];
static uint8_t s_own_addr_type;
static bool s_started;

#define SELECT_BUF_LEN 64
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
/* label(31) + ':' + a generous pin + '@' + a version tag (NET_GITHUB_TAG_LEN-1)
 * comfortably exceeds SELECT_BUF_LEN, which only ever needs to hold
 * label[:pin] -- see handle_ota_update_write(). The app requests a 185-byte
 * MTU (182-byte ATT payload), well over this. */
#define OTA_BUF_LEN 128
#endif

/* Shared by every write characteristic here that takes "value:pin" --
 * splits on the first ':', trims `value` into `out_value` (capped at
 * out_value_size), and returns the pin substring (or NULL if there was no
 * ':' at all -- distinct from an empty pin after a trailing ':'). */
static const char *split_value_pin(const char *raw, char *out_value, size_t out_value_size) {
    const char *pin = NULL;
    const char *sep = strchr(raw, ':');
    if (sep != NULL) {
        size_t value_len = (size_t)(sep - raw);
        if (value_len >= out_value_size) {
            value_len = out_value_size - 1;
        }
        memcpy(out_value, raw, value_len);
        out_value[value_len] = '\0';
        pin = sep + 1;
    } else {
        strncpy(out_value, raw, out_value_size - 1);
        out_value[out_value_size - 1] = '\0';
    }
    return pin;
}

static bool pin_ok(const char *pin) {
    if (CONFIG_LAUNCHER_NET_REMOTE_PIN[0] == '\0') {
        return true;
    }
    return pin != NULL && strcmp(pin, CONFIG_LAUNCHER_NET_REMOTE_PIN) == 0;
}

static void handle_select_write(const char *value) {
    char label[32];
    const char *pin = split_value_pin(value, label, sizeof(label));

    if (!pin_ok(pin)) {
        ESP_LOGW(TAG, "BLE select write rejected: invalid/missing PIN");
        return;
    }

    bool known_slot = false;
    for (size_t i = 0; i < app_registry_count(); i++) {
        if (strcmp(app_registry_partition_label(i), label) == 0) {
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

#if CONFIG_LAUNCHER_NET_OTA_ENABLE
typedef struct {
    char label[32];
    /* Empty string = newest release, same as omitting it entirely -- see
     * net_ota_update_slot_from_manifest()'s version_tag doc comment. */
    char version[NET_GITHUB_TAG_LEN];
} ota_update_task_args_t;

static void ota_update_task(void *param) {
    ota_update_task_args_t *args = (ota_update_task_args_t *)param;
    const char *version_tag = args->version[0] != '\0' ? args->version : NULL;
    esp_err_t err = net_ota_update_slot_from_manifest(args->label, version_tag);
    ESP_LOGI(TAG, "remote (BLE) OTA update of '%s' (%s) finished: %s", args->label,
             version_tag != NULL ? version_tag : "latest", esp_err_to_name(err));
    free(args);
    vTaskDelete(NULL);
}

/* Payload: "<label>[:<pin>][@<version>]" -- '@' is checked for first (and
 * split off) so the existing "label[:pin]" shape parses exactly as before
 * when there's no '@'; the ':'-based label/pin split then runs on whatever
 * remains. A bare trailing '@' (empty version) is treated the same as no
 * '@' at all. Same pragmatic tradeoff as this file's other wire formats
 * (e.g. the WiFi payload's tab choice): a version tag containing a literal
 * '@' would misparse, but that's not a realistic tag name.
 *
 * Fire-and-forget by design: no progress/result reported back over BLE (no
 * new characteristic for it yet) -- a client re-reads the slots
 * characteristic afterward to see whether the version/flashed state
 * changed. Runs on its own task rather than inline here: WiFi connect +
 * manifest/GitHub fetches + the download itself can take several seconds,
 * far too long to block a GATT access callback (risks the central's ATT
 * operation timeout). */
static void handle_ota_update_write(const char *value) {
    char label_and_pin[OTA_BUF_LEN];
    const char *version = NULL;
    const char *at = strchr(value, '@');
    if (at != NULL) {
        size_t len = (size_t)(at - value);
        if (len >= sizeof(label_and_pin)) {
            len = sizeof(label_and_pin) - 1;
        }
        memcpy(label_and_pin, value, len);
        label_and_pin[len] = '\0';
        version = at + 1;
    } else {
        strncpy(label_and_pin, value, sizeof(label_and_pin) - 1);
        label_and_pin[sizeof(label_and_pin) - 1] = '\0';
    }

    char label[32];
    const char *pin = split_value_pin(label_and_pin, label, sizeof(label));

    if (!pin_ok(pin)) {
        ESP_LOGW(TAG, "BLE OTA-update write rejected: invalid/missing PIN");
        return;
    }

    bool known_slot = false;
    for (size_t i = 0; i < app_registry_count(); i++) {
        if (strcmp(app_registry_partition_label(i), label) == 0) {
            known_slot = true;
            break;
        }
    }
    if (!known_slot) {
        ESP_LOGW(TAG, "BLE OTA-update write rejected: unknown slot '%s'", label);
        return;
    }

    ota_update_task_args_t *args = calloc(1, sizeof(*args));
    if (args == NULL) {
        ESP_LOGE(TAG, "OTA-update task spawn failed: out of memory");
        return;
    }
    strncpy(args->label, label, sizeof(args->label) - 1);
    if (version != NULL) {
        strncpy(args->version, version, sizeof(args->version) - 1);
    }
    BaseType_t rc = xTaskCreate(ota_update_task, "launcher_ble_ota", CONFIG_LAUNCHER_NET_TASK_STACK_SIZE, args,
                                tskIDLE_PRIORITY + 5, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "OTA-update task spawn failed");
        free(args);
        return;
    }
    /* Log from the local label/version (still-valid stack data), not
     * args->... -- the just-spawned task owns *args now and may already have
     * freed it by the time this line runs. */
    ESP_LOGI(TAG, "remote (BLE) OTA update requested for '%s' (%s)", label,
             (version != NULL && version[0] != '\0') ? version : "latest");
}
#endif

#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
/* Payload: "ssid\tpass\tpin" (pass/pin may be empty strings, but the tabs
 * must be present -- see net_remote_ble.h-adjacent docs / README). Tab
 * rather than ':' since a WiFi password containing ':' is far more likely
 * than one containing a literal tab; still not a general escaping scheme,
 * same pragmatic tradeoff as the rest of this file's wire formats. */
static void handle_wifi_write(const char *value) {
    char ssid[33];
    char pass[65];
    char pin[64];

    const char *p = value;
    const char *sep1 = strchr(p, '\t');
    if (sep1 == NULL) {
        ESP_LOGW(TAG, "BLE WiFi write rejected: malformed payload (expected ssid\\tpass\\tpin)");
        return;
    }
    size_t ssid_len = (size_t)(sep1 - p);
    if (ssid_len >= sizeof(ssid)) {
        ssid_len = sizeof(ssid) - 1;
    }
    memcpy(ssid, p, ssid_len);
    ssid[ssid_len] = '\0';
    p = sep1 + 1;

    const char *sep2 = strchr(p, '\t');
    if (sep2 == NULL) {
        ESP_LOGW(TAG, "BLE WiFi write rejected: malformed payload (expected ssid\\tpass\\tpin)");
        return;
    }
    size_t pass_len = (size_t)(sep2 - p);
    if (pass_len >= sizeof(pass)) {
        pass_len = sizeof(pass) - 1;
    }
    memcpy(pass, p, pass_len);
    pass[pass_len] = '\0';
    p = sep2 + 1;

    strncpy(pin, p, sizeof(pin) - 1);
    pin[sizeof(pin) - 1] = '\0';

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "BLE WiFi write rejected: empty ssid");
        return;
    }
    if (!pin_ok(pin[0] != '\0' ? pin : NULL)) {
        ESP_LOGW(TAG, "BLE WiFi write rejected: invalid/missing PIN");
        return;
    }

    net_wifi_save_credentials(ssid, pass);
    ESP_LOGI(TAG, "remote (BLE) WiFi credentials updated for SSID '%s' (effective next reconnect/boot)", ssid);
}
#endif

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

#if CONFIG_LAUNCHER_NET_OTA_ENABLE
    if (attr_handle == s_ota_update_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[OTA_BUF_LEN];
        uint16_t len = 0;
        int rc = gatt_write_to_buf(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) {
            return rc;
        }
        buf[len] = '\0';
        handle_ota_update_write(buf);
        return 0;
    }
#endif

#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
    if (attr_handle == s_wifi_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* ssid(32)+'\t'+pass(64)+'\t'+pin(63)+NUL -- generous over SELECT_BUF_LEN. */
        char buf[164];
        uint16_t len = 0;
        int rc = gatt_write_to_buf(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) {
            return rc;
        }
        buf[len] = '\0';
        handle_wifi_write(buf);
        return 0;
    }
#endif

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
#if CONFIG_LAUNCHER_NET_OTA_ENABLE
                {
                    .uuid = &s_ota_update_chr_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &s_ota_update_val_handle,
                },
#endif
#if CONFIG_LAUNCHER_NET_WIFI_ENABLE
                {
                    .uuid = &s_wifi_chr_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &s_wifi_val_handle,
                },
#endif
                {0},
            },
    },
    {0},
};

static void build_slots_text(void) {
    /* One record per slot, "label\tname\tversion\tflashed\n":
     *   - label:   app_registry_partition_label(i) -- the boot key the select
     *              characteristic still expects (unchanged), so booting is by label.
     *   - name:    app_registry_resolve_label(i) -- the friendly display name.
     *   - version: app_registry_get_version(i), empty if the slot isn't flashed.
     *   - flashed: '1' if app_registry_slot_is_flashed(i), else '0'.
     * Mirrors what the TFT menu (ui_menu.c) and the HTTP remote (net_remote_http.c)
     * already surface, so a BLE client shows the same rows -- see issue #30. Built
     * once at start; the app's "Refresh" re-reads this same cached string. */
    int len = 0;
    s_slots_text[0] = '\0';
    /* Reserve one full record's worth of headroom so a slot is never half-written. */
    const int reserve = (int)(NVS_STATE_SLOT_NAME_LEN + APP_REGISTRY_VERSION_LEN + 32);
    for (size_t i = 0; i < app_registry_count() && len < (int)sizeof(s_slots_text) - reserve; i++) {
        char name_buf[NVS_STATE_SLOT_NAME_LEN];
        const char *name = app_registry_resolve_label(i, name_buf, sizeof(name_buf));
        char version[APP_REGISTRY_VERSION_LEN];
        if (!app_registry_get_version(i, version, sizeof(version))) {
            version[0] = '\0';
        }
        len += snprintf(s_slots_text + len, sizeof(s_slots_text) - len, "%s\t%s\t%s\t%d\n",
                        app_registry_partition_label(i), name, version,
                        app_registry_slot_is_flashed(i) ? 1 : 0);
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
