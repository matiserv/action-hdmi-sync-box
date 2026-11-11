idf_component_register(
    SRCS "main.c" "sync.c" "capture.c" "spi_out.c" "hdmi.c" "console.c" "params.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_mcpwm esp_driver_pcnt esp_adc esp_timer nvs_flash app_update esp_partition
)
