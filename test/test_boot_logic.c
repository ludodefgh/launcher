/*
 * Host-based unit tests for the pure logic in main/boot_logic.c. No
 * ESP-IDF/FreeRTOS dependency on purpose (boot_logic.c doesn't have one
 * either) -- builds and runs with a plain host compiler, see README.md:
 *
 *   gcc -std=c11 -I main main/boot_logic.c test/test_boot_logic.c -o /tmp/test_boot_logic && /tmp/test_boot_logic
 *
 * Deliberately not using the ESP-IDF Unity/`--target linux` machinery: that
 * preview feature pulls in more of the IDF build system than this small,
 * genuinely hardware-free module needs. Scope is limited to boot_logic.c's
 * pure decision functions -- no attempt to mock display/nav drivers here,
 * see spec section "Tests unitaires host-based".
 */

#include <stdio.h>
#include "boot_logic.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);           \
        }                                                                    \
    } while (0)

static boot_decision_input_t make_input(bool has_last_app, const char *label, bool force_menu, bool button_held) {
    boot_decision_input_t in = {0};
    in.has_last_app = has_last_app;
    if (label != NULL) {
        size_t i = 0;
        for (; label[i] != '\0' && i < BOOT_LOGIC_MAX_LABEL_LEN - 1; i++) {
            in.last_app_partition[i] = label[i];
        }
        in.last_app_partition[i] = '\0';
    }
    in.force_menu = force_menu;
    in.button_held = button_held;
    return in;
}

static void test_decide_boots_direct_when_nothing_forces_menu(void) {
    boot_decision_input_t in = make_input(true, "app_slot1", false, false);
    CHECK(boot_logic_decide(&in) == BOOT_ACTION_BOOT_DIRECT, "should boot direct: has last app, no force, no button");
}

static void test_decide_shows_menu_when_no_last_app(void) {
    boot_decision_input_t in = make_input(false, NULL, false, false);
    CHECK(boot_logic_decide(&in) == BOOT_ACTION_SHOW_MENU, "should show menu: first boot, no last app");
}

static void test_decide_shows_menu_when_force_menu_set(void) {
    boot_decision_input_t in = make_input(true, "app_slot1", true, false);
    CHECK(boot_logic_decide(&in) == BOOT_ACTION_SHOW_MENU, "should show menu: force_menu set by guest app");
}

static void test_decide_shows_menu_when_button_held(void) {
    boot_decision_input_t in = make_input(true, "app_slot1", false, true);
    CHECK(boot_logic_decide(&in) == BOOT_ACTION_SHOW_MENU, "should show menu: button held at boot");
}

static void test_decide_shows_menu_when_all_conditions_true(void) {
    boot_decision_input_t in = make_input(true, "app_slot1", true, true);
    CHECK(boot_logic_decide(&in) == BOOT_ACTION_SHOW_MENU, "should show menu: all conditions true at once");
}

static void test_slot_count_matches(void) {
    CHECK(boot_logic_slot_count_matches(3, 3) == true, "3 found / 3 configured should match");
    CHECK(boot_logic_slot_count_matches(0, 3) == false, "0 found / 3 configured should not match");
    CHECK(boot_logic_slot_count_matches(4, 3) == false, "4 found / 3 configured should not match");
    CHECK(boot_logic_slot_count_matches(0, 0) == true, "0 found / 0 configured should match");
}

static void test_valid_app_magic(void) {
    CHECK(boot_logic_is_valid_app_magic(0xE9) == true, "0xE9 is the ESP-IDF app image magic byte");
    CHECK(boot_logic_is_valid_app_magic(0x00) == false, "0x00 (erased/never-flashed flash) is not valid");
    CHECK(boot_logic_is_valid_app_magic(0xFF) == false, "0xFF (erased flash) is not valid");
}

int main(void) {
    test_decide_boots_direct_when_nothing_forces_menu();
    test_decide_shows_menu_when_no_last_app();
    test_decide_shows_menu_when_force_menu_set();
    test_decide_shows_menu_when_button_held();
    test_decide_shows_menu_when_all_conditions_true();
    test_slot_count_matches();
    test_valid_app_magic();

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
