idf_component_register(
    SRCS app_main.c crash_report.c panel.c touch_gt911.c audio.c hal_esp.c
    INCLUDE_DIRS "."
    REQUIRES macui minivmac esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_sdspi
             esp_driver_spi esp_lcd esp_timer fatfs sdmmc esp_psram espcoredump)

# The firmware's version (VERSION), shown on the crash report
file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" CYD_VERSION)
string(STRIP "${CYD_VERSION}" CYD_VERSION)
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION="${CYD_VERSION}")
