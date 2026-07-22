set(launcher_srcs
    "main.c"
    "app_registry.c"
    "nvs_state.c"
    "boot_logic.c"
    "boot_into.c"
    "ui_menu.c"
    "display_st7789.c"
)

if(CONFIG_LAUNCHER_NAV_DRIVER_MOCK)
    list(APPEND launcher_srcs "nav_input_mock.c")
else()
    list(APPEND launcher_srcs "nav_input_ec11.c")
endif()

idf_component_register(
    SRCS ${launcher_srcs}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_partition app_update esp_lcd driver esp_timer
)
