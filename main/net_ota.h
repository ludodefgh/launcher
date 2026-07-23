#pragma once

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

#ifdef __cplusplus
}
#endif
