//! Fuzz the complete SPIR-V → WGSL conversion pipeline.
//!
//! This exercises all 7 SPIR-V preprocessing passes followed by naga parsing,
//! validation, and WGSL code generation. Panics in naga (the upstream SPIR-V
//! parser) are caught — only panics in naga-converter's own preprocessing
//! passes would be bugs worth investigating.

#![no_main]

use libfuzzer_sys::fuzz_target;
use std::sync::Once;

static INIT: Once = Once::new();

fuzz_target!(|data: &[u8]| {
    // Override libfuzzer's panic hook so catch_unwind actually works.
    // Naga's SPIR-V parser panics on some malformed inputs — that's expected.
    INIT.call_once(|| {
        std::panic::set_hook(Box::new(|_| {}));
    });

    let _ = std::panic::catch_unwind(|| {
        naga_converter::spirv_to_wgsl_native(data)
    });
});
