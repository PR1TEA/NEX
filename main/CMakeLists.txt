# NEX firmware entry component.
idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "services.cpp"
        "ui.cpp"
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
