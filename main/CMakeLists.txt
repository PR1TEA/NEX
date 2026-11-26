# NEX firmware entry component.
idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "audio.cpp"
        "ble_upload.cpp"
        "imu.cpp"
        "reminders.cpp"
        "services.cpp"
        "wallpaper.cpp"
        "ui.cpp"
        "fonts/nex_cjk_16.c"
        "fonts/nex_cjk_text_16.c"
        "fonts/nex_cjk_20.c"
        "fonts/nex_font_64_bold.c"
        "fonts/nex_font_64_regular.c"
        "fonts/nex_font_40_regular.c"
        "keys.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        M5GFX
        nex_rust
        esp_driver_i2c
        esp_timer
        esp_app_format
        nvs_flash
        espressif__bmi270
        spiffs
        bt
        json
        espressif__esp_codec_dev
        esp_driver_i2s
    PRIV_REQUIRES
        lvgl
        heap
)

# Inject the short git commit for the System Info page (best effort).
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    OUTPUT_VARIABLE NEX_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT NEX_GIT_COMMIT)
    set(NEX_GIT_COMMIT "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE NEX_GIT_COMMIT="${NEX_GIT_COMMIT}")
