#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tiny contract library for guest apps ("ASCII Aquarium" etc.) that want to
 * hand control back to the launcher. Include this component, call
 * launcher_request_menu_on_next_boot() on whatever action your app defines
 * (e.g. long-press 3s on its own EC11) -- it never returns, it reboots
 * straight into the launcher's menu.
 *
 * NVS namespace/keys are duplicated (not shared via a common header)
 * because a guest app only depends on this one small component, not on the
 * launcher's main/ sources. IMPORTANT: if a project changes
 * CONFIG_LAUNCHER_NVS_NAMESPACE away from its default "launcher" on the
 * launcher side, LAUNCHER_CLIENT_NVS_NAMESPACE below must be updated to
 * match, or the force_menu flag silently goes unnoticed. See README.md.
 */

#define LAUNCHER_CLIENT_NVS_NAMESPACE "launcher"

/* Bump when the NVS contract (keys/format) changes in a way the launcher
 * needs to know about. The launcher logs a warning (never crashes) on a
 * version it doesn't recognize -- see main.c's
 * check_client_protocol_version(). */
#define LAUNCHER_CLIENT_PROTOCOL_VERSION 1

/*
 * Writes force_menu=true and the protocol version to NVS, then calls
 * esp_restart(). Does not return on success. Returns an esp_err_t only if
 * the NVS write itself failed (so the caller can decide what to do instead
 * of silently never returning to the launcher).
 */
esp_err_t launcher_request_menu_on_next_boot(void);

#ifdef __cplusplus
}
#endif
