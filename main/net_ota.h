#pragma once

#include "esp_err.h"
#include "nav_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Self-contained "download a program" flow, entered from the main menu's
 * extra entry (see ui_menu.c) when CONFIG_LAUNCHER_NET_OTA_ENABLE: connect
 * WiFi -> fetch manifest -> pick an app -> pick a destination slot ->
 * confirm -> stream the .bin straight into that partition via manual
 * esp_ota_begin/write/end (not the high-level esp_https_ota wrapper, which
 * always targets "the next OTA slot" rather than a specific one -- this
 * project needs an explicitly chosen slot). Returns to the caller (the
 * main menu loop) when done, whether it succeeded, failed, or was
 * cancelled -- never auto-boots the freshly written slot, the user picks
 * it from the normal menu afterward like any other slot.
 */
void net_ota_run_download_flow(const nav_input_driver_t *drv);

/*
 * Non-interactive counterpart to net_ota_run_download_flow() above -- looks
 * up the manifest entry whose "slot" field matches slot_label and downloads
 * it into that slot with no local UI at all (no pickers, no confirm
 * prompt, no on-screen progress), meant for a remote trigger (see
 * net_remote_ble.c) rather than the local menu. For a github_repo entry,
 * always uses the newest release -- there's no remote version-picker round
 * trip yet, so installing an older version still needs the local menu.
 * Requires WiFi already connected or connectable (calls net_wifi_connect()
 * itself). Blocking -- WiFi connect + manifest/GitHub fetches + the
 * download itself can take several seconds; callers must run this on a
 * dedicated task, never inline in a time-sensitive callback.
 */
esp_err_t net_ota_update_slot_from_manifest(const char *slot_label);

#ifdef __cplusplus
}
#endif
