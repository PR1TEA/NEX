/*
 * NEX Rust core — civil time maths and clock state
 * SPDX-License-Identifier: MIT
 *
 * The core keeps UTC internally; the UI applies the user timezone offset only
 * when formatting. Monotonic milliseconds drive elapsed time, never wall time.
 */

pub const SECS_PER_DAY: i64 = 86_400;

pub const SOURCE_NONE: i32 = 0;
pub const SOURCE_MANUAL: i32 = 1;
pub const SOURCE_RTC: i32 = 2;
pub const SOURCE_BLUETOOTH: i32 = 3;
pub const SOURCE_WIFI: i32 = 4;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct Civil {
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub minute: i32,
    pub second: i32,
}

pub fn is_leap(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

pub fn days_in_month(year: i32, month: i32) -> i32 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 => {
            if is_leap(year) {
                29
            } else {
                28
            }
        }
        _ => 0,
    }
}

pub fn valid(c: &Civil) -> bool {
    c.year >= 2000
        && c.year <= 2099
        && c.month >= 1
        && c.month <= 12
        && c.day >= 1
        && c.day <= days_in_month(c.year, c.month)
        && c.hour >= 0
        && c.hour < 24
        && c.minute >= 0
        && c.minute < 60
        && c.second >= 0
        && c.second < 60
}

fn days_from_civil(year: i32, month: i32, day: i32) -> i64 {
    let y = if month <= 2 { year - 1 } else { year } as i64;
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let m = month as i64;
    let mp = if m > 2 { m - 3 } else { m + 9 };
    let doy = (153 * mp + 2) / 5 + day as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn civil_from_days(z: i64) -> (i32, i32, i32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if m <= 2 { y + 1 } else { y };
    (year as i32, m as i32, d as i32)
}

pub fn civil_to_epoch(c: &Civil) -> i64 {
    days_from_civil(c.year, c.month, c.day) * SECS_PER_DAY
        + c.hour as i64 * 3600
        + c.minute as i64 * 60
        + c.second as i64
}

pub fn epoch_to_civil(secs: i64) -> Civil {
    let days = secs.div_euclid(SECS_PER_DAY);
    let rem = secs.rem_euclid(SECS_PER_DAY);
    let (year, month, day) = civil_from_days(days);
    Civil {
        year,
        month,
        day,
        hour: (rem / 3600) as i32,
        minute: ((rem % 3600) / 60) as i32,
        second: (rem % 60) as i32,
    }
}

#[derive(Clone, Copy, Default)]
pub struct Clock {
    pub valid: bool,
    pub base_utc: i64,
    pub base_mono_ms: u64,
    pub source: i32,
}

impl Clock {
    pub fn set(&mut self, utc: i64, mono_ms: u64, source: i32) {
        self.valid = true;
        self.base_utc = utc;
        self.base_mono_ms = mono_ms;
        self.source = source;
    }

    pub fn invalidate(&mut self) {
        self.valid = false;
        self.source = SOURCE_NONE;
    }

    pub fn now_utc(&self, mono_ms: u64) -> i64 {
        let elapsed = mono_ms.saturating_sub(self.base_mono_ms) / 1000;
        self.base_utc + elapsed as i64
    }

    pub fn local(&self, mono_ms: u64, tz_minutes: i32) -> Civil {
        epoch_to_civil(self.now_utc(mono_ms) + tz_minutes as i64 * 60)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn epoch_roundtrip() {
        let c = Civil {
            year: 2026,
            month: 11,
            day: 26,
            hour: 13,
            minute: 20,
            second: 30,
        };
        let e = civil_to_epoch(&c);
        assert_eq!(epoch_to_civil(e), c);
    }

    #[test]
    fn leap_years() {
        assert!(is_leap(2024));
        assert!(!is_leap(2100));
        assert_eq!(days_in_month(2024, 2), 29);
        assert_eq!(days_in_month(2023, 2), 28);
    }
}
