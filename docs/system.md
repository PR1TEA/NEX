# NEX minimum system: core, watchface, settings, developer mode

Task 004 turns the hardware demo into a small system. This file records the
architecture, the time-service contract and the verification evidence.

## 1. Responsibilities

| Layer | Owns | Does not own |
|---|---|---|
| C++ (`main/board.cpp`) | I²C, M5PM1/M5IOE1, CO5300, CST820, LVGL task | time policy, navigation |
| C++ (`main/services.cpp`) | RX8130CE access, NVS blob, monotonic ms | date/time policy, timezone maths |
| C++ (`main/ui.cpp`) | every LVGL object, page rendering, input dispatch | which page is active, validation |
| Rust (`components/nex_rust`) | clock policy, settings values, navigation, app registry, compatibility | hardware, LVGL, NVS I/O |
| C++ (`main/keys.cpp`) | debounce, long-press, combo detection | what an event means |

All Rust calls happen on the UI task (keys are queued to it), so the core is
single-threaded in practice; a mutex still protects the state.

## 2. Startup

```
board_init()                I2C/PMIC/IOE/L3B/panel/touch (unchanged)
services_init()             NVS + RX8130CE device handle
nex_core_init()             settings → RTC validation → clock state
ui_start()                  LVGL display + 8 page containers + UI task
keys_start()                GPIO1/2 polling task
```

If the clock is invalid the watchface shows `------` and the editor validates
input before writing anything.

## 3. Pages and navigation

| Page | Entry | Back |
|---|---|---|
| `WATCHFACE` | boot / BOTH LONG | — |
| `SETTINGS` | KEYB LONG on watchface | row 4 / KEYA LONG |
| `TIME_EDITOR` | Settings → Time & Date | Cancel / KEYA LONG |
| `SYSTEM_INFO` | Settings → System Info, or Developer → System Status | Back / KEYA LONG |
| `DEVELOPER` | Settings → Developer Options | row 5 / KEYA LONG |
| `INPUT_DIAG` | Developer → Input Diagnostics | Back |
| `APPS` | Developer → Built-in Apps | Back row |
| `APP_RUNNER` | compatible app row | Exit App / BOTH LONG |

Input semantics (touch is primary):

* touch a row = select + confirm
* KEYA click = previous item, KEYB click = next item
* KEYA long = back, KEYB long = confirm (watchface: KEYB long opens Settings)
* BOTH long = emergency return to watchface

`KEYA/B PRESSED` is still logged for the Task 002 regression but is never
converted into an action: only CLICK (on release) and LONG (once, at 1000 ms)
reach the navigation layer, so holding a key cannot fire a short action first.

Pages are created once at startup and then shown/hidden; navigation never
allocates a new page tree.

## 4. Time service

* Internal representation: **UTC seconds** plus a timezone offset in minutes.
* The RX8130CE is written in **UTC**. A 32-bit RTC-RAM marker `"NEXU"` records
  that the content was written by NEX; without it the stored time is treated as
  untrusted and the clock starts invalid (no assumption that existing content
  is UTC).
* Startup validation: I2C result, BCD decode, range checks, `VLF` (oscillator
  lost) and `STOP` bits, plus the marker. Any failure → clock invalid.
* Runtime reads use `nex_monotonic_ms()` from a base point; the RTC is read once
  at boot, not every second.
* Manual set: validate fields (including leap years and month lengths), convert
  local → UTC with the offset, write the RTC, update the clock base, persist the
  timezone. Return code: bit0 invalid input, bit1 RTC write failed, bit2
  timezone not persisted (clock still updated).
* Source types reserved: `Manual`, `HardwareRtc`, `Bluetooth`, `WiFi`.
  `nex_core_sync_time()` exists for future BLE/Wi-Fi sync and returns `-1`
  with a "not implemented" error; no fake sync is performed.

C ABI (see `main/nex_core.h`): `get` via `nex_core_time_digits`,
`nex_core_local_fields`, `nex_core_clock_valid/source`; `set` via
`nex_core_set_time`; status via `nex_core_last_error`.

## 5. Settings and developer mode

* NVS namespace `nex`, key `core`: `magic "NEXS"`, version 1, timezone minutes,
  reserved. Written only when the user saves the time, never periodically.
* Developer mode is session-only: default OFF, reset to OFF on every boot.
* Developer menu: mode toggle, system status, input diagnostics, built-in apps,
  back. It is only reachable through Settings → Developer Options.

## 6. Built-in apps

Static registry in Rust (`apps.rs`), API version 1:

| App | Required API | Result |
|---|---|---|
| Hello Sample | 1 | compatible |
| Clock Sample | 1 | compatible |
| Legacy Sample | 99 | rejected: `requires newer NEX API` |

Starting an app requires developer mode. Starting an incompatible app keeps the
list open and shows the reason. The app runner page is a placeholder that proves
registration, start and exit; it is not a real app framework.

## 7. Verification (real hardware)

Boot with no trusted RTC content:

```
NEX: core ready: clock=invalid source=0 page=0
NEX-UI: page -> WATCHFACE
```

Manual set through the UI:

```
NEX-UI: page -> TIME EDITOR
NEX-UI: time set to 2026-11-26 11:50:49 tz=+480 min
NEX-UI: page -> SETTINGS
```

Reboot (RTC restore):

```
NEX: core ready: clock=valid source=2 page=0
NEX-UI: page -> WATCHFACE
```

Navigation / app flow captured on the wire:

```
KEYB LONG PRESS (1000ms) → page -> SETTINGS
page -> DEVELOPER → page -> APPS
page -> APP               (Hello Sample started)
page -> WATCHFACE         (Exit App)
```

KEYA/KEYB CLICK lines were present in every menu; BOTH LONG returned to the
watchface. Touch buttons and the three original UI buttons were re-checked by
the user; no reboot, hang or visible display corruption during the session.

## 8. Size and memory

| Metric | Task 003 | Task 004 |
|---|---|---|
| `NEX.bin` | 660,672 B | 1,006,608 B |
| Flash Code | 423,214 | 619,878 |
| Flash Data | 147,756 | 292,876 |
| DIRAM | 143,167 (41.9 %) | 147,955 (43.3 %) |
| IRAM | 16,384 | 16,384 |

The growth is mostly the LVGL 48 px font and the new page widgets. The app
partition is 3 MB, so the image uses about 32 % of it.

## 9. Known limitations

1. The watchface reads the clock through Rust once per second; only the changed
   labels are invalidated, but there is no explicit TE/tickless optimisation yet.
2. Time editor default start value is the build timestamp when the clock has
   never been set; the watchface still shows `------` until Save.
3. Bluetooth/Wi-Fi sync, DST rules and named timezones are not implemented;
   only a fixed offset in minutes.
4. The app runner is a placeholder; there is no app sandbox or dynamic loading.
5. Developer mode gates app start but is not a security boundary.
6. Uptime/System Info refresh when the page is opened or a key/touch event
   arrives; it does not tick once per second on that page.
