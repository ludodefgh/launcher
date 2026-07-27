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
    uint32_t crash_streak;                       /* consecutive fast-abnormal-reset count, already
                                                    * updated for this boot -- see
                                                    * boot_logic_next_crash_streak() below */
    uint32_t crash_loop_threshold;               /* CONFIG_LAUNCHER_CRASH_LOOP_THRESHOLD; 0 disables
                                                    * the crash-loop check entirely */
} boot_decision_input_t;

typedef enum {
    BOOT_ACTION_BOOT_DIRECT,
    BOOT_ACTION_SHOW_MENU,
} boot_action_t;

/* Implements: show the menu if force_menu, or the button is held, or there
 * is no remembered app yet, or the app has crash-looped past the configured
 * threshold (see issue #23); otherwise boot straight into the remembered app. */
boot_action_t boot_logic_decide(const boot_decision_input_t *input);

/* Crash-loop failsafe (issue #23): computes the next crash_streak value given
 * the streak persisted in NVS from before this boot. Only counts a "fast"
 * abnormal reset -- one where less than crash_loop_window_us elapsed between
 * the launcher last handing control to the app and this boot -- since the
 * whole point is catching an app that crashes before the user has any
 * chance to react (hold the button to force the menu); an abnormal reset
 * after the app ran fine for a while is deliberately not counted here, nor
 * is a normal reset. elapsed_us_since_boot_attempt should be negative or a
 * very large sentinel (e.g. INT64_MAX) when no prior boot attempt was
 * recorded (first boot ever) or the clock appears to have gone backwards --
 * both are treated as "not fast" rather than risking a false positive.
 * The streak is otherwise left unchanged here; it is only ever reset to 0
 * by the caller when the user deliberately reselects an app from the menu
 * (a deliberate design choice -- see README "Design decisions"). */
uint32_t boot_logic_next_crash_streak(uint32_t current_streak, bool last_boot_abnormal,
                                       int64_t elapsed_us_since_boot_attempt, int64_t crash_loop_window_us);

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
