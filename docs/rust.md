# Rust / C++ interoperability foundation

Task 003 adds a Rust toolchain and a pinned Rust component to NEX without
touching the verified display, touch, key or power code. ESP-IDF remains the
only SDK and `idf.py` remains the only build entry point.

## 1. Toolchain (pinned)

| Component | Version | Notes |
|---|---|---|
| Host | Fedora Linux 45, kernel 7.3.4 | x86_64 |
| ESP-IDF | v5.5.4 (`$HOME/esp/esp-idf`) | unchanged |
| rustup | 1.29.3 | manages toolchains |
| Rust (Xtensa) | 1.97.0-nightly (8ea53bcd7 2026-07-08) | installed by espup as the `esp` toolchain |
| Cargo (Xtensa) | 1.97.0-nightly (c980f4866 2026-06-30) | |
| espup | 0.19.0 | |
| Xtensa GCC | esp-16.1.0_20260609 (GCC 16.1.0) | from the esp toolchain |
| Xtensa LLVM | esp-21.1.3_20260408 | from the esp toolchain |
| Rust target | `xtensa-esp32s3-espidf` | std built from `rust-src` |

`espup` installs a single, self-contained toolchain named `esp` under
`$HOME/.rustup/toolchains/esp`. The ESP-IDF installation is reused as-is; no
second ESP-IDF is introduced.

### One-time setup (new machine)

```bash
# 1) rustup (no default toolchain needed)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal

# 2) espup (prebuilt release binary is enough)
curl -L -o /tmp/espup https://github.com/esp-rs/espup/releases/download/v0.19.0/espup-x86_64-unknown-linux-gnu
install -m 755 /tmp/espup "$HOME/.cargo/bin/espup"

# 3) Xtensa toolchain for the ESP32-S3 only
espup install --targets esp32s3
```

## 2. Build

```bash
source tools/env.sh          # ESP-IDF + $HOME/.cargo/bin + export-esp.sh
idf.py build
```

Rust output lives in the component build directory
(`build/esp-idf/nex_rust/target/...`), never in the source tree.

## 3. Component layout

```
components/nex_rust/
├── CMakeLists.txt          # Cargo invocation + link into ESP-IDF
├── Cargo.toml              # staticlib, panic=abort, LTO
├── Cargo.lock              # committed (--locked)
├── rust-toolchain.toml     # channel = "esp"
├── .cargo/config.toml      # build-std = ["std", "panic_abort"]
├── include/nex_rust.h      # C ABI contract (shared by C++ and Rust)
└── src/lib.rs              # exported functions
```

### Why `build-std`

Espressif's `esp` toolchain ships `rust-src` but **no prebuilt `std` for
`xtensa-esp32s3-espidf`**. Building std from source is the supported esp-rs
flow for ESP-IDF std projects:

```toml
[unstable]
build-std = ["std", "panic_abort"]
```

`panic_immediate_abort` was intentionally **not** used: on this toolchain it is
now a real panic strategy (`panic = "immediate-abort"`), and the old
`build-std-features` form fails to compile `core`.

`Cargo.lock` is committed and the build uses `--locked`, so the crate graph is
reproducible.

### CMake integration details

* `idf_component_register(INCLUDE_DIRS "include")` exposes the C header.
* A custom command produces `libnex_rust.a` inside the component build dir and
  depends on `src/`, `Cargo.toml`, `Cargo.lock`, `rust-toolchain.toml` and
  `.cargo/config.toml`; editing or adding Rust files re-runs Cargo.
* The component links the archive via `target_link_libraries(... INTERFACE ...)`
  and `main` declares `REQUIRES nex_rust`.
* Code after `idf_component_register` is guarded with
  `if(NOT CMAKE_SCRIPT_MODE_FILE)`: ESP-IDF includes component CMake files in
  script mode while collecting requirements, where build commands are invalid.
* `RUSTUP_TOOLCHAIN=esp` and `CARGO_TARGET_DIR` are set per build command, so no
  global shell state is required beyond `cargo` being on `PATH`.

## 4. C ABI contract

Only these types cross the boundary: `int32_t`, `void`, and
`const char *` borrowing a NUL-terminated string for the duration of the call.

| Direction | Symbol | Owner |
|---|---|---|
| C++ → Rust | `int32_t nex_rust_add(int32_t, int32_t)` | `src/lib.rs` |
| C++ → Rust | `void nex_rust_hello(void)` | `src/lib.rs` |
| Rust → C++ | `void nex_log_from_rust(const char *)` | `main/main.cpp` |

Rules enforced in this task:

* No Rust allocation or Rust object crosses the boundary.
* No `std::string`, `String`, exceptions or panics cross the boundary;
  `panic = "abort"` means a Rust panic aborts instead of unwinding into C++.
* The C++ callback does not throw and does not retain the pointer.
* Rust never touches LVGL or any C++ object; the only side effect is the log
  callback, which the C++ side implements with `ESP_LOGI`.
* No global mutable state in Rust; `nex_rust_add` wraps on overflow instead of
  panicking.

## 5. Verification performed

| Check | Result |
|---|---|
| Baseline build before Rust | `NEX.bin` 654,512 B; Flash Code 423,086 / Data 141,724 / DIRAM 143,167 |
| Integrated `idf.py build` | success, no errors |
| Rust archive linked | yes (`libnex_rust.a` in `NEX.map` and map shows `main.cpp.obj (nex_rust_add)`) |
| Symbols in `NEX.elf` | `nex_rust_add`, `nex_rust_hello`, `nex_log_from_rust` |
| Incremental rebuild | editing `src/lib.rs` re-runs Cargo and relinks; string appears in the new image |
| No-op rebuild | no Cargo/link work |
| Duplicate runtime/symbols | none observed; link succeeded |
| Size delta | **+6,160 B** (654,512 → 660,672), +0.94 % |

Flash Code +128 B, Flash Data +6,032 B, DIRAM and IRAM unchanged. The Rust
static library itself is ~1.1 MB, but only the referenced code and data are
linked into the firmware.

### Runtime verification (after flashing approval)

Boot log:

```
I (1453) NEX: Rust interop: nex_rust_add(20, 22) = 42 (expected 42)
I (1454) NEX_RUST: hello from Rust
I (1457) NEX: UI running
```

* C++ → Rust: `nex_rust_add(20, 22)` returned 42.
* Rust → C++: `nex_log_from_rust()` reached `ESP_LOGI` with the borrowed string.
* AMOLED still shows the Hello World screen; Button A/B/C touch events still
  fire (`NEX-UI: touch test: Button A/B/C pressed/released`).
* KEYA/KEYB short press, both-press and both-long-press still work
  (`NEX_KEY: ...` lines), no reboot or crash during the session.

## 6. Known limitations and risks

1. The Xtensa Rust compiler is a **nightly** build pinned by espup; it is not
   the stable host toolchain. It is installed as `esp` and selected via
   `rust-toolchain.toml`, not by changing the host default.
2. `build-std` compiles `std` from `rust-src` on the first build in each build
   directory (~20 s). This is expected; subsequent builds are incremental.
3. The Rust component currently links `std` (newlib-backed). It references a
   few libc calls (`getenv`, `stat`, ...) resolved by ESP-IDF's newlib; no
   second C runtime is linked.
4. `espup install` downloads roughly 2 GB into `$HOME/.rustup`; it is not part
   of the repository.
5. Only the ESP32-S3 target is installed. Other targets would need
   `espup install --targets <target>`.
6. Runtime verification was done after explicit flashing approval; the display,
   touch and key paths were re-checked on the device.

## 7. Suggested Git commit

```
基础设施:引入 Rust 与 C++ 互操作
```
