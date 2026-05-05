//! Fuzz the split_combined_samplers pass in isolation.
//!
//! This is the most complex pass (~500 lines) — it allocates new IDs,
//! rewrites entry points, manipulates bindings, and injects new instructions.
//! Focused fuzzing here has the highest chance of finding bugs.

#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let _ = naga_converter::split_combined_samplers(data);
});
