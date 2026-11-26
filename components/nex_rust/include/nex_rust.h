/*
 * NEX Rust core — C ABI boundary
 * SPDX-License-Identifier: MIT
 *
 * Keep this header in sync with components/nex_rust/src/lib.rs.
 * Only fixed-width integers and NUL-terminated strings cross the boundary.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in Rust (components/nex_rust/src/lib.rs). */
int32_t nex_rust_add(int32_t a, int32_t b);
void nex_rust_hello(void);

/* Implemented in C++ (main/main.cpp), called from Rust. */
void nex_log_from_rust(const char *message);

#ifdef __cplusplus
}
#endif
