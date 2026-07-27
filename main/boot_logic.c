#include "boot_logic.h"

#define ESP_IMAGE_HEADER_MAGIC 0xE9

boot_action_t boot_logic_decide(const boot_decision_input_t *input) {
    if (input->force_menu || input->button_held || !input->has_last_app) {
        return BOOT_ACTION_SHOW_MENU;
    }
    return BOOT_ACTION_BOOT_DIRECT;
}

bool boot_logic_slot_count_matches(int found_ota_partitions, int configured_slot_count) {
    return found_ota_partitions == configured_slot_count;
}

bool boot_logic_is_valid_app_magic(uint8_t first_byte) {
    return first_byte == ESP_IMAGE_HEADER_MAGIC;
}
