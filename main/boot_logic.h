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
    bool otadata_matches_last_app;               /* see boot_logic_decide() doc below */
} boot_decision_input_t;

typedef enum {
    BOOT_ACTION_BOOT_DIRECT,
    BOOT_ACTION_SHOW_MENU,
} boot_action_t;

/* Implements: show the menu if force_menu, or the button is held, or there
 * is no remembered app yet, or otadata doesn't actually agree with the
 * remembered app; otherwise boot straight into the remembered app.
 *
 * otadata_matches_last_app (issue #23, crash-loop recovery, attempt 3) is
 * what makes automatic recovery from a fast-crashing guest app possible.
 * Two earlier attempts were tried here and both fully reverted, confirmed
 * on real hardware:
 *   1. An NVS-based crash-streak counter checked in app_main() -- dead
 *      code, since esp_ota_set_boot_partition() redirects the bootloader
 *      itself permanently, so app_main() never runs again once a guest app
 *      has been direct-booted, crash or not.
 *   2. ESP-IDF's own bootloader-level app rollback
 *      (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) -- runs at the right layer
 *      (before any application code), but per ESP-IDF's own documentation
 *      ("Only OTA partitions can be rolled back. Factory partition is not
 *      rolled back.") it structurally cannot ever fall back to this
 *      launcher's factory partition, confirmed by repeated real-hardware
 *      testing across issues #23/#25/#27 never recovering.
 * Attempt 3 sidesteps both failure modes by using a *different*, unrelated
 * ESP-IDF bootloader feature -- CONFIG_BOOTLOADER_FACTORY_RESET (see
 * sdkconfig.defaults) polls a GPIO directly in the 2nd-stage bootloader,
 * before otadata-based partition selection is ever trusted, so it doesn't
 * depend on app_main() getting a chance to run (unlike button_held above,
 * which only gets read once we're already back in the launcher). Holding
 * the button long enough erases otadata (CONFIG_BOOTLOADER_OTA_DATA_ERASE)
 * and the bootloader's own pre-existing "otadata invalid -> boot factory"
 * fallback lands back on this launcher. That alone isn't sufficient though:
 * NVS's "last_app" is untouched by an otadata erase, so app_main() would
 * otherwise immediately read the same crashing slot back out and re-arm a
 * fresh direct-boot into it, undoing the recovery within the same boot --
 * the same class of bug that defeated attempt 2 (issue #27). The caller
 * computes otadata_matches_last_app by comparing esp_ota_get_boot_partition()
 * (a fresh read of otadata, standard app_update API) against last_app_partition;
 * a mismatch (which is what an otadata erase produces, since it resolves to
 * "factory" instead) means don't trust last_app for this boot. See
 * README.md "Design decisions" for the full history. */
boot_action_t boot_logic_decide(const boot_decision_input_t *input);

/* True if the first byte of a partition looks like a valid ESP-IDF app
 * image header (magic byte 0xE9), used to avoid booting into a slot that
 * was never flashed. */
bool boot_logic_is_valid_app_magic(uint8_t first_byte);

#ifdef __cplusplus
}
#endif
