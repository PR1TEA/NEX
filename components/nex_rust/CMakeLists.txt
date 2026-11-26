# NEX Rust core.
#
# Cargo builds a static library for the ESP32-S3 ESP-IDF target and this
# component links it into the normal ESP-IDF application. Cargo output goes to
# the component build directory, so the source tree stays clean. The command
# depends on the whole src/ directory, so editing or adding Rust files makes
# `idf.py build` rebuild the library; Cargo itself is incremental.

set(NEX_RUST_TARGET "xtensa-esp32s3-espidf")
set(NEX_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")
set(NEX_RUST_LIB "${NEX_RUST_TARGET_DIR}/${NEX_RUST_TARGET}/release/libnex_rust.a")

idf_component_register(
    INCLUDE_DIRS "include"
)

# ESP-IDF includes component CMakeLists.txt in CMake script mode while it
# collects component requirements; build commands are not available there.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    find_program(NEX_CARGO_EXECUTABLE cargo
        HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")

    if(NOT NEX_CARGO_EXECUTABLE)
        message(FATAL_ERROR
            "cargo not found. Install rustup and espup, then put $HOME/.cargo/bin on PATH.")
    endif()

    add_custom_command(
        OUTPUT "${NEX_RUST_LIB}"
        COMMAND ${CMAKE_COMMAND} -E env
                "CARGO_TARGET_DIR=${NEX_RUST_TARGET_DIR}"
                "RUSTUP_TOOLCHAIN=esp"
                "${NEX_CARGO_EXECUTABLE}" build --release --locked --target ${NEX_RUST_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
            "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/.cargo/config.toml"
        COMMENT "Building Rust component nex_rust (${NEX_RUST_TARGET})"
        VERBATIM)

    add_custom_target(nex_rust_build DEPENDS "${NEX_RUST_LIB}")
    add_dependencies(${COMPONENT_LIB} nex_rust_build)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "${NEX_RUST_LIB}")
endif()
