# NEX firmware entry component.
idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "ui.cpp"
        "keys.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        M5GFX
        esp_driver_i2c
        esp_timer
    PRIV_REQUIRES
        lvgl
        heap
)
