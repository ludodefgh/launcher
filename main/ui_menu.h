#pragma once

#include "nav_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Renders the slot selection menu (title + kApps list, current entry
 * highlighted) and blocks until the user validates a choice with
 * NAV_EVENT_SELECT. Assumes display_init() and drv->init() have already
 * been called by main.c; only registers its own nav callback on top of the
 * already-initialized driver. When CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE,
 * rows also show a "(MAJ)" suffix for slots net_version_check_has_update()
 * reports as outdated.
 *
 * Returns the index into kApps[] that was selected, OR, when
 * CONFIG_LAUNCHER_NET_OTA_ENABLE, kAppsCount itself if the extra
 * "Telecharger un programme" entry was selected -- callers built with that
 * define must check for this sentinel before indexing kApps[].
 */
int ui_menu_run(const nav_input_driver_t *drv);

#ifdef __cplusplus
}
#endif
