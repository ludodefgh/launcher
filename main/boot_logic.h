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
} boot_decision_input_t;

typedef enum {
    BOOT_ACTION_BOOT_DIRECT,
    BOOT_ACTION_SHOW_MENU,
} boot_action_t;

/* Implements: show the menu if force_menu, or the button is held, or there
 * is no remembered app yet; otherwise boot straight into the remembered app. */
boot_action_t boot_logic_decide(const boot_decision_input_t *input);

/* True if the number of ota_* partitions actually found on flash matches
 * CONFIG_LAUNCHER_APP_SLOT_COUNT (caller logs a warning on mismatch, does
 * not block boot -- see README). */
bool boot_logic_slot_count_matches(int found_ota_partitions, int configured_slot_count);

/* True if the first byte of a partition looks like a valid ESP-IDF app
 * image header (magic byte 0xE9), used to avoid booting into a slot that
 * was never flashed. */
bool boot_logic_is_valid_app_magic(uint8_t first_byte);

#ifdef __cplusplus
}
#endif
