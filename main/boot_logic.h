#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure decision logic for the launcher's boot sequence (spec: "Logique de
 * boot"). Deliberately free of any ESP-IDF/hardware dependency (no NVS, no
 * esp_partition, no FreeRTOS) so it can be unit-tested with a plain host
 * compiler -- see test/test_boot_logic.c. Hardware-dependent wiring
 * (reading NVS, calling esp_ota_set_boot_partition/esp_restart) lives in
 * boot_into.c, which calls into this module rather than the other way
 * around.
 */

#define BOOT_LOGIC_MAX_LABEL_LEN 32

typedef struct {
    bool has_last_app;                          /* "last_app_partition" was found in NVS */
    char last_app_partition[BOOT_LOGIC_MAX_LABEL_LEN];
    bool force_menu;                             /* consumed force_menu flag from NVS */
    bool button_held;                            /* encoder button held at boot */
    bool last_app_flagged_unhealthy;             /* esp_ota_get_state_partition() found
                                                    * last_app_partition marked ABORTED/INVALID by
                                                    * ESP-IDF's app-rollback mechanism -- see issue
                                                    * #27 follow-up */
} boot_decision_input_t;

typedef enum {
    BOOT_ACTION_BOOT_DIRECT,
    BOOT_ACTION_SHOW_MENU,
} boot_action_t;

/* Implements: show the menu if force_menu, the button is held, there is no
 * remembered app, or the remembered app was flagged unhealthy by rollback;
 * otherwise boot straight into the remembered app.
 *
 * NOTE on crash-loop recovery (issue #23): an NVS-based crash-streak counter
 * was tried here and reverted -- confirmed on real hardware to be dead code,
 * since esp_ota_set_boot_partition() redirects the bootloader itself
 * permanently, so app_main() (and this function) never runs again once a
 * guest app has been direct-booted, crash or not. Recovery is now handled at
 * the bootloader level instead via ESP-IDF's own app-rollback mechanism
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, selected by
 * CONFIG_LAUNCHER_CRASH_LOOP_RECOVERY_ENABLE) -- see boot_into.c and
 * README.md "Design decisions". This module's only role in that is the
 * last_app_flagged_unhealthy check above: once the bootloader's own rollback
 * logic has flagged the remembered app, this launcher must not blindly
 * retry it via ordinary "has_last_app" direct-boot logic, or it would just
 * re-arm a fresh rollback cycle and the user would never actually see the
 * menu (issue #27 follow-up). */
boot_action_t boot_logic_decide(const boot_decision_input_t *input);

/* True if the first byte of a partition looks like a valid ESP-IDF app
 * image header (magic byte 0xE9), used to avoid booting into a slot that
 * was never flashed. */
bool boot_logic_is_valid_app_magic(uint8_t first_byte);

#ifdef __cplusplus
}
#endif
