#pragma once

#include "nav_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Renders the slot selection menu (title + app_registry slot list, current
 * entry highlighted) and blocks until the user validates a choice with
 * NAV_EVENT_SELECT. Assumes display_init(), drv->init(), and
 * app_registry_init() have already been called by main.c; only registers
 * its own nav callback on top of the already-initialized driver. When
 * CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE, rows also show a "(UPD)" suffix
 * for slots net_version_check_has_update() reports as outdated.
 *
 * Returns the slot index (0..app_registry_count()-1) that was selected, OR,
 * when CONFIG_LAUNCHER_NET_OTA_ENABLE, app_registry_count() itself if the
 * extra "Download a program" entry was selected -- callers built with that
 * define must check for this sentinel before treating the result as a slot
 * index.
 */
int ui_menu_run(const nav_input_driver_t *drv);

#ifdef __cplusplus
}
#endif
