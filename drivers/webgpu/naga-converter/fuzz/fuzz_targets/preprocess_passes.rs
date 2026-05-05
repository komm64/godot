//! Fuzz all SPIR-V preprocessing passes in sequence.
//!
//! Runs each binary rewriting pass on the input, chaining output → input.
//! This exercises the passes without naga parsing, catching panics in the
//! raw SPIR-V manipulation code (index out-of-bounds, integer overflow, etc.).

#![no_main]

use libfuzzer_sys::fuzz_target;
use naga_converter::{
    freeze_spec_constant_ops,
    rewrite_copy_logical,
    rewrite_terminate_invocation,
    infer_readonly_storage,
    convert_push_constants_to_uniforms,
    split_combined_samplers,
    fix_depth2_images,
};

fuzz_target!(|data: &[u8]| {
    // Run each pass in the same order as spirv_to_wgsl.
    // Each pass must handle arbitrary byte sequences without panicking.
    let step1 = freeze_spec_constant_ops(data);
    let step2 = rewrite_copy_logical(&step1);
    let step3 = rewrite_terminate_invocation(&step2);
    let step4 = infer_readonly_storage(&step3);
    let step5 = convert_push_constants_to_uniforms(&step4);
    let step6 = split_combined_samplers(&step5);
    let _ = fix_depth2_images(&step6);
});
