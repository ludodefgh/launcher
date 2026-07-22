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
 * already-initialized driver.
 *
 * Returns the index into kApps[] that was selected.
 */
int ui_menu_run(const nav_input_driver_t *drv);

#ifdef __cplusplus
}
#endif
