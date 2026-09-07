set(launcher_srcs
    "main.c"
    "app_registry.c"
    "nvs_state.c"
    "boot_logic.c"
    "boot_into.c"
    "ui_menu.c"
)

# NOTE: REQUIRES/PRIV_REQUIRES cannot reliably be made conditional on
# CONFIG_* Kconfig values -- ESP-IDF resolves component requirements in an
# earlier pass than SRCS/COMPILE_DEFINITIONS, before Kconfig values are
# necessarily available (verified empirically: a CONFIG_LAUNCHER_NET_WIFI_ENABLE-gated
# `list(APPEND ... esp_wifi)` silently had no effect while the equally-gated
# SRCS entry worked fine). These components are already part of ESP-IDF's
# default build set for this target regardless (visible in any build's
# "-- Components: ..." log line) and unused code is dropped at link time via
# -ffunction-sections/-fdata-sections, so declaring them unconditionally here
# does not bloat the flashed binary -- only the SRCS conditionals below
# (which reference or don't reference their symbols) actually matter for that.
set(launcher_requires
    nvs_flash
    esp_partition
    app_update
    esp_lcd
    esp_lcd_gc9a01
    driver
    esp_timer
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp_http_server
    json
    mbedtls
    bt
)

# Exactly one display panel driver; both implement the panel-agnostic
# display.h API (see the LAUNCHER_DISPLAY_DRIVER Kconfig choice). esp_lcd_gc9a01
# stays in REQUIRES unconditionally (conditional REQUIRES is unreliable, see
# CLAUDE.md) -- it is dropped at link when display_gc9a01.c is not compiled.
if(CONFIG_LAUNCHER_DISPLAY_DRIVER_GC9A01)
    list(APPEND launcher_srcs "display_gc9a01.c")
else()
    list(APPEND launcher_srcs "display_st7789.c")
endif()

if(CONFIG_LAUNCHER_NAV_DRIVER_MOCK)
    list(APPEND launcher_srcs "nav_input_mock.c")
elseif(CONFIG_LAUNCHER_NAV_DRIVER_BUTTONS)
    list(APPEND launcher_srcs "nav_input_buttons.c")
else()
    list(APPEND launcher_srcs "nav_input_ec11.c")
endif()

if(CONFIG_LAUNCHER_NET_WIFI_ENABLE)
    list(APPEND launcher_srcs "net_wifi.c")
endif()

if(CONFIG_LAUNCHER_NET_OTA_ENABLE OR CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE)
    list(APPEND launcher_srcs "net_manifest.c" "net_http_util.c")
endif()

if(CONFIG_LAUNCHER_NET_OTA_ENABLE)
    list(APPEND launcher_srcs "net_ota.c" "net_github.c")
endif()

if(CONFIG_LAUNCHER_NET_VERSION_CHECK_ENABLE)
    list(APPEND launcher_srcs "net_version_check.c")
endif()

if(CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_HTTP)
    list(APPEND launcher_srcs "net_remote_http.c")
endif()

if(CONFIG_LAUNCHER_NET_REMOTE_TRANSPORT_BLE)
    list(APPEND launcher_srcs "net_remote_ble.c")
endif()

idf_component_register(
    SRCS ${launcher_srcs}
    INCLUDE_DIRS "."
    REQUIRES ${launcher_requires}
)
