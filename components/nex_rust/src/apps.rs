/*
 * NEX Rust core — built-in app registry
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Static built-in apps only. Compatibility is a simple API-version check; it
 * is not an ABI or memory-safety boundary.
 */

pub const API_VERSION: u32 = 1;

pub struct AppDef {
    pub id: &'static str,
    pub name: &'static str,
    pub entry: &'static str,
    pub version: u32,
    pub required_api: u32,
}

pub const APPS: &[AppDef] = &[
    AppDef {
        id: "nex.sample.hello",
        name: "Hello Sample",
        entry: "hello_main",
        version: 1,
        required_api: 1,
    },
    AppDef {
        id: "nex.sample.clock",
        name: "Clock Sample",
        entry: "clock_main",
        version: 1,
        required_api: 1,
    },
    AppDef {
        id: "nex.sample.legacy",
        name: "Legacy Sample",
        entry: "legacy_main",
        version: 1,
        required_api: 99,
    },
];

pub fn compatible(required_api: u32) -> bool {
    required_api <= API_VERSION
}
