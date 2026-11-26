//! NEX Rust core — minimal, auditable C ABI surface.
//!
//! Rules enforced by this crate:
//!   * only fixed-width integer and `*const c_char` cross the boundary;
//!   * no Rust allocation or Rust object ever leaves the crate;
//!   * no panic unwinds across FFI (`panic = "abort"` in the release profile);
//!   * the C++ side owns every buffer that is passed in.
use core::ffi::c_char;

extern "C" {
    /// Implemented by C++ (`main/main.cpp`). The pointer is valid for the
    /// duration of the call and must not be retained by the callee.
    fn nex_log_from_rust(message: *const c_char);
}

/// Adds two 32-bit integers. Overflow wraps (defined behaviour, no panic).
#[no_mangle]
pub extern "C" fn nex_rust_add(a: i32, b: i32) -> i32 {
    a.wrapping_add(b)
}

/// Proves the Rust -> C++ direction: sends one static string to the C++
/// logger through the C ABI. No allocation, no retained pointer.
#[no_mangle]
pub extern "C" fn nex_rust_hello() {
    let message = c"hello from Rust";
    unsafe {
        nex_log_from_rust(message.as_ptr());
    }
}
