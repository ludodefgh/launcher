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
 * net_remote_ble.c) rather than the local menu.
 *
 * version_tag: NULL or an empty string picks the newest release, same as
 * before this parameter existed. A non-empty value is looked up directly via
 * net_github_fetch_release_by_tag() instead of net_github_fetch_tags()'s
 * newest-first list -- this is what lets a remote caller roll back to (or
 * pin) a specific version without the local menu's interactive "CHOOSE
 * VERSION" step. Ignored for a manifest entry with a plain url/version (no
 * github_repo) -- there's only ever one version to fetch there. Returns
 * ESP_ERR_NOT_FOUND if version_tag doesn't match any release for that repo.
 *
 * Requires WiFi already connected or connectable (calls net_wifi_connect()
 * itself). Blocking -- WiFi connect + manifest/GitHub fetches + the
 * download itself can take several seconds; callers must run this on a
 * dedicated task, never inline in a time-sensitive callback.
 */
esp_err_t net_ota_update_slot_from_manifest(const char *slot_label, const char *version_tag);

#ifdef __cplusplus
}
#endif
