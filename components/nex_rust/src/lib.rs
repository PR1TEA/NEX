//! NEX Rust core — system state, clock policy, navigation and app registry.
//!
//! The C++ side owns hardware, NVS, RTC I2C and LVGL. This crate owns the
//! decisions: what time it is, which page is active, which item is selected,
//! whether developer mode is on and whether a built-in app is compatible.
//!
//! All exported functions are called from the UI task (single thread), but a
//! mutex still protects the state so the ABI stays safe if that changes.
//! No Rust object, allocation or panic crosses the boundary; `panic = "abort"`.
use core::ffi::c_char;
use std::sync::{Mutex, MutexGuard, OnceLock};

mod apps;
mod clock;

use clock::{Civil, Clock, SOURCE_BLUETOOTH, SOURCE_MANUAL, SOURCE_NONE, SOURCE_RTC, SOURCE_WIFI};

// ------------------------------------------------------------------- pages
const PAGE_WATCHFACE: i32 = 0;
const PAGE_SETTINGS: i32 = 1;
const PAGE_TIME_EDITOR: i32 = 2;
const PAGE_SYSTEM_INFO: i32 = 3;
const PAGE_DEVELOPER: i32 = 4;
const PAGE_INPUT_DIAG: i32 = 5;
const PAGE_APPS: i32 = 6;
const PAGE_APP_RUNNER: i32 = 7;
const PAGE_STOPWATCH: i32 = 8;

// ----------------------------------------------------------------- actions
const ACT_UP: i32 = 1;
const ACT_DOWN: i32 = 2;
const ACT_CONFIRM: i32 = 3;
const ACT_BACK: i32 = 4;
const ACT_SELECT: i32 = 5;
const ACT_TOGGLE_DEV: i32 = 6;
const ACT_HOME: i32 = 7;
const ACT_STOPWATCH_TOGGLE: i32 = 8;
const ACT_STOPWATCH_RESET: i32 = 9;
const ACT_OPEN_STOPWATCH: i32 = 10;

// -------------------------------------------------------------------- keys
const KEY_A_CLICK: i32 = 1;
const KEY_B_CLICK: i32 = 2;
const KEY_A_LONG: i32 = 3;
const KEY_B_LONG: i32 = 4;
const KEY_BOTH_LONG: i32 = 5;

// ---------------------------------------------------------------- settings
const SETTINGS_MAGIC: &[u8; 4] = b"NEXS";
const SETTINGS_VERSION: u32 = 1;
const DEFAULT_TZ_MINUTES: i32 = 480; // Asia/Shanghai

// ------------------------------------------------------------------- C ABI
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NexCivil {
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub minute: i32,
    pub second: i32,
    pub tz_minutes: i32,
}

#[repr(C)]
pub struct NexAppInfo {
    pub id: [u8; 24],
    pub name: [u8; 40],
    pub entry: [u8; 24],
    pub version: u32,
    pub required_api: u32,
    pub compatible: i32,
    pub reason: [u8; 64],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NexRtcFields {
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub minute: i32,
    pub second: i32,
    pub flags: u32,
}

extern "C" {
    fn nex_rtc_read(out: *mut NexRtcFields) -> i32;
    fn nex_rtc_write(
        year: i32,
        month: i32,
        day: i32,
        hour: i32,
        minute: i32,
        second: i32,
    ) -> i32;
    fn nex_nvs_load(buf: *mut u8, len: i32) -> i32;
    fn nex_nvs_save(buf: *const u8, len: i32) -> i32;
    fn nex_monotonic_ms() -> u64;
    fn nex_log_from_rust(message: *const c_char);
}

const RTC_FLAG_VLF: u32 = 1 << 0;
const RTC_FLAG_STOPPED: u32 = 1 << 1;
const RTC_FLAG_MAGIC_OK: u32 = 1 << 2;

// -------------------------------------------------------------------- state
struct Settings {
    tz_minutes: i32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            tz_minutes: DEFAULT_TZ_MINUTES,
        }
    }
}

#[derive(Default)]
struct Stopwatch {
    running: bool,
    start_mono_ms: u64,
    accumulated_ms: u64,
}

impl Stopwatch {
    fn start(&mut self, now: u64) {
        if !self.running {
            self.running = true;
            self.start_mono_ms = now;
        }
    }
    fn stop(&mut self, now: u64) {
        if self.running {
            self.accumulated_ms = self.accumulated_ms.saturating_add(now.saturating_sub(self.start_mono_ms));
            self.running = false;
        }
    }
    fn reset(&mut self) {
        self.running = false;
        self.accumulated_ms = 0;
        self.start_mono_ms = 0;
    }
    fn elapsed(&self, now: u64) -> u64 {
        if self.running {
            self.accumulated_ms.saturating_add(now.saturating_sub(self.start_mono_ms))
        } else {
            self.accumulated_ms
        }
    }
}

struct Core {
    page: i32,
    selection: i32,
    settings: Settings,
    clock: Clock,
    dev_enabled: bool,
    running_app: i32,
    system_parent: i32,
    error: [u8; 96],
    initialized: bool,
    stopwatch: Stopwatch,
}

impl Default for Core {
    fn default() -> Self {
        Self {
            page: PAGE_WATCHFACE,
            selection: 0,
            settings: Settings::default(),
            clock: Clock::default(),
            dev_enabled: false,
            running_app: -1,
            system_parent: PAGE_SETTINGS,
            error: [0u8; 96],
            initialized: false,
            stopwatch: Stopwatch::default(),
        }
    }
}

static CORE: OnceLock<Mutex<Core>> = OnceLock::new();

fn lock() -> MutexGuard<'static, Core> {
    let m = CORE.get_or_init(|| Mutex::new(Core::default()));
    match m.lock() {
        Ok(g) => g,
        Err(poisoned) => poisoned.into_inner(),
    }
}

fn copy_str(dst: &mut [u8], src: &str) {
    dst.fill(0);
    let bytes = src.as_bytes();
    let n = core::cmp::min(bytes.len(), dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&bytes[..n]);
}

impl Core {
    fn set_error(&mut self, message: &str) {
        copy_str(&mut self.error, message);
    }

    fn load_settings(&mut self) {
        let mut buf = [0u8; 32];
        let n = unsafe { nex_nvs_load(buf.as_mut_ptr(), buf.len() as i32) };
        if n < 16 || &buf[0..4] != SETTINGS_MAGIC {
            return;
        }
        let mut version_bytes = [0u8; 4];
        version_bytes.copy_from_slice(&buf[4..8]);
        let mut tz_bytes = [0u8; 4];
        tz_bytes.copy_from_slice(&buf[8..12]);
        let version = u32::from_le_bytes(version_bytes);
        let tz = i32::from_le_bytes(tz_bytes);
        if version == SETTINGS_VERSION && (-720..=840).contains(&tz) {
            self.settings.tz_minutes = tz;
        }
    }

    fn save_settings(&self) -> i32 {
        let mut buf = [0u8; 16];
        buf[0..4].copy_from_slice(SETTINGS_MAGIC);
        buf[4..8].copy_from_slice(&SETTINGS_VERSION.to_le_bytes());
        buf[8..12].copy_from_slice(&self.settings.tz_minutes.to_le_bytes());
        buf[12..16].copy_from_slice(&0u32.to_le_bytes());
        unsafe { nex_nvs_save(buf.as_ptr(), buf.len() as i32) }
    }

    fn init(&mut self) {
        self.load_settings();

        let mono = unsafe { nex_monotonic_ms() };
        let mut rtc = NexRtcFields::default();
        let rc = unsafe { nex_rtc_read(&mut rtc) };

        if rc != 0 {
            self.clock.invalidate();
            self.set_error("RTC not responding");
        } else if (rtc.flags & (RTC_FLAG_VLF | RTC_FLAG_STOPPED)) != 0 {
            self.clock.invalidate();
            self.set_error("RTC lost power; set time manually");
        } else if (rtc.flags & RTC_FLAG_MAGIC_OK) == 0 {
            self.clock.invalidate();
            self.set_error("RTC time was not written by NEX (UTC)");
        } else {
            let c = Civil {
                year: rtc.year,
                month: rtc.month,
                day: rtc.day,
                hour: rtc.hour,
                minute: rtc.minute,
                second: rtc.second,
            };
            if clock::valid(&c) {
                self.clock.set(clock::civil_to_epoch(&c), mono, SOURCE_RTC);
                self.set_error("");
            } else {
                self.clock.invalidate();
                self.set_error("RTC date is out of range");
            }
        }

        self.page = PAGE_WATCHFACE;
        self.selection = 0;
        self.dev_enabled = false;
        self.running_app = -1;
        self.initialized = true;
    }

    fn item_count(&self) -> i32 {
        match self.page {
            PAGE_SETTINGS => 4,
            PAGE_DEVELOPER => 5,
            PAGE_APPS => apps::APPS.len() as i32 + 1,
            _ => 1,
        }
    }

    fn open(&mut self, page: i32) {
        self.page = page;
        self.selection = 0;
    }

    fn move_selection(&mut self, delta: i32) {
        let count = self.item_count();
        if count <= 0 {
            return;
        }
        self.selection = (self.selection + delta).rem_euclid(count);
    }

    fn start_app(&mut self, index: i32) -> i32 {
        if !self.dev_enabled {
            self.set_error("Developer mode is disabled");
            return -1;
        }
        if index < 0 || index as usize >= apps::APPS.len() {
            self.set_error("Unknown app");
            return -2;
        }
        let app = &apps::APPS[index as usize];
        if !apps::compatible(app.required_api) {
            self.set_error("App requires a newer NEX API");
            return -3;
        }
        self.running_app = index;
        self.set_error("");
        self.open(PAGE_APP_RUNNER);
        0
    }

    fn confirm(&mut self) {
        match self.page {
            PAGE_SETTINGS => match self.selection {
                0 => self.open(PAGE_TIME_EDITOR),
                1 => {
                    self.system_parent = PAGE_SETTINGS;
                    self.open(PAGE_SYSTEM_INFO);
                }
                2 => self.open(PAGE_DEVELOPER),
                3 => self.open(PAGE_WATCHFACE),
                _ => {}
            },
            PAGE_DEVELOPER => match self.selection {
                0 => {
                    self.dev_enabled = !self.dev_enabled;
                    self.set_error("");
                }
                1 => {
                    self.system_parent = PAGE_DEVELOPER;
                    self.open(PAGE_SYSTEM_INFO);
                }
                2 => self.open(PAGE_INPUT_DIAG),
                3 => self.open(PAGE_APPS),
                4 => self.open(PAGE_SETTINGS),
                _ => {}
            },
            PAGE_APPS => {
                if (self.selection as usize) < apps::APPS.len() {
                    let index = self.selection;
                    self.start_app(index);
                } else {
                    self.open(PAGE_DEVELOPER);
                }
            }
            PAGE_SYSTEM_INFO | PAGE_INPUT_DIAG => self.back(),
            PAGE_APP_RUNNER => {
                self.running_app = -1;
                self.open(PAGE_WATCHFACE);
            }
            _ => {}
        }
    }

    fn back(&mut self) {
        let target = match self.page {
            PAGE_SETTINGS | PAGE_APP_RUNNER => PAGE_WATCHFACE,
            PAGE_TIME_EDITOR | PAGE_DEVELOPER => PAGE_SETTINGS,
            PAGE_STOPWATCH => PAGE_WATCHFACE,
            PAGE_SYSTEM_INFO => self.system_parent,
            PAGE_INPUT_DIAG | PAGE_APPS => PAGE_DEVELOPER,
            _ => PAGE_WATCHFACE,
        };
        if self.page == PAGE_APP_RUNNER {
            self.running_app = -1;
        }
        self.open(target);
    }

    fn action(&mut self, action: i32, arg: i32) {
        match action {
            ACT_UP => self.move_selection(-1),
            ACT_DOWN => self.move_selection(1),
            ACT_CONFIRM => self.confirm(),
            ACT_BACK => self.back(),
            ACT_SELECT => {
                let count = self.item_count();
                if count > 0 {
                    self.selection = arg.clamp(0, count - 1);
                }
            }
            ACT_TOGGLE_DEV => {
                self.dev_enabled = !self.dev_enabled;
                self.set_error("");
            }
            ACT_HOME => {
                self.running_app = -1;
                self.open(PAGE_WATCHFACE);
            }
            ACT_STOPWATCH_TOGGLE => {
                let now = unsafe { nex_monotonic_ms() };
                if self.stopwatch.running {
                    self.stopwatch.stop(now);
                } else {
                    self.stopwatch.start(now);
                }
            }
            ACT_STOPWATCH_RESET => self.stopwatch.reset(),
            ACT_OPEN_STOPWATCH => self.open(PAGE_STOPWATCH),
            _ => {}
        }
    }

    fn on_key(&mut self, event: i32) {
        match event {
            KEY_A_CLICK => self.action(ACT_UP, 0),
            KEY_B_CLICK => self.action(ACT_DOWN, 0),
            KEY_A_LONG => self.action(ACT_BACK, 0),
            KEY_B_LONG => {
                if self.page == PAGE_WATCHFACE {
                    self.open(PAGE_SETTINGS);
                } else {
                    self.action(ACT_CONFIRM, 0);
                }
            }
            KEY_BOTH_LONG => self.action(ACT_HOME, 0),
            _ => {}
        }
    }

    /// Returns 0 on success. Bit 0: invalid input, bit 1: RTC write failed,
    /// bit 2: timezone could not be persisted (clock still updated).
    fn set_time(
        &mut self,
        year: i32,
        month: i32,
        day: i32,
        hour: i32,
        minute: i32,
        second: i32,
        tz_minutes: i32,
    ) -> i32 {
        let local = Civil {
            year,
            month,
            day,
            hour,
            minute,
            second,
        };
        if !clock::valid(&local) || !(-720..=840).contains(&tz_minutes) {
            self.set_error("Invalid date, time or timezone");
            return 1;
        }

        let utc = clock::civil_to_epoch(&local) - tz_minutes as i64 * 60;
        let utc_civil = clock::epoch_to_civil(utc);
        let rc = unsafe {
            nex_rtc_write(
                utc_civil.year,
                utc_civil.month,
                utc_civil.day,
                utc_civil.hour,
                utc_civil.minute,
                utc_civil.second,
            )
        };
        if rc != 0 {
            self.set_error("RTC write failed; time not changed");
            return 2;
        }

        let mono = unsafe { nex_monotonic_ms() };
        self.clock.set(utc, mono, SOURCE_MANUAL);
        self.settings.tz_minutes = tz_minutes;
        if self.save_settings() != 0 {
            self.set_error("Time set, but timezone was not saved");
            return 4;
        }
        self.set_error("");
        0
    }

    fn digits(&self, mono: u64) -> [u8; 7] {
        let mut out = [b'-'; 7];
        out[6] = 0;
        if !self.clock.valid {
            return out;
        }
        let c = self.clock.local(mono, self.settings.tz_minutes);
        out[0] = b'0' + (c.hour / 10) as u8;
        out[1] = b'0' + (c.hour % 10) as u8;
        out[2] = b'0' + (c.minute / 10) as u8;
        out[3] = b'0' + (c.minute % 10) as u8;
        out[4] = b'0' + (c.second / 10) as u8;
        out[5] = b'0' + (c.second % 10) as u8;
        out
    }

    fn local_fields(&self) -> NexCivil {
        let c = if self.clock.valid {
            self.clock
                .local(unsafe { nex_monotonic_ms() }, self.settings.tz_minutes)
        } else {
            Civil {
                year: 2026,
                month: 1,
                day: 1,
                hour: 0,
                minute: 0,
                second: 0,
            }
        };
        NexCivil {
            year: c.year,
            month: c.month,
            day: c.day,
            hour: c.hour,
            minute: c.minute,
            second: c.second,
            tz_minutes: self.settings.tz_minutes,
        }
    }
}

// ------------------------------------------------------------- Task 003 API
#[no_mangle]
pub extern "C" fn nex_rust_add(a: i32, b: i32) -> i32 {
    a.wrapping_add(b)
}

#[no_mangle]
pub extern "C" fn nex_rust_hello() {
    let message = c"hello from Rust";
    unsafe {
        nex_log_from_rust(message.as_ptr());
    }
}

// ------------------------------------------------------------- core C ABI
#[no_mangle]
pub extern "C" fn nex_core_init() -> i32 {
    let mut core = lock();
    core.init();
    0
}

#[no_mangle]
pub extern "C" fn nex_core_tick(mono_ms: u64) {
    let _ = mono_ms; // the clock derives time from nex_monotonic_ms()
}

#[no_mangle]
pub extern "C" fn nex_core_key_event(event: i32) {
    lock().on_key(event);
}

#[no_mangle]
pub extern "C" fn nex_core_action(action: i32, arg: i32) {
    lock().action(action, arg);
}

#[no_mangle]
pub extern "C" fn nex_core_page() -> i32 {
    lock().page
}

#[no_mangle]
pub extern "C" fn nex_core_selection() -> i32 {
    lock().selection
}

#[no_mangle]
pub extern "C" fn nex_core_dev_enabled() -> i32 {
    if lock().dev_enabled {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn nex_core_clock_valid() -> i32 {
    if lock().clock.valid {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn nex_core_clock_source() -> i32 {
    let core = lock();
    if core.clock.valid {
        core.clock.source
    } else {
        SOURCE_NONE
    }
}

#[no_mangle]
pub extern "C" fn nex_core_api_version() -> i32 {
    apps::API_VERSION as i32
}

#[no_mangle]
pub extern "C" fn nex_core_time_digits(out: *mut c_char) {
    if out.is_null() {
        return;
    }
    let mono = unsafe { nex_monotonic_ms() };
    let digits = lock().digits(mono);
    unsafe {
        core::ptr::copy_nonoverlapping(digits.as_ptr(), out as *mut u8, digits.len());
    }
}

#[no_mangle]
pub extern "C" fn nex_core_local_fields(out: *mut NexCivil) {
    if out.is_null() {
        return;
    }
    let value = lock().local_fields();
    unsafe {
        *out = value;
    }
}

#[no_mangle]
pub extern "C" fn nex_core_set_time(
    year: i32,
    month: i32,
    day: i32,
    hour: i32,
    minute: i32,
    second: i32,
    tz_minutes: i32,
) -> i32 {
    lock().set_time(year, month, day, hour, minute, second, tz_minutes)
}

/// Reserved unified synchronisation entry point. Manual/RTC writes go through
/// `nex_core_set_time`; Bluetooth/WiFi are intentionally not implemented.
#[no_mangle]
pub extern "C" fn nex_core_sync_time(source: i32) -> i32 {
    let mut core = lock();
    match source {
        SOURCE_MANUAL | SOURCE_RTC => {
            core.set_error("Use nex_core_set_time for manual/RTC time");
            -1
        }
        SOURCE_BLUETOOTH | SOURCE_WIFI => {
            core.set_error("Bluetooth/WiFi time sync not implemented");
            -1
        }
        _ => -1,
    }
}

#[no_mangle]
pub extern "C" fn nex_core_stopwatch_ms() -> u64 {
    let core = lock();
    core.stopwatch.elapsed(unsafe { nex_monotonic_ms() })
}

#[no_mangle]
pub extern "C" fn nex_core_stopwatch_running() -> i32 {
    if lock().stopwatch.running {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn nex_core_app_count() -> i32 {
    apps::APPS.len() as i32
}

#[no_mangle]
pub extern "C" fn nex_core_app_info(index: i32, out: *mut NexAppInfo) -> i32 {
    if out.is_null() || index < 0 || index as usize >= apps::APPS.len() {
        return -1;
    }
    let app = &apps::APPS[index as usize];
    let compatible = apps::compatible(app.required_api);
    let mut info = NexAppInfo {
        id: [0u8; 24],
        name: [0u8; 40],
        entry: [0u8; 24],
        version: app.version,
        required_api: app.required_api,
        compatible: if compatible { 1 } else { 0 },
        reason: [0u8; 64],
    };
    copy_str(&mut info.id, app.id);
    copy_str(&mut info.name, app.name);
    copy_str(&mut info.entry, app.entry);
    copy_str(
        &mut info.reason,
        if compatible {
            "OK"
        } else {
            "requires newer NEX API"
        },
    );
    unsafe {
        *out = info;
    }
    0
}

#[no_mangle]
pub extern "C" fn nex_core_start_app(index: i32) -> i32 {
    lock().start_app(index)
}

#[no_mangle]
pub extern "C" fn nex_core_running_app() -> i32 {
    lock().running_app
}

#[no_mangle]
pub extern "C" fn nex_core_last_error(out: *mut c_char, len: i32) {
    if out.is_null() || len <= 0 {
        return;
    }
    let core = lock();
    let n = core::cmp::min((len - 1) as usize, core.error.len());
    unsafe {
        core::ptr::copy_nonoverlapping(core.error.as_ptr(), out as *mut u8, n);
        *out.add(n) = 0;
    }
}
