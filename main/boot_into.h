#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware/ESP-IDF-dependent half of the boot sequence: finds the target
 * partition, refuses to jump into an unflashed/invalid slot (magic byte
 * check, see boot_logic_is_valid_app_magic), then calls
 * esp_ota_set_boot_partition() + esp_restart(). Returns an error instead of
 * restarting if the partition is missing or looks invalid, so the caller
 * can show a message and stay in the menu instead of crash-looping.
 */
esp_err_t boot_into(const char *partition_label);

#ifdef __cplusplus
}
#endif
