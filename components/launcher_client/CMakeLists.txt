idf_component_register(
    SRCS "launcher_client.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_partition app_update
)
