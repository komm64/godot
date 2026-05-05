use wasm_bindgen::prelude::*;

use std::collections::{HashMap, HashSet};
use naga::{
    AddressSpace, Arena, Binding, BuiltIn, Expression, Handle, Interpolation,
    Literal, Module, Sampling, StorageAccess, Type, TypeInner,
    back::{wgsl, pipeline_constants::process_overrides, PipelineConstants},
    front::spv,
    valid::{Validator, ValidationFlags, Capabilities},
};

#[cfg(target_arch = "wasm32")]
#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = console)]
    fn log(s: &str);
}

#[cfg(not(target_arch = "wasm32"))]
fn log(_s: &str) {
    // No-op outside WASM (tests, fuzzing, native builds).
}

// SPIR-V opcodes relevant to specialization constants.
const OP_TYPE_BOOL: u16 = 20;
const OP_TYPE_INT: u16 = 21;
const OP_TYPE_FLOAT: u16 = 22;
const OP_DECORATE: u16 = 71;
const OP_CONSTANT_TRUE: u16 = 41;
const OP_CONSTANT_FALSE: u16 = 42;
const OP_CONSTANT: u16 = 43;
const OP_CONSTANT_COMPOSITE: u16 = 44;
const OP_SPEC_CONSTANT_TRUE: u16 = 48;
const OP_SPEC_CONSTANT_FALSE: u16 = 49;
const OP_SPEC_CONSTANT: u16 = 50;
const OP_SPEC_CONSTANT_COMPOSITE: u16 = 51;
const OP_SPEC_CONSTANT_OP: u16 = 52;

/// Read a u32 from a little-endian byte slice at the given word index.
/// Returns 0 for out-of-bounds access (malformed SPIR-V safety).
fn read_word(bytes: &[u8], word_idx: usize) -> u32 {
    let off = word_idx * 4;
    if off + 3 >= bytes.len() {
        return 0;
    }
    u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]])
}

/// Write a u32 to a little-endian byte vec.
fn push_word(out: &mut Vec<u8>, w: u32) {
    out.extend_from_slice(&w.to_le_bytes());
}

/// Freeze OpSpecConstantOp instructions by evaluating them with default values.
/// Naga does not support OpSpecConstantOp, causing InvalidId cascades.
/// We evaluate each op using previously-collected constant values and emit
/// regular OpConstant instructions instead.
pub fn freeze_spec_constant_ops(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }

    // Collect type info: type_id -> (is_bool, bit_width, is_signed)
    let mut type_bool: HashMap<u32, bool> = HashMap::new(); // type_id -> true if bool
    let mut type_int_width: HashMap<u32, (u32, bool)> = HashMap::new(); // type_id -> (width, signed)

    // Collect constant scalar values: result_id -> value (as u64).
    let mut constants: HashMap<u32, u64> = HashMap::new();
    // Track which IDs are bool-typed.
    let mut bool_ids: HashMap<u32, bool> = HashMap::new();

    // First pass: collect types and constant values.
    let mut pos: usize = 5;
    let mut spec_op_count: usize = 0;
    let mut spec_const_count: usize = 0;
    let mut spec_op_details: Vec<String> = Vec::new();
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;

        if wc == 0 || pos + wc > total_words {
            break;
        }

        match op {
            OP_TYPE_BOOL => {
                if wc >= 2 {
                    let id = read_word(bytes, pos + 1);
                    type_bool.insert(id, true);
                }
            }
            OP_TYPE_INT => {
                if wc >= 4 {
                    let id = read_word(bytes, pos + 1);
                    let width = read_word(bytes, pos + 2);
                    let signed = read_word(bytes, pos + 3) != 0;
                    type_int_width.insert(id, (width, signed));
                }
            }
            OP_CONSTANT | OP_SPEC_CONSTANT => {
                if wc >= 4 {
                    let type_id = read_word(bytes, pos + 1);
                    let result_id = read_word(bytes, pos + 2);
                    let val = read_word(bytes, pos + 3) as u64;
                    // For 64-bit constants, also grab the high word.
                    let val = if wc >= 5 {
                        val | ((read_word(bytes, pos + 4) as u64) << 32)
                    } else {
                        val
                    };
                    constants.insert(result_id, val);
                    if type_bool.contains_key(&type_id) {
                        bool_ids.insert(result_id, true);
                    }
                }
            }
            OP_CONSTANT_TRUE | OP_SPEC_CONSTANT_TRUE => {
                if wc >= 3 {
                    let result_id = read_word(bytes, pos + 2);
                    constants.insert(result_id, 1);
                    bool_ids.insert(result_id, true);
                }
            }
            OP_CONSTANT_FALSE | OP_SPEC_CONSTANT_FALSE => {
                if wc >= 3 {
                    let result_id = read_word(bytes, pos + 2);
                    constants.insert(result_id, 0);
                    bool_ids.insert(result_id, true);
                }
            }
            OP_SPEC_CONSTANT_OP => {
                if wc >= 4 {
                    let type_id = read_word(bytes, pos + 1);
                    let result_id = read_word(bytes, pos + 2);
                    let spec_op = read_word(bytes, pos + 3);
                    spec_op_count += 1;

                    let operands: Vec<u64> = (4..wc)
                        .map(|i| {
                            let id = read_word(bytes, pos + i);
                            *constants.get(&id).unwrap_or(&0)
                        })
                        .collect();

                    let val = eval_spec_op(spec_op, &operands);
                    constants.insert(result_id, val);
                    spec_op_details.push(format!("id={} op={} val={}", result_id, spec_op, val));
                    if type_bool.contains_key(&type_id) {
                        bool_ids.insert(result_id, true);
                    }
                }
            }
            OP_SPEC_CONSTANT | OP_SPEC_CONSTANT_TRUE | OP_SPEC_CONSTANT_FALSE | OP_SPEC_CONSTANT_COMPOSITE => {
                spec_const_count += 1;
                // Also collect values for OpSpecConstant (already handled above for CONSTANT|SPEC_CONSTANT combined match)
            }
            _ => {}
        }
        pos += wc;
    }

    // Second pass: rewrite, replacing OpSpecConstantOp with OpConstant.
    if spec_op_count > 0 || spec_const_count > 0 {
        log(&format!("[FREEZE] {} OpSpecConstantOp, {} OpSpecConstant* found in {} bytes. Details: {:?}",
            spec_op_count, spec_const_count, bytes.len(), &spec_op_details[..spec_op_details.len().min(10)]));
    }
    let mut out = Vec::with_capacity(bytes.len());
    // Copy header (5 words).
    out.extend_from_slice(&bytes[..20]);

    pos = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;

        if wc == 0 || pos + wc > total_words {
            break;
        }

        if op == OP_DECORATE && wc >= 3 && read_word(bytes, pos + 2) == 1 {
            // Strip OpDecorate ... SpecId decorations — not valid after freezing.
            pos += wc;
            continue;
        } else if op == OP_SPEC_CONSTANT_OP {
            // Replace OpSpecConstantOp with evaluated OpConstant.
            let type_id = read_word(bytes, pos + 1);
            let result_id = read_word(bytes, pos + 2);
            let val = *constants.get(&result_id).unwrap_or(&0);

            if bool_ids.contains_key(&result_id) || type_bool.contains_key(&type_id) {
                let bool_op = if val != 0 { OP_CONSTANT_TRUE } else { OP_CONSTANT_FALSE };
                push_word(&mut out, (3u32 << 16) | bool_op as u32);
                push_word(&mut out, type_id);
                push_word(&mut out, result_id);
            } else {
                push_word(&mut out, (4u32 << 16) | OP_CONSTANT as u32);
                push_word(&mut out, type_id);
                push_word(&mut out, result_id);
                push_word(&mut out, val as u32);
            }
        } else if op == OP_SPEC_CONSTANT_TRUE {
            // Rewrite as OpConstantTrue (same layout, different opcode).
            push_word(&mut out, (wc as u32) << 16 | OP_CONSTANT_TRUE as u32);
            for i in 1..wc {
                push_word(&mut out, read_word(bytes, pos + i));
            }
        } else if op == OP_SPEC_CONSTANT_FALSE {
            push_word(&mut out, (wc as u32) << 16 | OP_CONSTANT_FALSE as u32);
            for i in 1..wc {
                push_word(&mut out, read_word(bytes, pos + i));
            }
        } else if op == OP_SPEC_CONSTANT {
            push_word(&mut out, (wc as u32) << 16 | OP_CONSTANT as u32);
            for i in 1..wc {
                push_word(&mut out, read_word(bytes, pos + i));
            }
        } else if op == OP_SPEC_CONSTANT_COMPOSITE {
            push_word(&mut out, (wc as u32) << 16 | OP_CONSTANT_COMPOSITE as u32);
            for i in 1..wc {
                push_word(&mut out, read_word(bytes, pos + i));
            }
        } else {
            // Copy instruction as-is.
            let byte_start = pos * 4;
            let byte_end = (pos + wc) * 4;
            out.extend_from_slice(&bytes[byte_start..byte_end]);
        }

        pos += wc;
    }

    out
}

/// Evaluate a SPIR-V specialization constant operation.
fn eval_spec_op(opcode: u32, operands: &[u64]) -> u64 {
    let a = || operands.get(0).copied().unwrap_or(0);
    let b = || operands.get(1).copied().unwrap_or(0);
    let c = || operands.get(2).copied().unwrap_or(0);

    match opcode {
        // Integer arithmetic.
        126 => (-(a() as i32)) as u64,          // SNegate
        128 => a().wrapping_add(b()),            // IAdd
        130 => a().wrapping_sub(b()),            // ISub
        132 => a().wrapping_mul(b()),            // IMul
        134 => if b() != 0 { a() / b() } else { 0 }, // UDiv
        135 => {                                  // SDiv
            let d = b() as i32;
            if d != 0 { ((a() as i32) / d) as u64 } else { 0 }
        }
        137 => if b() != 0 { a() % b() } else { 0 }, // UMod
        // Logical.
        164 => (a() == b()) as u64,              // LogicalEqual
        165 => (a() != b()) as u64,              // LogicalNotEqual
        166 => ((a() != 0) || (b() != 0)) as u64, // LogicalOr
        167 => ((a() != 0) && (b() != 0)) as u64, // LogicalAnd
        168 => (a() == 0) as u64,                // LogicalNot
        // Select: condition, true_val, false_val.
        169 => if a() != 0 { b() } else { c() }, // Select
        // Integer comparison.
        170 => (a() == b()) as u64,              // IEqual
        171 => (a() != b()) as u64,              // INotEqual
        172 => ((a() as u32) > (b() as u32)) as u64,  // UGreaterThan
        173 => ((a() as i32) > (b() as i32)) as u64,  // SGreaterThan
        174 => ((a() as u32) >= (b() as u32)) as u64, // UGreaterThanEqual
        175 => ((a() as i32) >= (b() as i32)) as u64, // SGreaterThanEqual
        176 => ((a() as u32) < (b() as u32)) as u64,  // ULessThan
        177 => ((a() as i32) < (b() as i32)) as u64,  // SLessThan
        178 => ((a() as u32) <= (b() as u32)) as u64, // ULessThanEqual
        179 => ((a() as i32) <= (b() as i32)) as u64, // SLessThanEqual
        // Bitwise.
        194 => (a() as u32).wrapping_shr(b() as u32) as u64, // ShiftRightLogical
        195 => ((a() as i32).wrapping_shr(b() as u32)) as u64, // ShiftRightArithmetic
        196 => (a() as u32).wrapping_shl(b() as u32) as u64, // ShiftLeftLogical
        197 => a() | b(),                        // BitwiseOr
        198 => a() ^ b(),                        // BitwiseXor
        199 => a() & b(),                        // BitwiseAnd
        200 => !(a() as u32) as u64,             // Not
        // Composite.
        81 => a(),                               // CompositeExtract (return first operand)
        // Conversion (values unchanged for const-eval of integers).
        109 | 110 | 111 | 112 | 113 | 114 => a(), // ConvertF/S/U, UConvert, SConvert
        // Default: return 0 for unhandled operations.
        _ => 0,
    }
}

// SPIR-V: StorageClass values.
const SC_UNIFORM: u32 = 2;
const SC_STORAGE_BUFFER: u32 = 12;
const SC_PUSH_CONSTANT: u32 = 9;
// SPIR-V: NonWritable decoration value.
const DECO_NON_WRITABLE: u32 = 24;
// SPIR-V: Binding=33, DescriptorSet=34  (per SPIR-V spec unified1 Table "Decoration").
const DECO_BINDING: u32 = 33;
const DECO_DESCRIPTOR_SET: u32 = 34;
// SPIR-V: OpVariable opcode.
const OP_VARIABLE: u16 = 59;

// Binding slot used by the PC ring buffer emulation inside group 3.
// Must be high enough to never collide with split combined-sampler bindings.
// Godot material shaders (set=3) typically use original bindings 0-20; after
// doubling the max is ~40. 120 gives safe headroom. Must match the C++ constant
// PUSH_CONSTANT_RING_BINDING in rendering_device_driver_webgpu.cpp.
const PC_RING_BUFFER_BINDING: u32 = 120;

/// Infer which storage buffer variables are read-only (never the target of an OpStore)
/// and add `OpDecorate <var_id> NonWritable` decorations for them.
///
/// glslang does NOT emit OpDecorate NonWritable for `restrict readonly buffer` blocks,
/// so NAGA defaults all storage buffers to `var<storage, read_write>`. This causes
/// writable-aliasing validation errors when the same placeholder buffer is bound in
/// multiple writable slots across different bind groups in a single pass (e.g. the
/// skeleton compute shader with blend shapes disabled).
///
/// By adding NonWritable here, NAGA emits `var<storage>` for truly read-only SSBOs,
/// which WebGPU allows to alias freely (ReadOnlyStorage bindings share buffers freely).
pub fn infer_readonly_storage(bytes: &[u8]) -> Vec<u8> {
    let len = bytes.len();
    if len < 20 || len % 4 != 0 {
        return bytes.to_vec();
    }
    let nwords = len / 4;

    // Pass 1: Collect all OpVariable in StorageBuffer class (storage_class == 12).
    let mut storage_vars: HashSet<u32> = HashSet::new();
    {
        let mut pos = 5usize;
        while pos < nwords {
            let word0 = read_word(bytes, pos);
            let wc = ((word0 >> 16) & 0xFFFF) as usize;
            let op = (word0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > nwords {
                break;
            }
            if op == OP_VARIABLE && wc >= 4 {
                let result_id = read_word(bytes, pos + 2);
                let storage_class = read_word(bytes, pos + 3);
                if storage_class == SC_STORAGE_BUFFER {
                    storage_vars.insert(result_id);
                }
            }
            pos += wc;
        }
    }

    if storage_vars.is_empty() {
        return bytes.to_vec();
    }

    // Pass 2 (multi-iteration for access-chain chaining):
    // Track result IDs derived from storage buffer vars via OpAccessChain,
    // then find which storage vars are written to via OpStore.
    let mut ptr_to_root: HashMap<u32, u32> = HashMap::new();
    let mut written_vars: HashSet<u32> = HashSet::new();

    for _iter in 0..3 {
        let mut pos = 5usize;
        while pos < nwords {
            let word0 = read_word(bytes, pos);
            let wc = ((word0 >> 16) & 0xFFFF) as usize;
            let op = (word0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > nwords {
                break;
            }
            match op {
                // OpAccessChain=65, OpInBoundsAccessChain=66: result_type result_id base indices...
                65 | 66 if wc >= 4 => {
                    let result_id = read_word(bytes, pos + 2);
                    let base_id = read_word(bytes, pos + 3);
                    if storage_vars.contains(&base_id) {
                        ptr_to_root.insert(result_id, base_id);
                    } else if let Some(&root) = ptr_to_root.get(&base_id) {
                        ptr_to_root.insert(result_id, root);
                    }
                }
                // OpPtrAccessChain=67, OpInBoundsPtrAccessChain=216: result_type result_id base element indices...
                67 | 216 if wc >= 5 => {
                    let result_id = read_word(bytes, pos + 2);
                    let base_id = read_word(bytes, pos + 3);
                    if storage_vars.contains(&base_id) {
                        ptr_to_root.insert(result_id, base_id);
                    } else if let Some(&root) = ptr_to_root.get(&base_id) {
                        ptr_to_root.insert(result_id, root);
                    }
                }
                // OpStore=62: pointer value [memory_access]
                62 if wc >= 3 => {
                    let ptr_id = read_word(bytes, pos + 1);
                    if storage_vars.contains(&ptr_id) {
                        written_vars.insert(ptr_id);
                    } else if let Some(&root) = ptr_to_root.get(&ptr_id) {
                        written_vars.insert(root);
                    }
                }
                // OpAtomicStore=228: pointer scope semantics value — pointer at pos+1
                228 if wc >= 5 => {
                    let ptr_id = read_word(bytes, pos + 1);
                    if storage_vars.contains(&ptr_id) {
                        written_vars.insert(ptr_id);
                    } else if let Some(&root) = ptr_to_root.get(&ptr_id) {
                        written_vars.insert(root);
                    }
                }
                // OpAtomicExchange=229..OpAtomicXor=242, OpAtomicCompareExchange=230/231:
                // format: result_type result_id pointer scope ... — pointer at pos+3
                229..=242 if wc >= 6 => {
                    let ptr_id = read_word(bytes, pos + 3);
                    if storage_vars.contains(&ptr_id) {
                        written_vars.insert(ptr_id);
                    } else if let Some(&root) = ptr_to_root.get(&ptr_id) {
                        written_vars.insert(root);
                    }
                }
                // OpCopyMemory=38: target source [access] — target at pos+1
                38 if wc >= 3 => {
                    let ptr_id = read_word(bytes, pos + 1);
                    if storage_vars.contains(&ptr_id) {
                        written_vars.insert(ptr_id);
                    } else if let Some(&root) = ptr_to_root.get(&ptr_id) {
                        written_vars.insert(root);
                    }
                }
                // OpCopyMemorySized=39: target source size [access] — target at pos+1
                39 if wc >= 4 => {
                    let ptr_id = read_word(bytes, pos + 1);
                    if storage_vars.contains(&ptr_id) {
                        written_vars.insert(ptr_id);
                    } else if let Some(&root) = ptr_to_root.get(&ptr_id) {
                        written_vars.insert(root);
                    }
                }
                // OpFunctionCall=57: result_type result_id function arg0 arg1 ...
                // If a storage var (or pointer derived from one) is passed as an
                // argument, conservatively mark it as writable (called fn may write).
                57 if wc >= 4 => {
                    for arg_pos in (pos + 4)..pos + wc {
                        let arg_id = read_word(bytes, arg_pos);
                        if storage_vars.contains(&arg_id) {
                            written_vars.insert(arg_id);
                        } else if let Some(&root) = ptr_to_root.get(&arg_id) {
                            written_vars.insert(root);
                        }
                    }
                }
                _ => {}
            }
            pos += wc;
        }
    }

    // Pass 3: Find storage vars that already carry NonWritable (don't duplicate).
    let mut already_nonwritable: HashSet<u32> = HashSet::new();
    {
        let mut pos = 5usize;
        while pos < nwords {
            let word0 = read_word(bytes, pos);
            let wc = ((word0 >> 16) & 0xFFFF) as usize;
            let op = (word0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > nwords {
                break;
            }
            if op == OP_DECORATE as u16 && wc >= 3 {
                let target = read_word(bytes, pos + 1);
                let deco = read_word(bytes, pos + 2);
                if deco == DECO_NON_WRITABLE {
                    already_nonwritable.insert(target);
                }
            }
            pos += wc;
        }
    }

    // Vars that are read-only and not yet decorated with NonWritable.
    let to_add: Vec<u32> = storage_vars
        .iter()
        .filter(|id| !written_vars.contains(id) && !already_nonwritable.contains(id))
        .copied()
        .collect();

    if to_add.is_empty() {
        return bytes.to_vec();
    }

    log(&format!(
        "[INFER-RO] Adding NonWritable to {} inferred-readonly storage buffer vars: {:?}",
        to_add.len(),
        to_add
    ));

    // Build output: insert OpDecorate NonWritable before the first annotation instruction
    // (OpDecorate=71, OpMemberDecorate=72, or OpDecorationGroup=73).
    let mut out: Vec<u8> = Vec::with_capacity(len + to_add.len() * 12);
    out.extend_from_slice(&bytes[0..20]); // copy 5-word SPIR-V header

    let mut inserted = false;
    let mut pos = 5usize;
    while pos < nwords {
        let word0 = read_word(bytes, pos);
        let wc = ((word0 >> 16) & 0xFFFF) as usize;
        let op = (word0 & 0xFFFF) as u16;
        if wc == 0 {
            break;
        }
        if pos + wc > nwords {
            out.extend_from_slice(&bytes[pos * 4..]);
            break;
        }
        if !inserted && (op == 71 || op == 72 || op == 73) {
            for &var_id in &to_add {
                // OpDecorate <var_id> NonWritable: wordcount=3, opcode=71
                push_word(&mut out, (3u32 << 16) | 71u32);
                push_word(&mut out, var_id);
                push_word(&mut out, DECO_NON_WRITABLE);
            }
            inserted = true;
        }
        for i in 0..wc {
            push_word(&mut out, read_word(bytes, pos + i));
        }
        pos += wc;
    }

    if !inserted {
        // Fallback: append at end (should not happen in valid SPIR-V).
        for &var_id in &to_add {
            push_word(&mut out, (3u32 << 16) | 71u32);
            push_word(&mut out, var_id);
            push_word(&mut out, DECO_NON_WRITABLE);
        }
    }

    out
}

/// Remap SPIR-V push-constant variables to uniform buffer bindings.
///
/// Naga v28 silently drops `PushConstant` variables from the WGSL output.
/// This makes push-constant data inaccessible in the generated shader.
/// We fix this by converting push-constant variables to regular uniform buffers
/// at descriptor set 3, binding 120 (the ring-buffer slot used by the C++ backend).
///
/// The struct type already carries `Block` + `Offset` decorations from GLSL
/// compilation, so it is already valid as a UBO in SPIR-V / WGSL.
pub fn convert_push_constants_to_uniforms(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }

    // Pass 1: find all OpVariable IDs with StorageClass == PushConstant,
    // and also collect their result_type_id (which is an OpTypePointer).
    // We need to rewrite both the variable AND its pointer type.
    let mut pc_var_ids: Vec<u32> = Vec::new();
    let mut pc_ptr_type_ids: Vec<u32> = Vec::new(); // OpTypePointer IDs to rewrite
    let mut pos: usize = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        if op == OP_VARIABLE && wc >= 4 {
            let type_id  = read_word(bytes, pos + 1); // result_type_id (an OpTypePointer)
            let result_id = read_word(bytes, pos + 2);
            let sc = read_word(bytes, pos + 3);
            if sc == SC_PUSH_CONSTANT {
                pc_var_ids.push(result_id);
                if !pc_ptr_type_ids.contains(&type_id) {
                    pc_ptr_type_ids.push(type_id);
                }
            }
        }
        pos += wc;
    }

    if pc_var_ids.is_empty() {
        return bytes.to_vec();
    }

    log(&format!("[PCFIX] Found {} push-constant variable(s), remapping to @group(3) @binding({PC_RING_BUFFER_BINDING}) read-only storage",
        pc_var_ids.len()));

    // Build decode set: which var IDs already have DescriptorSet / Binding decorations
    // (they shouldn't for push constants, but be safe).
    let mut has_set: std::collections::HashSet<u32> = std::collections::HashSet::new();
    let mut has_binding: std::collections::HashSet<u32> = std::collections::HashSet::new();
    pos = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == 71 /* OpDecorate */ && wc >= 3 {
            let target = read_word(bytes, pos + 1);
            let deco = read_word(bytes, pos + 2);
            if pc_var_ids.contains(&target) {
                if deco == DECO_DESCRIPTOR_SET { has_set.insert(target); }
                if deco == DECO_BINDING { has_binding.insert(target); }
            }
        }
        pos += wc;
    }

    // Pass 2: rewrite SPIR-V.
    //   • Inject new OpDecorate {DescriptorSet=3} and {Binding=PC_RING_BUFFER_BINDING} for each PC var
    //     just before the first type-declaration instruction (opcode 19–32).
    //   • Remove any pre-existing DescriptorSet/Binding decorations on PC vars
    //     (push constants don't normally have them, but guard against this).
    //   • Change StorageClass of OpVariable and its pointer type from PushConstant to Uniform.
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len() + pc_var_ids.len() * 32);
    out.extend_from_slice(&bytes[..20]); // Copy 5-word SPIR-V header.

    let mut injected_decorations = false;

    pos = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        // Injection point: before the first type instruction (opcode 19–32).
        if !injected_decorations && op >= 19 && op <= 32 {
            injected_decorations = true;
            for &var_id in &pc_var_ids {
                // OpDecorate <var_id> DescriptorSet 3
                push_word(&mut out, (4u32 << 16) | 71);
                push_word(&mut out, var_id);
                push_word(&mut out, DECO_DESCRIPTOR_SET);
                push_word(&mut out, 3);
                // OpDecorate <var_id> Binding <PC_RING_BUFFER_BINDING>
                push_word(&mut out, (4u32 << 16) | 71);
                push_word(&mut out, var_id);
                push_word(&mut out, DECO_BINDING);
                push_word(&mut out, PC_RING_BUFFER_BINDING);
                // OpDecorate <var_id> NonWritable — results in var<storage, read> in WGSL
                push_word(&mut out, (3u32 << 16) | 71);
                push_word(&mut out, var_id);
                push_word(&mut out, DECO_NON_WRITABLE);
            }
        }

        // Remove any pre-existing DescriptorSet / Binding decorations on PC vars
        // to avoid duplicates that would conflict with the injected ones above.
        if op == 71 /* OpDecorate */ && wc >= 3 {
            let target = read_word(bytes, pos + 1);
            if pc_var_ids.contains(&target) && wc >= 3 {
                let deco = read_word(bytes, pos + 2);
                if deco == DECO_DESCRIPTOR_SET || deco == DECO_BINDING {
                    pos += wc;
                    continue; // skip
                }
            }
        }

        // Rewrite OpTypePointer PushConstant → StorageBuffer for any pointer type
        // used by a push-constant variable (storage class must match the variable).
        if op == 32 /* OpTypePointer */ && wc == 4 {
            let ptr_result_id = read_word(bytes, pos + 1);
            let ptr_sc        = read_word(bytes, pos + 2);
            if ptr_sc == SC_PUSH_CONSTANT && pc_ptr_type_ids.contains(&ptr_result_id) {
                push_word(&mut out, (4u32 << 16) | 32);
                push_word(&mut out, ptr_result_id);
                push_word(&mut out, SC_STORAGE_BUFFER); // changed storage class
                push_word(&mut out, read_word(bytes, pos + 3)); // pointee type_id
                pos += wc;
                continue;
            }
        }

        if op == OP_VARIABLE && wc >= 4 {
            let result_id = read_word(bytes, pos + 2);
            if pc_var_ids.contains(&result_id) {
                // Emit OpVariable with StorageClass changed to StorageBuffer.
                push_word(&mut out, (wc as u32) << 16 | OP_VARIABLE as u32);
                push_word(&mut out, read_word(bytes, pos + 1)); // type_id
                push_word(&mut out, result_id);
                push_word(&mut out, SC_STORAGE_BUFFER); // changed!
                for i in 4..wc {
                    push_word(&mut out, read_word(bytes, pos + i));
                }
                pos += wc;
                continue;
            }
        }

        // Copy instruction as-is.
        out.extend_from_slice(&bytes[pos * 4..(pos + wc) * 4]);
        pos += wc;
    }

    out
}

/// Rewrite OpCopyLogical (op 400) to OpCopyObject (op 83).
///
/// OpCopyLogical is a SPIR-V 1.4+ instruction for copying between structs that are
/// logically equivalent but have different decorations. Naga doesn't support it,
/// but OpCopyObject is semantically equivalent for our purposes (the types have
/// identical memory layout; only decorations differ).
pub fn rewrite_copy_logical(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }

    const OP_COPY_LOGICAL: u16 = 400;
    const OP_COPY_OBJECT: u16 = 83;

    // Quick scan: if no CopyLogical present, return as-is.
    let mut found = false;
    let mut pos = 5usize;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == OP_COPY_LOGICAL {
            found = true;
            break;
        }
        pos += wc;
    }

    if !found {
        return bytes.to_vec();
    }

    // Rewrite: replace OpCopyLogical with OpCopyObject (same word count and layout).
    let mut out = bytes.to_vec();
    let mut count = 0u32;
    pos = 5;
    while pos < total_words {
        let w0 = read_word(&out, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == OP_COPY_LOGICAL {
            // Replace opcode in-place: keep word count, change opcode to CopyObject.
            let new_w0 = ((wc as u32) << 16) | (OP_COPY_OBJECT as u32);
            let off = pos * 4;
            out[off..off + 4].copy_from_slice(&new_w0.to_le_bytes());
            count += 1;
        }
        pos += wc;
    }

    if count > 0 {
        log(&format!("[REWRITE] Replaced {} OpCopyLogical → OpCopyObject", count));
    }

    out
}

/// Rewrite OpTerminateInvocation (op 4416) to OpKill (op 252).
///
/// OpTerminateInvocation is from SPV_KHR_terminate_invocation and behaves like
/// OpKill but with defined helper-invocation semantics. Naga doesn't support it.
/// OpKill is the SPIR-V 1.0 equivalent and naga handles it correctly.
pub fn rewrite_terminate_invocation(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }

    const OP_TERMINATE_INVOCATION: u16 = 4416;
    const OP_KILL: u16 = 252;

    // Quick scan: if no TerminateInvocation present, return as-is.
    let mut found = false;
    let mut pos = 5usize;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == OP_TERMINATE_INVOCATION {
            found = true;
            break;
        }
        pos += wc;
    }

    if !found {
        return bytes.to_vec();
    }

    // Rewrite in-place.
    let mut out = bytes.to_vec();
    let mut count = 0u32;
    pos = 5;
    while pos < total_words {
        let w0 = read_word(&out, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == OP_TERMINATE_INVOCATION {
            // Both OpKill and OpTerminateInvocation have word count 1, but be safe.
            let new_w0 = ((wc as u32) << 16) | (OP_KILL as u32);
            let off = pos * 4;
            out[off..off + 4].copy_from_slice(&new_w0.to_le_bytes());
            count += 1;
        }
        pos += wc;
    }

    if count > 0 {
        log(&format!("[REWRITE] Replaced {} OpTerminateInvocation → OpKill", count));
    }

    out
}

/// Split combined image sampler variables into separate image + sampler.
/// Naga's SPIR-V frontend doesn't handle OpLoad of combined image sampler
/// variables — it expects separate image/sampler loads followed by OpSampledImage.
/// This function rewrites the SPIR-V to use that pattern.
///
/// Key design decisions:
///   - The original combined var is KEPT as the image var (just its binding is changed).
///     This is critical: if it were removed, any OpFunctionCall passing it as an argument
///     would fail naga's lookup with InvalidId.
///   - Only a new sampler var is added per combined sampler.
///   - OpLoad from combined vars is rewritten everywhere (global loads AND loads from
///     function parameters that received combined vars at their call sites).
///
/// Binding convention (matching Godot's WebGPU uniform_set_create):
///   Original combined sampler at binding N →
///     Sampler at binding N*2+0, Image at binding N*2+1 (repurposed original var)
pub fn split_combined_samplers(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }

    // --- First pass: collect type info and combined sampler variables ---
    // sampled_image_type_id → image_type_id
    let mut sampled_image_types: HashMap<u32, u32> = HashMap::new();
    // image_type_id → (dim, arrayed) from OpTypeImage for logging
    let mut image_type_info: HashMap<u32, (u32, u32)> = HashMap::new();
    // pointer_type_id → base_type_id (for UniformConstant pointers)
    let mut uc_pointer_types: HashMap<u32, u32> = HashMap::new();
    // variable_id → pointer_type_id (for UniformConstant variables)
    let mut uc_variables: HashMap<u32, u32> = HashMap::new();
    // variable_id → (set, binding) decorations
    let mut var_bindings: HashMap<u32, (u32, u32)> = HashMap::new();
    // Track all decorations: (id, decoration) pairs for set/binding
    let mut var_sets: HashMap<u32, u32> = HashMap::new();
    let mut var_bind_nums: HashMap<u32, u32> = HashMap::new();
    // Sampler type ID (if any exists)
    let mut existing_sampler_type: Option<u32> = None;
    // Current max ID for allocating new IDs
    let bound = read_word(bytes, 3);
    // combined SampledImage pointer type IDs → image_type_id (for function param detection)
    let mut combined_ptr_types: HashMap<u32, u32> = HashMap::new(); // ptr_type_id → image_type_id

    let mut pos: usize = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        match op {
            25 => { // OpTypeImage: wc >= 9: [wc|25, id, sampled_type, dim, depth, arrayed, ms, sampled, format]
                if wc >= 9 {
                    let id = read_word(bytes, pos + 1);
                    let dim = read_word(bytes, pos + 3);
                    let arrayed = read_word(bytes, pos + 5);
                    image_type_info.insert(id, (dim, arrayed));
                }
            }
            27 => { // OpTypeSampledImage
                if wc >= 3 {
                    let id = read_word(bytes, pos + 1);
                    let image_id = read_word(bytes, pos + 2);
                    sampled_image_types.insert(id, image_id);
                }
            }
            26 => { // OpTypeSampler
                if wc >= 2 {
                    existing_sampler_type = Some(read_word(bytes, pos + 1));
                }
            }
            32 => { // OpTypePointer
                if wc >= 4 {
                    let id = read_word(bytes, pos + 1);
                    let sc = read_word(bytes, pos + 2);
                    let base = read_word(bytes, pos + 3);
                    if sc == 0 { // UniformConstant
                        uc_pointer_types.insert(id, base);
                    }
                }
            }
            59 => { // OpVariable
                if wc >= 4 {
                    let type_id = read_word(bytes, pos + 1);
                    let id = read_word(bytes, pos + 2);
                    let sc = read_word(bytes, pos + 3);
                    if sc == 0 { // UniformConstant
                        uc_variables.insert(id, type_id);
                    }
                }
            }
            71 => { // OpDecorate
                if wc >= 4 {
                    let target = read_word(bytes, pos + 1);
                    let decoration = read_word(bytes, pos + 2);
                    let value = read_word(bytes, pos + 3);
                    match decoration {
                        34 => { var_sets.insert(target, value); } // DescriptorSet
                        33 => { var_bind_nums.insert(target, value); } // Binding
                        _ => {}
                    }
                }
            }
            _ => {}
        }
        pos += wc;
    }

    // Build binding map: var_id → (set, binding)
    for (&var_id, _) in &uc_variables {
        if let (Some(&set), Some(&binding)) = (var_sets.get(&var_id), var_bind_nums.get(&var_id)) {
            var_bindings.insert(var_id, (set, binding));
        }
    }

    // Identify which variables are combined image samplers.
    // combined_var_id → (image_type_id, sampled_image_type_id, set, binding)
    let mut combined_vars: HashMap<u32, (u32, u32, u32, u32)> = HashMap::new();
    for (&var_id, &ptr_type_id) in &uc_variables {
        if let Some(&base_type_id) = uc_pointer_types.get(&ptr_type_id) {
            if let Some(&image_type_id) = sampled_image_types.get(&base_type_id) {
                if let Some(&(set, binding)) = var_bindings.get(&var_id) {
                    combined_vars.insert(var_id, (image_type_id, base_type_id, set, binding));
                    combined_ptr_types.insert(ptr_type_id, image_type_id);
                }
            }
        }
    }

    if !combined_vars.is_empty() {
        log(&format!("[SPLIT] Found {} combined image sampler variables", combined_vars.len()));
    }

    // --- Allocate new IDs ---
    let mut next_id = bound;
    let mut alloc_id = || { let id = next_id; next_id = next_id.wrapping_add(1); id };

    // For each combined var, we reuse the original var as the image var (just change its binding
    // and type pointer). We allocate NEW IDs for:
    //   image_ptr_type_id: OpTypePointer UniformConstant image_type_id (correct type for image var)
    //   sampler_ptr_type_id: OpTypePointer UniformConstant sampler_type_id
    //   sampler_var_id: the new separate sampler variable
    struct SplitInfo {
        image_type_id: u32,
        sampled_image_type_id: u32,
        original_ptr_type_id: u32, // the old _ptr_UC_SampledImage type (kept in module but unused)
        image_ptr_type_id: u32,    // NEW: ptr_to_image_type so NAGA emits texture_*<f32> correctly
        sampler_ptr_type_id: u32,  // newly allocated
        image_var_id: u32,         // = original combined_var_id (reused!)
        sampler_var_id: u32,       // newly allocated
        set: u32,
        binding: u32,
    }
    let sampler_type_id = if !combined_vars.is_empty() {
        existing_sampler_type.unwrap_or_else(&mut alloc_id)
    } else {
        0 // Unused.
    };
    let need_sampler_type = !combined_vars.is_empty() && existing_sampler_type.is_none();

    let mut splits: HashMap<u32, SplitInfo> = HashMap::new();
    // Pre-compute image_ptr_type_id per unique image_type_id.
    // Multiple combined vars can share the same image type — they must share the SAME
    // image_ptr_type_id so that only ONE OpTypePointer needs to be injected.
    let mut image_ptr_type_map: HashMap<u32, u32> = HashMap::new(); // image_type_id → image_ptr_type_id
    for (_, &(image_type_id, _, _, _)) in &combined_vars {
        image_ptr_type_map.entry(image_type_id).or_insert_with(&mut alloc_id);
    }
    for (&var_id, &(image_type_id, sampled_image_type_id, set, binding)) in &combined_vars {
        let original_ptr_type_id = uc_variables[&var_id]; // the existing pointer type for this var
        // Log image dimension info for diagnostics.
        if let Some(&(dim, arrayed)) = image_type_info.get(&image_type_id) {
            let dim_name = match dim {
                0 => "1D", 1 => "2D", 2 => "3D", 3 => "Cube", 4 => "Rect", 5 => "Buffer",
                6 => "SubpassData", _ => "?",
            };
            log(&format!("[SPLIT-DIM] set={} binding={} dim={dim_name} arrayed={}", set, binding, arrayed));
        }
        splits.insert(var_id, SplitInfo {
            image_type_id,
            sampled_image_type_id,
            original_ptr_type_id,
            image_ptr_type_id: image_ptr_type_map[&image_type_id], // SHARED per image_type_id
            sampler_ptr_type_id: alloc_id(),
            image_var_id: var_id, // REUSE the combined var ID as the image var
            sampler_var_id: alloc_id(),
            set,
            binding,
        });
    }

    // --- Collect function parameters that receive combined vars at call sites ---
    // (func_id, param_index) → combined_var_id
    let mut call_combined_args: HashMap<(u32, u32), u32> = HashMap::new();
    // func_id → Vec<param_id> (in order of declaration)
    let mut function_param_ids: HashMap<u32, Vec<u32>> = HashMap::new();

    {
        let mut cur_func: u32 = 0;
        let mut cur_params: Vec<u32> = Vec::new();
        let mut pos2 = 5usize;
        while pos2 < total_words {
            let w0 = read_word(bytes, pos2);
            let wc2 = (w0 >> 16) as usize;
            let op2 = (w0 & 0xFFFF) as u16;
            if wc2 == 0 || pos2 + wc2 > total_words { break; }
            match op2 {
                54 => { // OpFunction
                    if cur_func != 0 {
                        function_param_ids.insert(cur_func, cur_params.clone());
                    }
                    cur_func = read_word(bytes, pos2 + 2); // result_id
                    cur_params = Vec::new();
                }
                55 => { // OpFunctionParameter
                    if wc2 >= 3 {
                        cur_params.push(read_word(bytes, pos2 + 2)); // result_id
                    }
                }
                56 => { // OpFunctionEnd
                    if cur_func != 0 {
                        function_param_ids.insert(cur_func, cur_params.clone());
                        cur_func = 0;
                        cur_params = Vec::new();
                    }
                }
                57 => { // OpFunctionCall
                    // op=57, wc>=4: [wc|57, result_type, result_id, func_id, args...]
                    if wc2 >= 4 {
                        let func_id = read_word(bytes, pos2 + 3);
                        for arg_idx in 0..(wc2 as u32 - 4) {
                            let arg_id = read_word(bytes, pos2 + 4 + arg_idx as usize);
                            if combined_vars.contains_key(&arg_id) {
                                call_combined_args.insert((func_id, arg_idx), arg_id);
                            }
                        }
                    }
                }
                _ => {}
            }
            pos2 += wc2;
        }
        if cur_func != 0 {
            function_param_ids.insert(cur_func, cur_params);
        }
    }

    // Build: param_id → combined_var_id (for function parameters that receive combined vars)
    let mut param_combined: HashMap<u32, u32> = HashMap::new();
    for (&(func_id, param_pos), &combined_var_id) in &call_combined_args {
        if let Some(params) = function_param_ids.get(&func_id) {
            if let Some(&param_id) = params.get(param_pos as usize) {
                param_combined.insert(param_id, combined_var_id);
            }
        }
    }
    if !param_combined.is_empty() {
        log(&format!("[SPLIT] Found {} function params receiving combined vars", param_combined.len()));
    }

    // Collect OpLoad result IDs that load from combined vars or combined-var function params.
    // load_result_id → combined_var_id
    let mut combined_loads: HashMap<u32, u32> = HashMap::new();
    pos = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }
        if op == 61 && wc >= 4 { // OpLoad
            let result_id = read_word(bytes, pos + 2);
            let pointer_id = read_word(bytes, pos + 3);
            if combined_vars.contains_key(&pointer_id) {
                combined_loads.insert(result_id, pointer_id);
            } else if let Some(&combined_var_id) = param_combined.get(&pointer_id) {
                combined_loads.insert(result_id, combined_var_id);
            }
        }
        pos += wc;
    }

    // Allocate IDs for split loads.
    // original_load_result_id → (image_load_id, sampler_load_id)
    let mut load_splits: HashMap<u32, (u32, u32)> = HashMap::new();
    for (&load_id, _) in &combined_loads {
        load_splits.insert(load_id, (alloc_id(), alloc_id()));
    }

    // --- Second pass: rewrite ---
    let mut out = Vec::with_capacity(bytes.len() + 256);
    // Copy header but update bound.
    out.extend_from_slice(&bytes[..12]);
    push_word(&mut out, next_id); // Updated bound
    push_word(&mut out, read_word(bytes, 4)); // Schema

    // Two injection points to respect SPIR-V section ordering:
    // 1. Decorations must come BEFORE type declarations (annotation section).
    // 2. Types/variables come BEFORE OpFunction (type-declaration section).
    // 3. image_ptr_type_id must be defined AFTER OpTypeImage (its base type) but
    //    BEFORE OpVariable that uses it — we inject it right after OpTypeImage is copied.
    let mut decorations_injected = false;
    let mut types_injected = false;
    // For each split, track whether its image_ptr_type_id has been injected yet.
    // (We inject it right after the OpTypeImage with result_id == split.image_type_id.)
    let mut image_ptr_injected: HashSet<u32> = HashSet::new(); // set of image_type_ids already injected

    pos = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        // Rewrite OpEntryPoint: keep combined var (now = image var), add sampler_var.
        if op == 15 { // OpEntryPoint
            let exec_model = read_word(bytes, pos + 1);
            let entry_id = read_word(bytes, pos + 2);
            let mut name_end = pos + 3;
            while name_end < pos + wc {
                let w = read_word(bytes, name_end);
                name_end += 1;
                if w & 0xFF == 0 || (w >> 8) & 0xFF == 0 || (w >> 16) & 0xFF == 0 || (w >> 24) & 0xFF == 0 {
                    break;
                }
            }
            // Keep combined var IDs (they are now image vars), but also add sampler_var IDs.
            let mut new_vars: Vec<u32> = Vec::new();
            for i in name_end..pos + wc {
                let var_id = read_word(bytes, i);
                new_vars.push(var_id); // keep everything (including combined/image var)
                if let Some(split) = splits.get(&var_id) {
                    new_vars.push(split.sampler_var_id); // add paired sampler var
                }
            }
            let new_wc = (name_end - pos) + new_vars.len();
            push_word(&mut out, ((new_wc as u32) << 16) | 15);
            push_word(&mut out, exec_model);
            push_word(&mut out, entry_id);
            for i in (pos + 3)..name_end {
                push_word(&mut out, read_word(bytes, i));
            }
            for v in &new_vars {
                push_word(&mut out, *v);
            }
            log(&format!("[SPLIT] OpEntryPoint rewritten: added {} sampler vars (total interface={})",
                splits.len(), new_vars.len()));
            pos += wc;
            continue;
        }

        // Inject decorations before the first type instruction (opcodes 19-33).
        if !decorations_injected && op >= 19 && op <= 33 {
            decorations_injected = true;
            for (&_var_id, split) in &splits {
                // Decorations for sampler var only (image var = combined var, handled below).
                push_word(&mut out, (4u32 << 16) | 71); // OpDecorate
                push_word(&mut out, split.sampler_var_id);
                push_word(&mut out, 34); // DescriptorSet
                push_word(&mut out, split.set);

                push_word(&mut out, (4u32 << 16) | 71); // OpDecorate
                push_word(&mut out, split.sampler_var_id);
                push_word(&mut out, 33); // Binding
                push_word(&mut out, split.binding.wrapping_mul(2));
            }
        }

        // Inject new types/variables before first OpFunction.
        if op == 54 && !types_injected { // OpFunction
            types_injected = true;
            // Emit OpTypeSampler if needed.
            if need_sampler_type {
                push_word(&mut out, (2u32 << 16) | 26); // OpTypeSampler, wc=2
                push_word(&mut out, sampler_type_id);
            }
            // For each split, emit the sampler pointer type + sampler variable.
            // (Image pointer type was already injected after OpTypeImage; image var was rewritten in-place.)
            for (&_var_id, split) in &splits {
                // OpTypePointer %sampler_ptr UniformConstant %sampler_type
                push_word(&mut out, (4u32 << 16) | 32);
                push_word(&mut out, split.sampler_ptr_type_id);
                push_word(&mut out, 0); // UniformConstant
                push_word(&mut out, sampler_type_id);

                // OpVariable %sampler_ptr %sampler_var UniformConstant
                push_word(&mut out, (4u32 << 16) | 59);
                push_word(&mut out, split.sampler_ptr_type_id);
                push_word(&mut out, split.sampler_var_id);
                push_word(&mut out, 0); // UniformConstant
            }
        }

        // Rewrite OpDecorate for combined sampler vars:
        // - Change Binding from original to binding*2+1 (image slot)
        // - Keep DescriptorSet unchanged
        // - Remove NonWritable (images are not NonWritable)
        // For all OTHER vars: double their Binding values to avoid collision.
        if op == 71 && wc >= 3 { // OpDecorate
            let target = read_word(bytes, pos + 1);
            let decoration = if wc >= 3 { read_word(bytes, pos + 2) } else { 0 };
            if combined_vars.contains_key(&target) {
                if decoration == 33 { // Binding → change to binding*2+1
                    let (_, _, _, binding) = combined_vars[&target];
                    push_word(&mut out, w0); // same wc|op
                    push_word(&mut out, target);
                    push_word(&mut out, 33); // Binding
                    push_word(&mut out, binding.wrapping_mul(2).wrapping_add(1));
                    pos += wc;
                    continue;
                } else if decoration == 24 { // NonWritable → remove
                    pos += wc;
                    continue;
                }
                // Keep DescriptorSet and all other decorations as-is.
            } else if decoration == 33 { // Binding for non-combined var → double
                let old_binding = if wc >= 4 { read_word(bytes, pos + 3) } else { 0 };
                // The PC ring-buffer emulation variable has binding = PC_RING_BUFFER_BINDING.
                // convert_push_constants_to_uniforms assigned this high fixed value so the
                // C++ can inject it directly without doubling. Don't double it here.
                let new_binding = if old_binding == PC_RING_BUFFER_BINDING {
                    old_binding
                } else {
                    old_binding.wrapping_mul(2)
                };
                push_word(&mut out, w0);
                push_word(&mut out, target);
                push_word(&mut out, decoration);
                push_word(&mut out, new_binding);
                for i in 4..wc {
                    push_word(&mut out, read_word(bytes, pos + i));
                }
                pos += wc;
                continue;
            }
        }

        // Rewrite OpVariable for the image var: change its type from ptr_to_SampledImage
        // to the new image_ptr_type_id (ptr_to_Image). This lets NAGA emit the correct
        // WGSL texture type (e.g. texture_2d_array<f32>) instead of getting confused by
        // the SampledImage pointer type.
        if op == 59 && wc >= 4 { // OpVariable
            let var_id = read_word(bytes, pos + 2);
            if let Some(split) = splits.get(&var_id) {
                if var_id == split.image_var_id {
                    // Replace ptr_to_SampledImage with ptr_to_Image for the image variable.
                    push_word(&mut out, w0); // same wc|op
                    push_word(&mut out, split.image_ptr_type_id); // new image pointer type
                    push_word(&mut out, var_id);
                    push_word(&mut out, 0); // UniformConstant storage class
                    pos += wc;
                    continue;
                }
            }
        }

        // Rewrite OpLoad of combined image sampler (from global var or function param).
        if op == 61 && wc >= 4 { // OpLoad
            let result_id = read_word(bytes, pos + 2);
            if let Some(&combined_var_id) = combined_loads.get(&result_id) {
                let split = &splits[&combined_var_id];
                let (image_load_id, sampler_load_id) = load_splits[&result_id];

                // OpLoad %image_type %image_load_id %image_var (= original combined var)
                push_word(&mut out, (4u32 << 16) | 61);
                push_word(&mut out, split.image_type_id);
                push_word(&mut out, image_load_id);
                push_word(&mut out, split.image_var_id);

                // OpLoad %sampler_type %sampler_load_id %sampler_var
                push_word(&mut out, (4u32 << 16) | 61);
                push_word(&mut out, sampler_type_id);
                push_word(&mut out, sampler_load_id);
                push_word(&mut out, split.sampler_var_id);

                // OpSampledImage %sampled_image_type %result_id %image_load_id %sampler_load_id
                push_word(&mut out, (5u32 << 16) | 86);
                push_word(&mut out, split.sampled_image_type_id);
                push_word(&mut out, result_id);
                push_word(&mut out, image_load_id);
                push_word(&mut out, sampler_load_id);

                pos += wc;
                continue;
            }
        }

        // Copy instruction as-is.
        let byte_start = pos * 4;
        let byte_end = (pos + wc) * 4;
        out.extend_from_slice(&bytes[byte_start..byte_end]);

        // After copying an OpTypeImage, inject the image pointer type for any
        // split that uses this image_type_id as its base. This ensures the
        // OpTypePointer is defined AFTER its base type and BEFORE the OpVariable
        // that uses it — satisfying SPIR-V forward-reference rules within section 8.
        if op == 25 && wc >= 9 { // OpTypeImage
            let image_type_id = read_word(bytes, pos + 1);
            for (&_var_id, split) in &splits {
                if split.image_type_id == image_type_id && !image_ptr_injected.contains(&image_type_id) {
                    image_ptr_injected.insert(image_type_id);
                    // OpTypePointer image_ptr_type_id UniformConstant image_type_id
                    push_word(&mut out, (4u32 << 16) | 32);
                    push_word(&mut out, split.image_ptr_type_id);
                    push_word(&mut out, 0); // UniformConstant
                    push_word(&mut out, split.image_type_id);
                }
            }
        }

        pos += wc;
    }

    log(&format!("[SPLIT] Rewrote {} OpLoad(s), output {} bytes (was {}); {} func params tracked",
        combined_loads.len(), out.len(), bytes.len(), param_combined.len()));
    out
}

/// Fix OpTypeImage with depth=2 (unknown) by resolving based on actual usage.
///
/// WGSL requires textures to be statically typed as either `texture_depth_*`
/// (for comparison/shadow sampling) or `texture_*<f32>` (for regular sampling).
/// NAGA rejects variables used for BOTH comparison and non-comparison sampling
/// (`InconsistentComparisonSampling` error).
///
/// This function:
/// 1. Finds ALL image-type UniformConstant variables and classifies their usage
///    as Dref (comparison), non-Dref (regular), or both.
/// 2. For variables with BOTH usages: duplicates the variable, rewrites Dref
///    OpSampledImage to load from the copy, and forces depth=0 on the image type
/// Fix OpTypeImage with depth=2 (unknown) → depth=1 (explicit depth).
///
/// WGSL's texture_depth_2d supports BOTH textureSample (returns f32)
/// and textureSampleCompare. NAGA handles the vec4/f32 mismatch by
/// inserting a Splat expression when is_depth && !depth_ref at parse time.
///
/// Function parameter image types with depth=0 are NOT changed here —
/// those are handled in mod.rs by patching function parameters that have
/// COMPARISON-only sampling flags to Depth type after parsing.
pub fn fix_depth2_images(bytes: &[u8]) -> (Vec<u8>, Vec<(u32, u32, u32)>) {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return (bytes.to_vec(), Vec::new());
    }

    let mut out = bytes.to_vec();

    let mut pos: usize = 5;
    while pos < total_words {
        let w0 = read_word(&out, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        if op == 25 && wc >= 9 { // OpTypeImage
            let id = read_word(&out, pos + 1);
            let depth = read_word(&out, pos + 4);
            if depth == 2 {
                let off = (pos + 4) * 4;
                out[off..off + 4].copy_from_slice(&1u32.to_le_bytes());
                log(&format!("[DEPTH2] Set depth=2 → 1 for OpTypeImage %{}", id));
            }
        }
        pos += wc;
    }

    (out, Vec::new())
}

/// Replace infinity/NaN literals with large finite values throughout a module.
fn fix_nonfinite_literals(module: &mut Module) {
    fn fix_lit(lit: &mut Literal) {
        match lit {
            Literal::F32(v) if v.is_infinite() || v.is_nan() => {
                *lit = Literal::F32(if v.is_sign_negative() { f32::MIN } else { f32::MAX });
            }
            Literal::F64(v) if v.is_infinite() || v.is_nan() => {
                *lit = Literal::F64(if v.is_sign_negative() { f64::MIN } else { f64::MAX });
            }
            _ => {}
        }
    }

    for (_, expr) in module.global_expressions.iter_mut() {
        if let Expression::Literal(ref mut lit) = *expr {
            fix_lit(lit);
        }
    }
    for (_, func) in module.functions.iter_mut() {
        for (_, expr) in func.expressions.iter_mut() {
            if let Expression::Literal(ref mut lit) = *expr {
                fix_lit(lit);
            }
        }
    }
    for ep in module.entry_points.iter_mut() {
        for (_, expr) in ep.function.expressions.iter_mut() {
            if let Expression::Literal(ref mut lit) = *expr {
                fix_lit(lit);
            }
        }
    }
}

/// Reduce BindingArray sizes to 1 element.
/// Dawn/WebGPU doesn't support binding arrays through multiple bind group entries
/// at the same binding index. We reduce the array size to 1 so the WGSL output
/// declares binding_array<T, 1> and the BGL/bind group only need a single entry.
/// Access expressions remain valid (dynamic index on a 1-element array is harmless).
fn flatten_binding_arrays(module: &mut Module) {
    use naga::ArraySize;
    use std::num::NonZeroU32;

    let one = NonZeroU32::new(1).unwrap();
    let mut changed = 0u32;
    // Collect type handles that need modification (can't modify during iteration).
    let mut replacements: Vec<(Handle<Type>, Type)> = Vec::new();
    for (handle, ty) in module.types.iter() {
        if let TypeInner::BindingArray { base, size } = ty.inner {
            match size {
                ArraySize::Constant(n) if n.get() > 1 => {
                    let mut new_ty = ty.clone();
                    new_ty.inner = TypeInner::BindingArray { base, size: ArraySize::Constant(one) };
                    replacements.push((handle, new_ty));
                    changed += 1;
                }
                ArraySize::Dynamic => {
                    // Dynamic binding arrays also need to be flattened to size 1.
                    let mut new_ty = ty.clone();
                    new_ty.inner = TypeInner::BindingArray { base, size: ArraySize::Constant(one) };
                    replacements.push((handle, new_ty));
                    changed += 1;
                }
                _ => {}
            }
        }
    }
    for (handle, new_ty) in replacements {
        module.types.replace(handle, new_ty);
    }
    if changed > 0 {
        log(&format!("[FLATTEN-BA] Reduced {} binding array type(s) to size 1", changed));
    }
}

/// Convert boolean members in entry-point I/O structs to u32.
/// WGSL (and naga validation) requires @location bindings to be IO-shareable,
/// which excludes booleans. We relax this validation in the patched NAGA instead.
fn fix_bool_io(_module: &mut Module) {
    // NotIOShareableType validation is relaxed in patched naga's valid/interface.rs
}

/// Convert write-only storage buffers to read-write (WGSL requirement).
fn fix_writeonly_storage(module: &mut Module) {
    for (_, var) in module.global_variables.iter_mut() {
        if let AddressSpace::Storage { ref mut access } = var.space {
            if access.contains(StorageAccess::STORE) && !access.contains(StorageAccess::LOAD) {
                *access = StorageAccess::LOAD | StorageAccess::STORE;
            }
        }
    }
}

/// Replace PointSize builtins with @location outputs.
/// WGSL has no @builtin(point_size); WebGPU always renders points as 1px.
/// We remap PointSize to an unused @location so the shader compiles and
/// the value is silently discarded by the pipeline.
fn strip_point_size(module: &mut Module) {
    // Find the highest location already in use.
    let mut max_loc: u32 = 0;
    for (_, ty) in module.types.iter() {
        if let TypeInner::Struct { ref members, .. } = ty.inner {
            for m in members {
                if let Some(Binding::Location { location, .. }) = m.binding {
                    max_loc = max_loc.max(location + 1);
                }
            }
        }
    }

    // Collect types whose struct members include PointSize, and build
    // replacement types with PointSize remapped to a @location.
    let mut replacements: Vec<(Handle<Type>, Type)> = Vec::new();
    for (h, ty) in module.types.iter() {
        if let TypeInner::Struct { ref members, .. } = ty.inner {
            if members
                .iter()
                .any(|m| matches!(m.binding, Some(Binding::BuiltIn(BuiltIn::PointSize))))
            {
                let mut new_ty = ty.clone();
                if let TypeInner::Struct {
                    ref mut members, ..
                } = new_ty.inner
                {
                    for m in members.iter_mut() {
                        if matches!(m.binding, Some(Binding::BuiltIn(BuiltIn::PointSize))) {
                            m.binding = Some(Binding::Location {
                                location: max_loc,
                                blend_src: None,
                                interpolation: Some(Interpolation::Perspective),
                                sampling: Some(Sampling::Center),
                                per_primitive: false,
                            });
                            max_loc += 1;
                        }
                    }
                }
                replacements.push((h, new_ty));
            }
        }
    }
    for (handle, new_ty) in replacements {
        module.types.replace(handle, new_ty);
    }

    // Fix entry point argument and result bindings.
    for ep in module.entry_points.iter_mut() {
        for arg in ep.function.arguments.iter_mut() {
            if matches!(arg.binding, Some(Binding::BuiltIn(BuiltIn::PointSize))) {
                arg.binding = Some(Binding::Location {
                    location: max_loc,
                    blend_src: None,
                    interpolation: Some(Interpolation::Perspective),
                    sampling: Some(Sampling::Center),
                    per_primitive: false,
                });
                max_loc += 1;
            }
        }
        if let Some(ref mut result) = ep.function.result {
            if matches!(result.binding, Some(Binding::BuiltIn(BuiltIn::PointSize))) {
                result.binding = Some(Binding::Location {
                    location: max_loc,
                    blend_src: None,
                    interpolation: Some(Interpolation::Perspective),
                    sampling: Some(Sampling::Center),
                    per_primitive: false,
                });
                max_loc += 1;
            }
        }
    }
}

/// Dump SPIR-V instructions that define or reference a given ID (for debugging).
fn dump_spirv_around_error(bytes: &[u8], error_msg: &str) {
    let target_id: Option<u32> = error_msg
        .find("InvalidId(")
        .and_then(|start| {
            let rest = &error_msg[start + 10..];
            rest.find(')').and_then(|end| rest[..end].parse().ok())
        });
    let target_id = match target_id {
        Some(id) => id,
        None => return,
    };

    let total_words = bytes.len() / 4;
    if total_words < 5 { return; }

    // Dump all instructions for small shaders, or just those referencing target_id.
    let mut lines = Vec::new();
    let mut pos: usize = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words { break; }

        // Check if any word matches target_id.
        let mut mentions = false;
        for i in 1..wc {
            if read_word(bytes, pos + i) == target_id { mentions = true; break; }
        }

        // For small shaders or instructions mentioning the target ID, dump them.
        if mentions || total_words < 1000 {
            let max_w = wc.min(6);
            let words: Vec<u32> = (0..max_w).map(|i| read_word(bytes, pos + i)).collect();
            lines.push(format!("@{}: op={} wc={} {:?}{}",
                pos, op, wc, words,
                if mentions { " <<<" } else { "" }));
        }
        pos += wc;
    }
    log(&format!("[DUMP] InvalidId({}) in {} words ({} instructions):\n{}",
        target_id, total_words, lines.len(), lines.join("\n")));
    // Also dump the entire SPIR-V as hex chunked into manageable log lines.
    const CHUNK: usize = 60; // words per log line
    let hex_words: Vec<String> = (0..total_words)
        .map(|i| format!("{:08x}", read_word(bytes, i)))
        .collect();
    let total_chunks = (hex_words.len() + CHUNK - 1) / CHUNK;
    log(&format!("[SPVHEX] total_words={} chunks={}", total_words, total_chunks));
    for (i, chunk) in hex_words.chunks(CHUNK).enumerate() {
        log(&format!("[SPVHEX:{}/{}] {}", i, total_chunks, chunk.join(" ")));
    }
}

/// Convert SPIR-V binary to WGSL source string.
/// Takes a &[u8] of SPIR-V bytes, returns a WGSL string or an error message.
#[wasm_bindgen]
pub fn spirv_to_wgsl(spirv_bytes: &[u8]) -> Result<String, JsError> {
    if spirv_bytes.len() % 4 != 0 {
        return Err(JsError::new("SPIR-V byte length must be a multiple of 4"));
    }

    // Pre-process: freeze OpSpecConstantOp instructions that naga can't handle.
    let spirv_bytes = freeze_spec_constant_ops(spirv_bytes);

    // Pre-process: rewrite SPIR-V 1.4+ instructions unsupported by naga.
    let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
    let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);

    // Pre-process: add OpDecorate NonWritable to storage buffer vars that are never
    // stored to. glslang omits NonWritable for `restrict readonly buffer` blocks, so
    // NAGA defaults all SSBOs to var<storage, read_write>. This causes writable-buffer
    // aliasing errors when Godot binds a single placeholder buffer in multiple slots
    // across bind groups in the same pass (e.g. skeleton compute with blend shapes off).
    let spirv_bytes = infer_readonly_storage(&spirv_bytes);

    // Pre-process: remap push-constant variables to uniform buffer bindings.
    // Naga v28 silently drops push constants from WGSL output — we fix this
    // in SPIR-V so the binding appears as a regular UBO at @group(3) @binding(0).
    let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);

    // Pre-process: split combined image samplers into separate image + sampler.
    let spirv_bytes = split_combined_samplers(&spirv_bytes);

    // Pre-process: fix OpTypeImage with depth=2 (unknown comparison).
    // Must run after split_combined_samplers (which doubles bindings).
    let (spirv_bytes, depth_aliases) = fix_depth2_images(&spirv_bytes);

    // adjust_coordinate_space: true makes NAGA negate gl_Position.y in vertex
    // shaders before writing WGSL. This compensates for the difference between
    // Vulkan's Y-down NDC (which Godot's GLSL shaders target) and WebGPU/WGSL's
    // Y-up NDC. Without this, all rendered content appears flipped vertically.
    let opts = spv::Options {
        adjust_coordinate_space: true,
        strict_capabilities: false,
        block_ctx_dump_prefix: None,
    };

    let mut module = spv::parse_u8_slice(&spirv_bytes, &opts)
        .map_err(|e| {
            // On failure, dump SPIR-V instructions to help debug InvalidId.
            dump_spirv_around_error(&spirv_bytes, &format!("{e:?}"));
            JsError::new(&format!("SPIR-V parse error: {e:?}"))
        })?;

    // Pre-validation fixes for WGSL compatibility.
    fix_writeonly_storage(&mut module);
    fix_nonfinite_literals(&mut module);
    strip_point_size(&mut module);
    flatten_binding_arrays(&mut module);

    // Validate.
    let info = Validator::new(ValidationFlags::all(), Capabilities::all())
        .validate(&module)
        .map_err(|e| {
            // Dump all global variable bindings to diagnose BindingCollision.
            log("[PCFIX-DBG] Validation failed — global variable bindings:");
            for (handle, var) in module.global_variables.iter() {
                if let Some(bind) = &var.binding {
                    log(&format!("  {:?} name={:?} group={} binding={}",
                        handle, var.name, bind.group, bind.binding));
                }
            }
            JsError::new(&format!("Validation error: {e:?}"))
        })?;

    // Resolve pipeline-overridable constants (specialization constants)
    let pipeline_constants = PipelineConstants::default();
    let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
        .map_err(|e| JsError::new(&format!("Pipeline constants error: {e:?}")))?;

    // Extract per-entry-point storage buffer binding usage metadata.
    // Naga's validator computes which global variables are transitively reachable
    // from each entry point through the call graph. We output this as WGSL comments
    // so the C++ driver can set BGL visibility per-stage, keeping per-stage storage
    // buffer counts under Firefox/wgpu's Metal limit of 8.
    let mut binding_metadata = String::new();
    let mut total_globals = 0u32;
    let mut storage_globals = 0u32;
    for (ep_idx, _ep) in module.entry_points.iter().enumerate() {
        let ep_info = info.get_entry_point(ep_idx);
        for (handle, var) in module.global_variables.iter() {
            total_globals += 1;
            let usage = ep_info[handle];
            if let Some(ref bind) = var.binding {
                if matches!(var.space, AddressSpace::Storage { .. }) {
                    storage_globals += 1;
                    if !usage.is_empty() {
                        binding_metadata.push_str(&format!(
                            "//SSBO_USED:{},{}\n", bind.group, bind.binding
                        ));
                    }
                }
            }
        }
    }
    log(&format!("[SSBO-META-GEN] globals={total_globals} storage={storage_globals} used_lines={}",
        binding_metadata.matches("//SSBO_USED:").count()));

    // process_overrides remaps Expression::Override → Expression::Constant
    // but leaves the overrides arena populated. The WGSL writer rejects
    // modules with any overrides, so clear the arena.
    let mut module = module.into_owned();
    module.overrides = Arena::new();

    // Write WGSL.
    let wgsl = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty())
        .map_err(|e| JsError::new(&format!("WGSL write error: {e:?}")))?;

    // Post-process 1: Rewrite push_constant address space.
    // convert_push_constants_to_uniforms() changed the SPIR-V storage class from
    // PushConstant to StorageBuffer, so naga writes var<storage, read>.
    // We keep var<storage, read> for ALL shaders — var<uniform> requires std140 layout
    // which is incompatible with push constant structs containing arrays (stride 4 vs 16).
    // The 8-buffer-per-stage limit is addressed by per-stage visibility metadata instead.
    // Fallback: if naga still emits var<push_constant> (shouldn't happen after SPIR-V rewrite).
    let wgsl = wgsl.replace(
        "var<push_constant>",
        &format!("@group(3) @binding({PC_RING_BUFFER_BINDING}) var<storage, read>"),
    );
    log(&format!("[NAGA] spirv_to_wgsl returned, output length={}", wgsl.len()));

    // Debug: check if Y-negate was applied for vertex shaders
    if wgsl.contains("@builtin(position)") {
        let has_negate = wgsl.contains("Negate") || wgsl.contains("= -") || wgsl.contains("negate");
        log(&format!("[YFLIP] vertex shader has @builtin(position), negate_present={has_negate}"));
    }

    // Post-process 2: Replace out-of-range f32 decimal literals.
    // naga writes f32::MAX as a decimal that the WGSL parser may reject
    // (it rounds the digit string to a value just above f32::MAX).
    // Replace any known large-magnitude f32 decimal literals with hex floats.
    let wgsl = fix_fmax_literals(&wgsl);

    // Post-process 3: Strip `enable f16;` — Chrome/WebGPU doesn't support the
    // shader-f16 feature yet and will reject shaders containing this directive.
    // Naga emits it whenever the module uses f16 types, but Godot's shaders
    // don't actually need half-precision; the f16 usage comes from naga seeing
    // SPIR-V Float16 capabilities that glslang emits for decorations.
    let wgsl = wgsl.replace("enable f16;\n", "");
    let wgsl = wgsl.replace("enable f16;", "");

    // Post-process 4: Prepend diagnostic directive to suppress derivative
    // uniformity errors. Godot shaders call textureSample() inside
    // non-uniform control flow (e.g. inside if/else branches depending on
    // per-instance flags). WGSL requires textureSample to be in uniform
    // control flow, but this is harmless for Godot's usage since all canvas
    // draws have uniform fragment flow at the WGPU level.
    let mut prefix = String::from("diagnostic(off, derivative_uniformity);\n");

    // No depth aliases needed — depth=2 images are now depth=1, and WGSL's
    // texture_depth_2d supports both textureSample and textureSampleCompare.

    let wgsl = format!("{binding_metadata}{prefix}{wgsl}");

    Ok(wgsl)
}

/// Native (non-WASM) version of `spirv_to_wgsl` for fuzzing and testing.
/// Identical pipeline but returns `Result<String, String>` instead of `JsError`.
pub fn spirv_to_wgsl_native(spirv_bytes: &[u8]) -> Result<String, String> {
    if spirv_bytes.len() % 4 != 0 {
        return Err("SPIR-V byte length must be a multiple of 4".into());
    }

    let spirv_bytes = freeze_spec_constant_ops(spirv_bytes);
    let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
    let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
    let spirv_bytes = infer_readonly_storage(&spirv_bytes);
    let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
    let spirv_bytes = split_combined_samplers(&spirv_bytes);
    let (spirv_bytes, _depth_aliases) = fix_depth2_images(&spirv_bytes);

    let opts = spv::Options {
        adjust_coordinate_space: true,
        strict_capabilities: false,
        block_ctx_dump_prefix: None,
    };

    let mut module = spv::parse_u8_slice(&spirv_bytes, &opts)
        .map_err(|e| format!("SPIR-V parse error: {e:?}"))?;

    fix_writeonly_storage(&mut module);
    fix_nonfinite_literals(&mut module);
    strip_point_size(&mut module);
    flatten_binding_arrays(&mut module);

    let info = Validator::new(ValidationFlags::all(), Capabilities::all())
        .validate(&module)
        .map_err(|e| format!("Validation error: {e:?}"))?;

    let pipeline_constants = PipelineConstants::default();
    let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
        .map_err(|e| format!("Pipeline constants error: {e:?}"))?;

    let mut module = module.into_owned();
    module.overrides = Arena::new();

    let wgsl = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty())
        .map_err(|e| format!("WGSL write error: {e:?}"))?;

    let wgsl = fix_fmax_literals(&wgsl);
    let wgsl = wgsl.replace("enable f16;\n", "");
    let wgsl = wgsl.replace("enable f16;", "");

    Ok(wgsl)
}

/// Replace overflowing f32 decimal literals (near f32::MAX) with hex float
/// equivalents that WGSL always accepts.
fn fix_fmax_literals(wgsl: &str) -> String {
    // Positive f32::MAX variants naga may write:
    const FMAX_HEX: &str     = "0x1.fffffep+127f";
    const NEG_FMAX_HEX: &str = "-0x1.fffffep+127f";
    // Known decimal approximations seen in the wild:
    const LITERALS: &[(&str, &str)] = &[
        // naga rounds trailing digits
        ("340282350000000000000000000000000000000f",   FMAX_HEX),
        ("-340282350000000000000000000000000000000f", NEG_FMAX_HEX),
        // exact expansion of f32::MAX bits
        ("340282346638528859811704183484516925440f",   FMAX_HEX),
        ("-340282346638528859811704183484516925440f", NEG_FMAX_HEX),
        // another rounding variant
        ("340282346638528860000000000000000000000f",   FMAX_HEX),
        ("-340282346638528860000000000000000000000f", NEG_FMAX_HEX),
    ];
    let mut result = wgsl.to_string();
    for (bad, good) in LITERALS {
        result = result.replace(bad, good);
    }
    result
}

// ==================== Tests ====================

#[cfg(test)]
mod tests {
    use super::*;

    // --- Helper: build a minimal SPIR-V header ---
    fn spirv_header(bound: u32) -> Vec<u8> {
        let mut out = Vec::new();
        push_word(&mut out, 0x07230203); // Magic
        push_word(&mut out, 0x00010300); // Version 1.3
        push_word(&mut out, 0);          // Generator
        push_word(&mut out, bound);      // Bound (max ID + 1)
        push_word(&mut out, 0);          // Schema
        out
    }

    /// Encode a SPIR-V instruction: first word = (word_count << 16) | opcode,
    /// followed by the given operand words.
    fn encode_inst(opcode: u16, operands: &[u32]) -> Vec<u8> {
        let wc = (operands.len() as u32) + 1;
        let mut out = Vec::new();
        push_word(&mut out, (wc << 16) | opcode as u32);
        for &w in operands {
            push_word(&mut out, w);
        }
        out
    }

    // ==================== freeze_spec_constant_ops tests ====================

    #[test]
    fn test_freeze_spec_constant_ops_no_ops() {
        // A module with no OpSpecConstantOp should be returned as-is.
        let mut spv = spirv_header(10);
        // Add a plain OpConstant: OpConstant %type=1 %result=2 %value=42
        spv.extend(encode_inst(OP_CONSTANT, &[1, 2, 42]));
        let result = freeze_spec_constant_ops(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_freeze_spec_constant_ops_rewrites_spec_constant() {
        // OpSpecConstant should be rewritten to OpConstant.
        let mut spv = spirv_header(10);
        // OpTypeInt %1 32 0 (unsigned int)
        spv.extend(encode_inst(OP_TYPE_INT, &[1, 32, 0]));
        // OpSpecConstant %1 %2 99
        spv.extend(encode_inst(OP_SPEC_CONSTANT, &[1, 2, 99]));

        let result = freeze_spec_constant_ops(&spv);

        // Parse output to verify OpSpecConstant became OpConstant.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_constant = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_CONSTANT && read_word(&result, pos + 2) == 2 {
                found_constant = true;
                assert_eq!(read_word(&result, pos + 1), 1); // type
                assert_eq!(read_word(&result, pos + 3), 99); // value
            }
            // Should NOT find OpSpecConstant
            assert_ne!(op, OP_SPEC_CONSTANT, "OpSpecConstant should have been rewritten");
            pos += wc;
        }
        assert!(found_constant, "Expected OpConstant for id 2");
    }

    #[test]
    fn test_freeze_spec_constant_ops_evaluates_iadd() {
        // OpSpecConstantOp with IAdd should be evaluated.
        let mut spv = spirv_header(10);
        // OpTypeInt %1 32 0
        spv.extend(encode_inst(OP_TYPE_INT, &[1, 32, 0]));
        // OpConstant %1 %2 10
        spv.extend(encode_inst(OP_CONSTANT, &[1, 2, 10]));
        // OpConstant %1 %3 20
        spv.extend(encode_inst(OP_CONSTANT, &[1, 3, 20]));
        // OpSpecConstantOp %1 %4 IAdd(128) %2 %3
        spv.extend(encode_inst(OP_SPEC_CONSTANT_OP, &[1, 4, 128, 2, 3]));

        let result = freeze_spec_constant_ops(&spv);

        // Find OpConstant for id 4.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_CONSTANT && read_word(&result, pos + 2) == 4 {
                found = true;
                assert_eq!(read_word(&result, pos + 3), 30); // 10 + 20
            }
            assert_ne!(op, OP_SPEC_CONSTANT_OP, "OpSpecConstantOp should be gone");
            pos += wc;
        }
        assert!(found, "Expected OpConstant for id 4 with value 30");
    }

    #[test]
    fn test_freeze_spec_constant_ops_bool_true() {
        // OpSpecConstantOp yielding a bool true should emit OpConstantTrue.
        let mut spv = spirv_header(10);
        // OpTypeBool %1
        spv.extend(encode_inst(OP_TYPE_BOOL, &[1]));
        // OpTypeInt %5 32 0
        spv.extend(encode_inst(OP_TYPE_INT, &[5, 32, 0]));
        // OpConstant %5 %2 7  (value 7)
        spv.extend(encode_inst(OP_CONSTANT, &[5, 2, 7]));
        // OpConstant %5 %3 7  (value 7)
        spv.extend(encode_inst(OP_CONSTANT, &[5, 3, 7]));
        // OpSpecConstantOp %1 %4 IEqual(170) %2 %3  -> true (7==7)
        spv.extend(encode_inst(OP_SPEC_CONSTANT_OP, &[1, 4, 170, 2, 3]));

        let result = freeze_spec_constant_ops(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_CONSTANT_TRUE && read_word(&result, pos + 2) == 4 {
                found = true;
            }
            pos += wc;
        }
        assert!(found, "Expected OpConstantTrue for id 4");
    }

    #[test]
    fn test_freeze_strips_spec_id_decorations() {
        // OpDecorate with SpecId (decoration=1) should be stripped.
        let mut spv = spirv_header(10);
        // OpTypeInt %1 32 0
        spv.extend(encode_inst(OP_TYPE_INT, &[1, 32, 0]));
        // OpDecorate %2 SpecId 0  (decoration=1, specId=0)
        spv.extend(encode_inst(OP_DECORATE, &[2, 1, 0]));
        // OpSpecConstant %1 %2 42
        spv.extend(encode_inst(OP_SPEC_CONSTANT, &[1, 2, 42]));

        let result = freeze_spec_constant_ops(&spv);

        // Verify no OpDecorate with decoration==1 remains.
        let total_words = result.len() / 4;
        let mut pos = 5;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_DECORATE {
                let deco = read_word(&result, pos + 2);
                assert_ne!(deco, 1, "SpecId decoration should be stripped");
            }
            pos += wc;
        }
    }

    #[test]
    fn test_freeze_spec_constant_true_false() {
        // OpSpecConstantTrue and OpSpecConstantFalse should become OpConstantTrue/False.
        let mut spv = spirv_header(10);
        // OpTypeBool %1
        spv.extend(encode_inst(OP_TYPE_BOOL, &[1]));
        // OpSpecConstantTrue %1 %2
        spv.extend(encode_inst(OP_SPEC_CONSTANT_TRUE, &[1, 2]));
        // OpSpecConstantFalse %1 %3
        spv.extend(encode_inst(OP_SPEC_CONSTANT_FALSE, &[1, 3]));

        let result = freeze_spec_constant_ops(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_true = false;
        let mut found_false = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_CONSTANT_TRUE && read_word(&result, pos + 2) == 2 {
                found_true = true;
            }
            if op == OP_CONSTANT_FALSE && read_word(&result, pos + 2) == 3 {
                found_false = true;
            }
            assert_ne!(op, OP_SPEC_CONSTANT_TRUE);
            assert_ne!(op, OP_SPEC_CONSTANT_FALSE);
            pos += wc;
        }
        assert!(found_true, "Expected OpConstantTrue for id 2");
        assert!(found_false, "Expected OpConstantFalse for id 3");
    }

    #[test]
    fn test_freeze_spec_constant_composite() {
        // OpSpecConstantComposite should be rewritten to OpConstantComposite.
        let mut spv = spirv_header(10);
        // OpTypeInt %1 32 0
        spv.extend(encode_inst(OP_TYPE_INT, &[1, 32, 0]));
        // OpConstant %1 %2 1
        spv.extend(encode_inst(OP_CONSTANT, &[1, 2, 1]));
        // OpConstant %1 %3 2
        spv.extend(encode_inst(OP_CONSTANT, &[1, 3, 2]));
        // OpSpecConstantComposite %type=4 %result=5 %2 %3
        spv.extend(encode_inst(OP_SPEC_CONSTANT_COMPOSITE, &[4, 5, 2, 3]));

        let result = freeze_spec_constant_ops(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_CONSTANT_COMPOSITE && read_word(&result, pos + 2) == 5 {
                found = true;
                // Verify constituents are preserved.
                assert_eq!(read_word(&result, pos + 3), 2);
                assert_eq!(read_word(&result, pos + 4), 3);
            }
            assert_ne!(op, OP_SPEC_CONSTANT_COMPOSITE);
            pos += wc;
        }
        assert!(found, "Expected OpConstantComposite for id 5");
    }

    // ==================== rewrite_copy_logical tests ====================

    #[test]
    fn test_rewrite_copy_logical_no_ops() {
        // No OpCopyLogical in input, output should be identical.
        let mut spv = spirv_header(10);
        // Some random instruction (OpNop = 0, wc=1).
        spv.extend(encode_inst(0, &[])); // OpNop
        let result = rewrite_copy_logical(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_rewrite_copy_logical_replaces_opcode() {
        // OpCopyLogical (op=400) should become OpCopyObject (op=83).
        let mut spv = spirv_header(10);
        // OpCopyLogical: wc=4, op=400, type=1, result=2, operand=3
        spv.extend(encode_inst(400, &[1, 2, 3]));

        let result = rewrite_copy_logical(&spv);

        // Parse and verify.
        let w0 = read_word(&result, 5);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        assert_eq!(op, 83, "OpCopyLogical should become OpCopyObject");
        assert_eq!(wc, 4);
        assert_eq!(read_word(&result, 6), 1); // type
        assert_eq!(read_word(&result, 7), 2); // result
        assert_eq!(read_word(&result, 8), 3); // operand
    }

    #[test]
    fn test_rewrite_copy_logical_multiple() {
        // Multiple OpCopyLogical should all be rewritten.
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(400, &[1, 2, 3])); // First CopyLogical
        spv.extend(encode_inst(400, &[1, 4, 5])); // Second CopyLogical

        let result = rewrite_copy_logical(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut count = 0;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 83 { count += 1; }
            assert_ne!(op, 400, "No OpCopyLogical should remain");
            pos += wc;
        }
        assert_eq!(count, 2);
    }

    #[test]
    fn test_rewrite_copy_logical_preserves_other_instructions() {
        // Other instructions should be untouched.
        let mut spv = spirv_header(10);
        // OpNop
        spv.extend(encode_inst(0, &[]));
        // OpCopyLogical
        spv.extend(encode_inst(400, &[1, 2, 3]));
        // Another OpNop
        spv.extend(encode_inst(0, &[]));

        let result = rewrite_copy_logical(&spv);

        // Same byte length.
        assert_eq!(result.len(), spv.len());
        // First instruction is still OpNop.
        let w0 = read_word(&result, 5);
        assert_eq!((w0 & 0xFFFF) as u16, 0);
    }

    // ==================== rewrite_terminate_invocation tests ====================

    #[test]
    fn test_rewrite_terminate_invocation_no_ops() {
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(0, &[])); // OpNop
        let result = rewrite_terminate_invocation(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_rewrite_terminate_invocation_replaces_opcode() {
        // OpTerminateInvocation (4416) should become OpKill (252).
        let mut spv = spirv_header(10);
        // OpTerminateInvocation is wc=1, no operands.
        spv.extend(encode_inst(4416, &[]));

        let result = rewrite_terminate_invocation(&spv);

        let w0 = read_word(&result, 5);
        let op = (w0 & 0xFFFF) as u16;
        let wc = (w0 >> 16) as usize;
        assert_eq!(op, 252, "OpTerminateInvocation should become OpKill");
        assert_eq!(wc, 1);
    }

    #[test]
    fn test_rewrite_terminate_invocation_multiple() {
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(4416, &[]));
        // Some other instruction in between
        spv.extend(encode_inst(0, &[])); // OpNop
        spv.extend(encode_inst(4416, &[]));

        let result = rewrite_terminate_invocation(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut kill_count = 0;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 252 { kill_count += 1; }
            assert_ne!(op, 4416, "No OpTerminateInvocation should remain");
            pos += wc;
        }
        assert_eq!(kill_count, 2);
    }

    // ==================== infer_readonly_storage tests ====================

    #[test]
    fn test_infer_readonly_no_storage_buffers() {
        // No storage buffer variables => output unchanged.
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(0, &[])); // OpNop
        let result = infer_readonly_storage(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_infer_readonly_adds_nonwritable() {
        // A StorageBuffer variable that is never stored to should get NonWritable.
        let mut spv = spirv_header(10);
        // Need a decoration instruction so the injection point exists.
        // OpDecorate %1 Block (decoration=2)
        spv.extend(encode_inst(71, &[1, 2]));
        // OpVariable: type=2, result=3, StorageClass=StorageBuffer(12)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_STORAGE_BUFFER]));

        let result = infer_readonly_storage(&spv);

        // Should have NonWritable decoration added for var 3.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_nonwritable = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                if target == 3 && deco == DECO_NON_WRITABLE {
                    found_nonwritable = true;
                }
            }
            pos += wc;
        }
        assert!(found_nonwritable, "Expected NonWritable decoration for var 3");
    }

    #[test]
    fn test_infer_readonly_written_var_not_decorated() {
        // A StorageBuffer variable that IS stored to should NOT get NonWritable.
        let mut spv = spirv_header(10);
        // OpDecorate %1 Block
        spv.extend(encode_inst(71, &[1, 2]));
        // OpVariable: type=2, result=3, StorageClass=StorageBuffer(12)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_STORAGE_BUFFER]));
        // OpStore(62): pointer=3, value=99
        spv.extend(encode_inst(62, &[3, 99]));

        let result = infer_readonly_storage(&spv);

        // Should NOT add NonWritable for var 3 (it's written to).
        let total_words = result.len() / 4;
        let mut pos = 5;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                assert!(
                    !(target == 3 && deco == DECO_NON_WRITABLE),
                    "Written var should NOT get NonWritable"
                );
            }
            pos += wc;
        }
    }

    #[test]
    fn test_infer_readonly_access_chain_write() {
        // Writing through an OpAccessChain derived from a storage buffer should mark it writable.
        let mut spv = spirv_header(10);
        // OpDecorate %1 Block
        spv.extend(encode_inst(71, &[1, 2]));
        // OpVariable: type=2, result=3, StorageClass=StorageBuffer(12)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_STORAGE_BUFFER]));
        // OpAccessChain(65): type=5, result=4, base=3, index=6
        spv.extend(encode_inst(65, &[5, 4, 3, 6]));
        // OpStore(62): pointer=4, value=99
        spv.extend(encode_inst(62, &[4, 99]));

        let result = infer_readonly_storage(&spv);

        // Var 3 is written to through access chain -> should NOT get NonWritable.
        let total_words = result.len() / 4;
        let mut pos = 5;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                assert!(
                    !(target == 3 && deco == DECO_NON_WRITABLE),
                    "Var written via access chain should NOT get NonWritable"
                );
            }
            pos += wc;
        }
    }

    #[test]
    fn test_infer_readonly_already_has_nonwritable() {
        // If a var already has NonWritable, don't add a duplicate.
        let mut spv = spirv_header(10);
        // OpDecorate %3 NonWritable
        spv.extend(encode_inst(71, &[3, DECO_NON_WRITABLE]));
        // OpVariable: type=2, result=3, StorageClass=StorageBuffer(12)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_STORAGE_BUFFER]));

        let result = infer_readonly_storage(&spv);

        // Count NonWritable decorations for var 3 — should be exactly 1.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut nw_count = 0;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                if target == 3 && deco == DECO_NON_WRITABLE {
                    nw_count += 1;
                }
            }
            pos += wc;
        }
        assert_eq!(nw_count, 1, "Should not duplicate NonWritable");
    }

    #[test]
    fn test_infer_readonly_mixed_vars() {
        // Two storage buffer vars: one written, one not. Only the unwritten one gets NonWritable.
        let mut spv = spirv_header(10);
        // OpDecorate %1 Block
        spv.extend(encode_inst(71, &[1, 2]));
        // OpVariable: type=2, result=3, StorageClass=StorageBuffer (read-only)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_STORAGE_BUFFER]));
        // OpVariable: type=2, result=4, StorageClass=StorageBuffer (written)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 4, SC_STORAGE_BUFFER]));
        // OpStore to var 4
        spv.extend(encode_inst(62, &[4, 99]));

        let result = infer_readonly_storage(&spv);

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut nw_for_3 = false;
        let mut nw_for_4 = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                if deco == DECO_NON_WRITABLE {
                    if target == 3 { nw_for_3 = true; }
                    if target == 4 { nw_for_4 = true; }
                }
            }
            pos += wc;
        }
        assert!(nw_for_3, "Read-only var 3 should get NonWritable");
        assert!(!nw_for_4, "Written var 4 should NOT get NonWritable");
    }

    // ==================== convert_push_constants_to_uniforms tests ====================

    #[test]
    fn test_convert_pc_no_push_constants() {
        // No push-constant variables => output unchanged.
        let mut spv = spirv_header(10);
        // OpVariable with Uniform storage class (not PushConstant)
        spv.extend(encode_inst(OP_VARIABLE, &[2, 3, SC_UNIFORM]));
        let result = convert_push_constants_to_uniforms(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_convert_pc_rewrites_storage_class() {
        // A PushConstant variable should have its storage class changed to StorageBuffer.
        let mut spv = spirv_header(10);
        // OpTypePointer(32): id=1, StorageClass=PushConstant(9), pointee=5
        spv.extend(encode_inst(32, &[1, SC_PUSH_CONSTANT, 5]));
        // OpVariable: type=1, result=2, StorageClass=PushConstant(9)
        spv.extend(encode_inst(OP_VARIABLE, &[1, 2, SC_PUSH_CONSTANT]));

        let result = convert_push_constants_to_uniforms(&spv);

        // Find OpVariable for id=2 and check storage class.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_var = false;
        let mut found_ptr = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == OP_VARIABLE && wc >= 4 && read_word(&result, pos + 2) == 2 {
                found_var = true;
                let sc = read_word(&result, pos + 3);
                assert_eq!(sc, SC_STORAGE_BUFFER, "PushConstant should become StorageBuffer");
            }
            if op == 32 && wc >= 4 && read_word(&result, pos + 1) == 1 {
                found_ptr = true;
                let sc = read_word(&result, pos + 2);
                assert_eq!(sc, SC_STORAGE_BUFFER, "Pointer type should become StorageBuffer");
            }
            pos += wc;
        }
        assert!(found_var, "Should find OpVariable for id 2");
        assert!(found_ptr, "Should find OpTypePointer for id 1");
    }

    #[test]
    fn test_convert_pc_injects_binding_decorations() {
        // Should inject DescriptorSet=3 and Binding=120 decorations.
        let mut spv = spirv_header(10);
        // Need a type instruction to trigger injection (opcode 19-32).
        // OpTypePointer(32): id=1, StorageClass=PushConstant(9), pointee=5
        spv.extend(encode_inst(32, &[1, SC_PUSH_CONSTANT, 5]));
        // OpVariable: type=1, result=2, StorageClass=PushConstant(9)
        spv.extend(encode_inst(OP_VARIABLE, &[1, 2, SC_PUSH_CONSTANT]));

        let result = convert_push_constants_to_uniforms(&spv);

        // Look for DescriptorSet=3 and Binding=120 decorations for var 2.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut has_set_3 = false;
        let mut has_binding_120 = false;
        let mut has_nonwritable = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 { break; }
            if op == 71 && wc >= 4 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                let val = read_word(&result, pos + 3);
                if target == 2 {
                    if deco == DECO_DESCRIPTOR_SET && val == 3 { has_set_3 = true; }
                    if deco == DECO_BINDING && val == PC_RING_BUFFER_BINDING { has_binding_120 = true; }
                }
            }
            if op == 71 && wc >= 3 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                if target == 2 && deco == DECO_NON_WRITABLE { has_nonwritable = true; }
            }
            pos += wc;
        }
        assert!(has_set_3, "Expected DescriptorSet=3 for push constant var");
        assert!(has_binding_120, "Expected Binding=120 for push constant var");
        assert!(has_nonwritable, "Expected NonWritable for push constant var");
    }

    // ==================== rewrite_copy_logical edge case ====================

    #[test]
    fn test_rewrite_copy_logical_too_small() {
        // Input smaller than header: should return as-is.
        let spv = vec![0u8; 12]; // Less than 5 words
        let result = rewrite_copy_logical(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_rewrite_terminate_invocation_too_small() {
        let spv = vec![0u8; 8];
        let result = rewrite_terminate_invocation(&spv);
        assert_eq!(result, spv);
    }

    // ==================== fix_depth2_images tests ====================

    #[test]
    fn test_fix_depth2_images_no_depth2() {
        // OpTypeImage with depth=0 should be unchanged.
        let mut spv = spirv_header(10);
        // OpTypeImage: id=1, sampled_type=2, dim=1(2D), depth=0, arrayed=0, ms=0, sampled=1, format=0
        spv.extend(encode_inst(25, &[1, 2, 1, 0, 0, 0, 1, 0]));

        let (result, aliases) = fix_depth2_images(&spv);
        assert_eq!(result, spv, "No changes expected for depth=0");
        assert!(aliases.is_empty());
    }

    #[test]
    fn test_fix_depth2_images_rewrites_depth2_to_1() {
        // OpTypeImage with depth=2 should become depth=1.
        let mut spv = spirv_header(10);
        // OpTypeImage: id=1, sampled_type=2, dim=1(2D), depth=2, arrayed=0, ms=0, sampled=1, format=0
        spv.extend(encode_inst(25, &[1, 2, 1, 2, 0, 0, 1, 0]));

        let (result, _) = fix_depth2_images(&spv);

        // Check depth field (word at pos+4 in instruction, which is header(5)+1(wc|op)+1+2+1+depth_pos=5+4=9)
        // Instruction starts at word 5 (after header).
        // depth is the 5th word of the instruction (0-indexed: wc|op, id, sampled_type, dim, depth)
        let depth = read_word(&result, 5 + 4);
        assert_eq!(depth, 1, "depth=2 should become depth=1");
    }

    #[test]
    fn test_fix_depth2_images_preserves_depth1() {
        // OpTypeImage with depth=1 should remain depth=1 (already correct).
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(25, &[1, 2, 1, 1, 0, 0, 1, 0]));

        let (result, _) = fix_depth2_images(&spv);
        let depth = read_word(&result, 5 + 4);
        assert_eq!(depth, 1);
    }

    // ==================== eval_spec_op tests ====================

    #[test]
    fn test_eval_spec_op_iadd() {
        assert_eq!(eval_spec_op(128, &[5, 3]), 8);
    }

    #[test]
    fn test_eval_spec_op_isub() {
        assert_eq!(eval_spec_op(130, &[10, 3]), 7);
    }

    #[test]
    fn test_eval_spec_op_imul() {
        assert_eq!(eval_spec_op(132, &[6, 7]), 42);
    }

    #[test]
    fn test_eval_spec_op_udiv() {
        assert_eq!(eval_spec_op(134, &[20, 4]), 5);
        // Division by zero => 0
        assert_eq!(eval_spec_op(134, &[20, 0]), 0);
    }

    #[test]
    fn test_eval_spec_op_sdiv() {
        // -10 / 3 = -3 (integer division)
        let neg10 = (-10i32) as u64;
        assert_eq!(eval_spec_op(135, &[neg10, 3]) as i32, -3);
        // Division by zero => 0
        assert_eq!(eval_spec_op(135, &[10, 0]), 0);
    }

    #[test]
    fn test_eval_spec_op_umod() {
        assert_eq!(eval_spec_op(137, &[17, 5]), 2);
        assert_eq!(eval_spec_op(137, &[17, 0]), 0); // mod by zero => 0
    }

    #[test]
    fn test_eval_spec_op_snegate() {
        let five = 5u64;
        let result = eval_spec_op(126, &[five]);
        assert_eq!(result as i32, -5);
    }

    #[test]
    fn test_eval_spec_op_logical_ops() {
        // LogicalEqual (164)
        assert_eq!(eval_spec_op(164, &[1, 1]), 1);
        assert_eq!(eval_spec_op(164, &[1, 0]), 0);
        // LogicalNotEqual (165)
        assert_eq!(eval_spec_op(165, &[1, 0]), 1);
        assert_eq!(eval_spec_op(165, &[1, 1]), 0);
        // LogicalOr (166)
        assert_eq!(eval_spec_op(166, &[0, 0]), 0);
        assert_eq!(eval_spec_op(166, &[1, 0]), 1);
        assert_eq!(eval_spec_op(166, &[0, 1]), 1);
        // LogicalAnd (167)
        assert_eq!(eval_spec_op(167, &[1, 1]), 1);
        assert_eq!(eval_spec_op(167, &[1, 0]), 0);
        // LogicalNot (168)
        assert_eq!(eval_spec_op(168, &[0]), 1);
        assert_eq!(eval_spec_op(168, &[1]), 0);
    }

    #[test]
    fn test_eval_spec_op_select() {
        // Select (169): condition, true_val, false_val
        assert_eq!(eval_spec_op(169, &[1, 42, 99]), 42);
        assert_eq!(eval_spec_op(169, &[0, 42, 99]), 99);
    }

    #[test]
    fn test_eval_spec_op_integer_comparison() {
        // IEqual (170)
        assert_eq!(eval_spec_op(170, &[5, 5]), 1);
        assert_eq!(eval_spec_op(170, &[5, 6]), 0);
        // INotEqual (171)
        assert_eq!(eval_spec_op(171, &[5, 6]), 1);
        assert_eq!(eval_spec_op(171, &[5, 5]), 0);
        // UGreaterThan (172)
        assert_eq!(eval_spec_op(172, &[10, 5]), 1);
        assert_eq!(eval_spec_op(172, &[5, 10]), 0);
        // ULessThan (176)
        assert_eq!(eval_spec_op(176, &[3, 7]), 1);
        assert_eq!(eval_spec_op(176, &[7, 3]), 0);
    }

    #[test]
    fn test_eval_spec_op_bitwise() {
        // BitwiseOr (197)
        assert_eq!(eval_spec_op(197, &[0b1010, 0b0101]), 0b1111);
        // BitwiseAnd (199)
        assert_eq!(eval_spec_op(199, &[0b1010, 0b1100]), 0b1000);
        // BitwiseXor (198)
        assert_eq!(eval_spec_op(198, &[0b1010, 0b1100]), 0b0110);
        // Not (200)
        assert_eq!(eval_spec_op(200, &[0]) as u32, 0xFFFFFFFF);
        // ShiftLeftLogical (196)
        assert_eq!(eval_spec_op(196, &[1, 4]), 16);
        // ShiftRightLogical (194)
        assert_eq!(eval_spec_op(194, &[16, 2]), 4);
    }

    #[test]
    fn test_eval_spec_op_unknown_opcode() {
        // Unknown opcode returns 0.
        assert_eq!(eval_spec_op(9999, &[42, 13]), 0);
    }

    // ==================== fix_fmax_literals tests ====================

    #[test]
    fn test_fix_fmax_literals_positive() {
        let input = "let x = 340282350000000000000000000000000000000f;";
        let result = fix_fmax_literals(input);
        assert!(result.contains("0x1.fffffep+127f"));
        assert!(!result.contains("340282350000000000000000000000000000000f"));
    }

    #[test]
    fn test_fix_fmax_literals_negative() {
        let input = "let x = -340282350000000000000000000000000000000f;";
        let result = fix_fmax_literals(input);
        assert!(result.contains("-0x1.fffffep+127f"));
    }

    #[test]
    fn test_fix_fmax_literals_no_match() {
        let input = "let x = 1.0f;";
        let result = fix_fmax_literals(input);
        assert_eq!(result, input);
    }

    // ==================== split_combined_samplers tests ====================

    #[test]
    fn test_split_combined_samplers_no_combined() {
        // No combined image sampler variables => output unchanged.
        let mut spv = spirv_header(10);
        spv.extend(encode_inst(0, &[])); // OpNop
        let result = split_combined_samplers(&spv);
        assert_eq!(result, spv);
    }

    #[test]
    fn test_split_combined_samplers_basic() {
        // Build a minimal module with a combined image sampler.
        let mut spv = spirv_header(20);

        // OpEntryPoint Fragment %10 "main" %7  (the combined var is %7)
        // Encoding: op=15, exec_model=4(Fragment), entry_id=10, name="main\0", interface_vars...
        // "main" = 0x6E69616D, null-terminated needs: 0x006E6961 after 'm'
        // Actually for SPIR-V string encoding: "main" fits in 2 words: [0x6E69616D, 0x00000000] wait no.
        // "main" in little-endian: bytes m=0x6D, a=0x61, i=0x69, n=0x6E -> word = 0x6E69616D
        // Then null terminator: 0x00000000
        // Actually strings are null-terminated and padded to 4 bytes.
        // "main\0" = 5 bytes -> padded to 8 bytes (2 words): [0x6E69616D, 0x00000000]
        // Wait: "main" = [m,a,i,n] = [0x6D, 0x61, 0x69, 0x6E] in LE word = 0x6E69616D
        // then null = [0x00, ...] next word = 0x00000000
        // But actually "main\0" = 5 chars, fits in 2 words of 4 bytes each = 8 bytes.
        // word1 = bytes [m,a,i,n] = 0x6E69616D, word2 = bytes [\0,pad,pad,pad] = 0x00000000
        // OpEntryPoint: wc = 3 (fixed) + 2 (name) + 1 (interface var) = 6
        // But that's not how encode_inst works... let me just manually build it.
        let ep_wc: u32 = 6;
        let mut ep = Vec::new();
        push_word(&mut ep, (ep_wc << 16) | 15); // OpEntryPoint
        push_word(&mut ep, 4); // Fragment
        push_word(&mut ep, 10); // entry function id
        push_word(&mut ep, 0x6E69616D); // "main" (m,a,i,n)
        push_word(&mut ep, 0x00000000); // null terminator padded
        push_word(&mut ep, 7); // interface variable (the combined sampler)
        spv.extend(ep);

        // OpDecorate %7 DescriptorSet 0
        spv.extend(encode_inst(71, &[7, 34, 0]));
        // OpDecorate %7 Binding 0
        spv.extend(encode_inst(71, &[7, 33, 0]));

        // OpTypeFloat %1 32
        spv.extend(encode_inst(22, &[1, 32]));
        // OpTypeImage %2 %1 Dim=2D(1) Depth=0 Arrayed=0 MS=0 Sampled=1 Format=0
        spv.extend(encode_inst(25, &[2, 1, 1, 0, 0, 0, 1, 0]));
        // OpTypeSampledImage %3 %2
        spv.extend(encode_inst(27, &[3, 2]));
        // OpTypePointer %4 UniformConstant(0) %3
        spv.extend(encode_inst(32, &[4, 0, 3]));
        // OpTypeVoid %8
        spv.extend(encode_inst(19, &[8]));
        // OpTypeFunction %9 %8
        spv.extend(encode_inst(33, &[9, 8]));
        // OpVariable %4 %7 UniformConstant(0)
        spv.extend(encode_inst(59, &[4, 7, 0]));

        // OpFunction %8 %10 None %9  (void main())
        spv.extend(encode_inst(54, &[8, 10, 0, 9]));
        // OpLabel %11
        spv.extend(encode_inst(248, &[11]));
        // OpLoad %3 %12 %7 (load the combined sampler)
        spv.extend(encode_inst(61, &[3, 12, 7]));
        // OpReturn
        spv.extend(encode_inst(253, &[]));
        // OpFunctionEnd
        spv.extend(encode_inst(56, &[]));

        let result = split_combined_samplers(&spv);

        // Verify: output should be different (split happened).
        assert_ne!(result.len(), spv.len(), "Output should differ after splitting");

        // Verify: the original OpLoad of the combined var should be gone,
        // replaced by two separate loads + OpSampledImage.
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_sampled_image = false;
        let mut found_sampler_var = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 86 { // OpSampledImage
                found_sampled_image = true;
            }
            // Look for new sampler variable (OpVariable with sampler_type ptr)
            if op == 59 && wc >= 4 {
                let sc = read_word(&result, pos + 3);
                let var_id = read_word(&result, pos + 2);
                if sc == 0 && var_id != 7 { // A new UniformConstant var (not the original)
                    found_sampler_var = true;
                }
            }
            pos += wc;
        }
        assert!(found_sampled_image, "Expected OpSampledImage after split");
        assert!(found_sampler_var, "Expected a new sampler variable");
    }

    // ==================== End-to-end test ====================

    #[test]
    fn test_end_to_end_minimal_vertex_shader() {
        // Build a minimal but VALID SPIR-V vertex shader and convert to WGSL.
        // This exercises the full pipeline: all preprocessing passes + naga parse + wgsl write.
        //
        // Equivalent GLSL:
        //   #version 450
        //   void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
        //
        // We'll use a pre-assembled SPIR-V binary (manually constructed).
        let spirv: &[u8] = &assemble_minimal_vertex_shader();

        // Run through the full pipeline (minus wasm_bindgen wrapper).
        // We replicate the same pass order as spirv_to_wgsl but call the functions directly.
        let spirv_bytes = freeze_spec_constant_ops(spirv);
        let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
        let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
        let spirv_bytes = infer_readonly_storage(&spirv_bytes);
        let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
        let spirv_bytes = split_combined_samplers(&spirv_bytes);
        let (spirv_bytes, _) = fix_depth2_images(&spirv_bytes);

        // Parse with naga.
        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let module = spv::parse_u8_slice(&spirv_bytes, &opts);
        assert!(module.is_ok(), "SPIR-V parse failed: {:?}", module.err());
        let mut module = module.unwrap();

        // Post-parse fixes.
        fix_writeonly_storage(&mut module);
        fix_nonfinite_literals(&mut module);
        strip_point_size(&mut module);
        flatten_binding_arrays(&mut module);

        // Validate.
        use naga::valid::{Validator, ValidationFlags, Capabilities};
        let info = Validator::new(ValidationFlags::all(), Capabilities::all())
            .validate(&module);
        assert!(info.is_ok(), "Validation failed: {:?}", info.err());
        let info = info.unwrap();

        // Process overrides.
        use naga::back::PipelineConstants;
        use naga::back::pipeline_constants::process_overrides;
        let pipeline_constants = PipelineConstants::default();
        let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
            .expect("process_overrides failed");

        // Clear overrides arena.
        let mut module = module.into_owned();
        module.overrides = naga::Arena::new();

        // Write WGSL.
        use naga::back::wgsl;
        let wgsl_result = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty());
        assert!(wgsl_result.is_ok(), "WGSL write failed: {:?}", wgsl_result.err());
        let wgsl = wgsl_result.unwrap();

        // Basic checks on the WGSL output.
        assert!(wgsl.contains("@vertex"), "Expected @vertex in WGSL output");
        assert!(wgsl.contains("@builtin(position)"), "Expected @builtin(position) in WGSL output");
    }

    /// Assemble a minimal valid SPIR-V vertex shader that outputs gl_Position = vec4(0,0,0,1).
    fn assemble_minimal_vertex_shader() -> Vec<u8> {
        let mut spv = Vec::new();

        // Header
        push_word(&mut spv, 0x07230203); // Magic
        push_word(&mut spv, 0x00010300); // Version 1.3
        push_word(&mut spv, 0);          // Generator
        push_word(&mut spv, 20);         // Bound
        push_word(&mut spv, 0);          // Schema

        // IDs:
        // 1 = void
        // 2 = function type (void -> void)
        // 3 = float (f32)
        // 4 = vec4
        // 5 = ptr Output vec4
        // 6 = gl_Position variable
        // 7 = constant 0.0
        // 8 = constant 1.0
        // 9 = composite constant vec4(0,0,0,1)
        // 10 = main function
        // 11 = label
        // 12 = ptr Input (not used, just for coverage)

        // OpCapability Shader (op=17, capability=1)
        spv.extend(encode_inst(17, &[1]));
        // OpMemoryModel Logical GLSL450 (op=14, addressing=0, memory=1)
        spv.extend(encode_inst(14, &[0, 1]));
        // OpEntryPoint Vertex %10 "main" %6
        // "main" = 2 words: 0x6E69616D, 0x00000000
        let ep_wc: u32 = 7;
        push_word(&mut spv, (ep_wc << 16) | 15);
        push_word(&mut spv, 0); // Vertex
        push_word(&mut spv, 10); // function id
        push_word(&mut spv, 0x6E69616D); // "main"
        push_word(&mut spv, 0x00000000); // null terminator
        push_word(&mut spv, 6); // gl_Position interface var
        push_word(&mut spv, 0); // padding? No - the last one is already the interface.
        // Actually wc=7 means 7 words total. Let me recount:
        // word0: wc|op, word1: exec_model, word2: func_id, word3-4: name, word5: var1
        // That's 6 words. Let me fix.
        // Oops I already pushed 7 words. Let me redo this properly.

        // Actually let me restart the assembly more carefully.
        spv.clear();

        // Header
        push_word(&mut spv, 0x07230203);
        push_word(&mut spv, 0x00010300);
        push_word(&mut spv, 0);
        push_word(&mut spv, 20);
        push_word(&mut spv, 0);

        // OpCapability Shader: op=17, wc=2
        spv.extend(encode_inst(17, &[1]));

        // OpMemoryModel Logical GLSL450: op=14, wc=3
        spv.extend(encode_inst(14, &[0, 1]));

        // OpEntryPoint Vertex %10 "main" %6
        // wc = 1 + 1 + 1 + 2(name) + 1(interface) = 6
        {
            let wc: u32 = 6;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 0); // Vertex execution model
            push_word(&mut spv, 10); // main function
            push_word(&mut spv, 0x6E69616D); // "main"
            push_word(&mut spv, 0x00000000); // null + padding
            push_word(&mut spv, 6); // interface: gl_Position
        }

        // OpDecorate %6 BuiltIn Position: op=71, wc=4, target=6, decoration=11(BuiltIn), value=0(Position)
        spv.extend(encode_inst(71, &[6, 11, 0]));

        // OpTypeVoid %1: op=19, wc=2
        spv.extend(encode_inst(19, &[1]));

        // OpTypeFunction %2 %1: op=33, wc=3 (function returning void, no params)
        spv.extend(encode_inst(33, &[2, 1]));

        // OpTypeFloat %3 32: op=22, wc=3
        spv.extend(encode_inst(22, &[3, 32]));

        // OpTypeVector %4 %3 4: op=23, wc=4
        spv.extend(encode_inst(23, &[4, 3, 4]));

        // OpTypePointer %5 Output %4: op=32, wc=4
        spv.extend(encode_inst(32, &[5, 3, 4])); // StorageClass 3 = Output

        // OpVariable %5 %6 Output: op=59, wc=4
        spv.extend(encode_inst(59, &[5, 6, 3])); // StorageClass 3 = Output

        // OpConstant %3 %7 0.0f: op=43, wc=4
        spv.extend(encode_inst(43, &[3, 7, 0x00000000])); // 0.0f

        // OpConstant %3 %8 1.0f: op=43, wc=4
        spv.extend(encode_inst(43, &[3, 8, 0x3F800000])); // 1.0f

        // OpConstantComposite %4 %9 %7 %7 %7 %8: op=44, wc=7
        spv.extend(encode_inst(44, &[4, 9, 7, 7, 7, 8]));

        // OpFunction %1 %10 None %2: op=54, wc=5
        spv.extend(encode_inst(54, &[1, 10, 0, 2]));

        // OpLabel %11: op=248, wc=2
        spv.extend(encode_inst(248, &[11]));

        // OpStore %6 %9: op=62, wc=3
        spv.extend(encode_inst(62, &[6, 9]));

        // OpReturn: op=253, wc=1
        spv.extend(encode_inst(253, &[]));

        // OpFunctionEnd: op=56, wc=1
        spv.extend(encode_inst(56, &[]));

        spv
    }

    #[test]
    fn test_end_to_end_fragment_shader_with_terminate_invocation() {
        // Build a fragment shader that uses OpTerminateInvocation.
        // After preprocessing, it should become OpKill and parse correctly.
        let spirv = assemble_fragment_shader_with_terminate();

        let spirv_bytes = freeze_spec_constant_ops(&spirv);
        let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
        let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
        let spirv_bytes = infer_readonly_storage(&spirv_bytes);
        let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
        let spirv_bytes = split_combined_samplers(&spirv_bytes);
        let (spirv_bytes, _) = fix_depth2_images(&spirv_bytes);

        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let module = spv::parse_u8_slice(&spirv_bytes, &opts);
        assert!(module.is_ok(), "SPIR-V parse failed: {:?}", module.err());
        let mut module = module.unwrap();

        fix_writeonly_storage(&mut module);
        fix_nonfinite_literals(&mut module);
        strip_point_size(&mut module);
        flatten_binding_arrays(&mut module);

        let info = Validator::new(ValidationFlags::all(), Capabilities::all())
            .validate(&module);
        assert!(info.is_ok(), "Validation failed: {:?}", info.err());
        let info = info.unwrap();

        use naga::back::PipelineConstants;
        use naga::back::pipeline_constants::process_overrides;
        let pipeline_constants = PipelineConstants::default();
        let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
            .expect("process_overrides failed");
        let mut module = module.into_owned();
        module.overrides = naga::Arena::new();

        use naga::back::wgsl;
        let wgsl_result = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty());
        assert!(wgsl_result.is_ok(), "WGSL write failed: {:?}", wgsl_result.err());
        let wgsl = wgsl_result.unwrap();

        assert!(wgsl.contains("@fragment"), "Expected @fragment in output");
        // OpKill in WGSL is emitted as "discard;"
        assert!(wgsl.contains("discard"), "Expected 'discard' in WGSL output");
    }

    /// Assemble a minimal fragment shader with OpTerminateInvocation.
    fn assemble_fragment_shader_with_terminate() -> Vec<u8> {
        let mut spv = Vec::new();

        // Header
        push_word(&mut spv, 0x07230203);
        push_word(&mut spv, 0x00010500); // Version 1.5
        push_word(&mut spv, 0);
        push_word(&mut spv, 20);
        push_word(&mut spv, 0);

        // OpCapability Shader
        spv.extend(encode_inst(17, &[1]));

        // OpExtension "SPV_KHR_terminate_invocation"
        // String encoding: "SPV_KHR_terminate_invocation\0" -> 8 words (29 chars + null = 30 bytes -> 8 words)
        // Actually let's just use OpCapability and skip the extension declaration —
        // naga doesn't validate extensions strictly.

        // OpMemoryModel Logical GLSL450
        spv.extend(encode_inst(14, &[0, 1]));

        // OpEntryPoint Fragment %10 "main" %6
        {
            let wc: u32 = 6;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 4); // Fragment execution model
            push_word(&mut spv, 10); // main function
            push_word(&mut spv, 0x6E69616D); // "main"
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 6); // interface: frag_color output
        }

        // OpExecutionMode %10 OriginUpperLeft: op=16, wc=3
        spv.extend(encode_inst(16, &[10, 7])); // 7 = OriginUpperLeft

        // OpDecorate %6 Location 0: op=71
        spv.extend(encode_inst(71, &[6, 30, 0])); // 30 = Location

        // OpTypeVoid %1
        spv.extend(encode_inst(19, &[1]));
        // OpTypeFunction %2 %1
        spv.extend(encode_inst(33, &[2, 1]));
        // OpTypeFloat %3 32
        spv.extend(encode_inst(22, &[3, 32]));
        // OpTypeVector %4 %3 4
        spv.extend(encode_inst(23, &[4, 3, 4]));
        // OpTypePointer %5 Output %4
        spv.extend(encode_inst(32, &[5, 3, 4])); // 3 = Output
        // OpVariable %5 %6 Output
        spv.extend(encode_inst(59, &[5, 6, 3]));
        // OpConstant %3 %7 0.0f
        spv.extend(encode_inst(43, &[3, 7, 0x00000000]));
        // OpConstantComposite %4 %9 %7 %7 %7 %7
        spv.extend(encode_inst(44, &[4, 9, 7, 7, 7, 7]));

        // OpTypeBool %13
        spv.extend(encode_inst(20, &[13]));
        // OpConstantTrue %13 %14
        spv.extend(encode_inst(41, &[13, 14]));

        // OpFunction %1 %10 None %2
        spv.extend(encode_inst(54, &[1, 10, 0, 2]));
        // OpLabel %11
        spv.extend(encode_inst(248, &[11]));

        // OpSelectionMerge %15 None: op=247, wc=3
        spv.extend(encode_inst(247, &[15, 0]));
        // OpBranchConditional %14 %16 %15: op=250, wc=4
        spv.extend(encode_inst(250, &[14, 16, 15]));

        // OpLabel %16
        spv.extend(encode_inst(248, &[16]));
        // OpTerminateInvocation (4416, wc=1)
        spv.extend(encode_inst(4416, &[]));

        // OpLabel %15 (merge block)
        spv.extend(encode_inst(248, &[15]));
        // OpStore %6 %9
        spv.extend(encode_inst(62, &[6, 9]));
        // OpReturn
        spv.extend(encode_inst(253, &[]));
        // OpFunctionEnd
        spv.extend(encode_inst(56, &[]));

        spv
    }

    // ==================== Pipeline pass ordering / integration ====================

    #[test]
    fn test_passes_are_idempotent_on_clean_input() {
        // Running passes on already-clean SPIR-V should not corrupt it.
        let spv = assemble_minimal_vertex_shader();
        let pass1 = freeze_spec_constant_ops(&spv);
        let pass2 = freeze_spec_constant_ops(&pass1);
        assert_eq!(pass1, pass2, "freeze_spec_constant_ops should be idempotent");

        let pass1 = rewrite_copy_logical(&spv);
        let pass2 = rewrite_copy_logical(&pass1);
        assert_eq!(pass1, pass2, "rewrite_copy_logical should be idempotent");

        let pass1 = rewrite_terminate_invocation(&spv);
        let pass2 = rewrite_terminate_invocation(&pass1);
        assert_eq!(pass1, pass2, "rewrite_terminate_invocation should be idempotent");

        let pass1 = infer_readonly_storage(&spv);
        let pass2 = infer_readonly_storage(&pass1);
        assert_eq!(pass1, pass2, "infer_readonly_storage should be idempotent");
    }

    #[test]
    fn test_empty_input() {
        // All passes should handle empty/tiny input gracefully.
        let empty: &[u8] = &[];
        assert_eq!(freeze_spec_constant_ops(empty), empty);
        assert_eq!(rewrite_copy_logical(empty), empty);
        assert_eq!(rewrite_terminate_invocation(empty), empty);
        assert_eq!(infer_readonly_storage(empty), empty);
        assert_eq!(convert_push_constants_to_uniforms(empty), empty);
        assert_eq!(split_combined_samplers(empty), empty);
        let (result, _) = fix_depth2_images(empty);
        assert_eq!(result, empty);
    }

    #[test]
    fn test_header_only_input() {
        // Just a SPIR-V header (5 words, 20 bytes) with no instructions.
        let spv = spirv_header(1);
        assert_eq!(freeze_spec_constant_ops(&spv), spv);
        assert_eq!(rewrite_copy_logical(&spv), spv);
        assert_eq!(rewrite_terminate_invocation(&spv), spv);
        assert_eq!(infer_readonly_storage(&spv), spv);
        assert_eq!(convert_push_constants_to_uniforms(&spv), spv);
        assert_eq!(split_combined_samplers(&spv), spv);
        let (result, _) = fix_depth2_images(&spv);
        assert_eq!(result, spv);
    }

    // ==================== split_combined_samplers (additional tests) ====================

    #[test]
    fn test_split_combined_samplers_multiple_combined_vars() {
        // Two combined image sampler variables at different bindings.
        // Both should be split into separate image + sampler pairs.
        let mut spv = spirv_header(30);

        // OpEntryPoint Fragment %20 "main" %7 %17
        {
            let wc: u32 = 7;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 4); // Fragment
            push_word(&mut spv, 20);
            push_word(&mut spv, 0x6E69616D); // "main"
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 7);  // combined sampler 1
            push_word(&mut spv, 17); // combined sampler 2
        }

        // Decorations for var 7: set=0, binding=0
        spv.extend(encode_inst(71, &[7, 34, 0]));
        spv.extend(encode_inst(71, &[7, 33, 0]));
        // Decorations for var 17: set=0, binding=1
        spv.extend(encode_inst(71, &[17, 34, 0]));
        spv.extend(encode_inst(71, &[17, 33, 1]));

        // Types
        // OpTypeFloat %1 32
        spv.extend(encode_inst(22, &[1, 32]));
        // OpTypeImage %2 %1 Dim=2D Depth=0 Arrayed=0 MS=0 Sampled=1 Format=0
        spv.extend(encode_inst(25, &[2, 1, 1, 0, 0, 0, 1, 0]));
        // OpTypeSampledImage %3 %2
        spv.extend(encode_inst(27, &[3, 2]));
        // OpTypePointer %4 UniformConstant %3
        spv.extend(encode_inst(32, &[4, 0, 3]));
        // OpTypeVoid %8
        spv.extend(encode_inst(19, &[8]));
        // OpTypeFunction %9 %8
        spv.extend(encode_inst(33, &[9, 8]));

        // Variables
        // OpVariable %4 %7 UniformConstant
        spv.extend(encode_inst(59, &[4, 7, 0]));
        // OpVariable %4 %17 UniformConstant (second combined sampler)
        spv.extend(encode_inst(59, &[4, 17, 0]));

        // OpFunction %8 %20 None %9
        spv.extend(encode_inst(54, &[8, 20, 0, 9]));
        // OpLabel %21
        spv.extend(encode_inst(248, &[21]));
        // OpLoad %3 %12 %7
        spv.extend(encode_inst(61, &[3, 12, 7]));
        // OpLoad %3 %22 %17
        spv.extend(encode_inst(61, &[3, 22, 17]));
        // OpReturn
        spv.extend(encode_inst(253, &[]));
        // OpFunctionEnd
        spv.extend(encode_inst(56, &[]));

        let result = split_combined_samplers(&spv);

        // Count OpSampledImage instructions - should be 2 (one per combined var)
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut sampled_image_count = 0;
        let mut new_sampler_vars = 0;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 86 { sampled_image_count += 1; } // OpSampledImage
            if op == 59 && wc >= 4 { // OpVariable
                let var_id = read_word(&result, pos + 2);
                let sc = read_word(&result, pos + 3);
                if sc == 0 && var_id != 7 && var_id != 17 {
                    new_sampler_vars += 1;
                }
            }
            pos += wc;
        }
        assert_eq!(sampled_image_count, 2, "Expected 2 OpSampledImage (one per combined var)");
        assert_eq!(new_sampler_vars, 2, "Expected 2 new sampler variables");
    }

    #[test]
    fn test_split_combined_samplers_2d_array_texture() {
        // Combined sampler with 2D array texture (Dim=1, Arrayed=1).
        let mut spv = spirv_header(20);

        // OpEntryPoint Fragment %10 "main" %7
        {
            let wc: u32 = 6;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 4); // Fragment
            push_word(&mut spv, 10);
            push_word(&mut spv, 0x6E69616D);
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 7);
        }
        spv.extend(encode_inst(71, &[7, 34, 0])); // DescriptorSet 0
        spv.extend(encode_inst(71, &[7, 33, 0])); // Binding 0

        // OpTypeFloat %1 32
        spv.extend(encode_inst(22, &[1, 32]));
        // OpTypeImage %2 %1 Dim=2D(1) Depth=0 Arrayed=1 MS=0 Sampled=1 Format=0
        spv.extend(encode_inst(25, &[2, 1, 1, 0, 1, 0, 1, 0]));
        // OpTypeSampledImage %3 %2
        spv.extend(encode_inst(27, &[3, 2]));
        // OpTypePointer %4 UniformConstant %3
        spv.extend(encode_inst(32, &[4, 0, 3]));
        // OpTypeVoid %8
        spv.extend(encode_inst(19, &[8]));
        // OpTypeFunction %9 %8
        spv.extend(encode_inst(33, &[9, 8]));
        // OpVariable %4 %7 UniformConstant
        spv.extend(encode_inst(59, &[4, 7, 0]));
        // Function
        spv.extend(encode_inst(54, &[8, 10, 0, 9]));
        spv.extend(encode_inst(248, &[11]));
        spv.extend(encode_inst(61, &[3, 12, 7])); // OpLoad
        spv.extend(encode_inst(253, &[]));
        spv.extend(encode_inst(56, &[]));

        let result = split_combined_samplers(&spv);
        assert_ne!(result.len(), spv.len(), "Output should differ after splitting 2D array texture");

        // Verify OpSampledImage was generated
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_sampled_image = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 86 { found_sampled_image = true; }
            pos += wc;
        }
        assert!(found_sampled_image, "Expected OpSampledImage for 2D array texture split");
    }

    #[test]
    fn test_split_combined_samplers_cube_texture() {
        // Combined sampler with Cube texture (Dim=3).
        let mut spv = spirv_header(20);

        // OpEntryPoint Fragment %10 "main" %7
        {
            let wc: u32 = 6;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 4); // Fragment
            push_word(&mut spv, 10);
            push_word(&mut spv, 0x6E69616D);
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 7);
        }
        spv.extend(encode_inst(71, &[7, 34, 0])); // DescriptorSet 0
        spv.extend(encode_inst(71, &[7, 33, 0])); // Binding 0

        // OpTypeFloat %1 32
        spv.extend(encode_inst(22, &[1, 32]));
        // OpTypeImage %2 %1 Dim=Cube(3) Depth=0 Arrayed=0 MS=0 Sampled=1 Format=0
        spv.extend(encode_inst(25, &[2, 1, 3, 0, 0, 0, 1, 0]));
        // OpTypeSampledImage %3 %2
        spv.extend(encode_inst(27, &[3, 2]));
        // OpTypePointer %4 UniformConstant %3
        spv.extend(encode_inst(32, &[4, 0, 3]));
        // OpTypeVoid %8
        spv.extend(encode_inst(19, &[8]));
        // OpTypeFunction %9 %8
        spv.extend(encode_inst(33, &[9, 8]));
        // OpVariable %4 %7 UniformConstant
        spv.extend(encode_inst(59, &[4, 7, 0]));
        // Function
        spv.extend(encode_inst(54, &[8, 10, 0, 9]));
        spv.extend(encode_inst(248, &[11]));
        spv.extend(encode_inst(61, &[3, 12, 7])); // OpLoad
        spv.extend(encode_inst(253, &[]));
        spv.extend(encode_inst(56, &[]));

        let result = split_combined_samplers(&spv);
        assert_ne!(result.len(), spv.len(), "Output should differ after splitting Cube texture");

        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut found_sampled_image = false;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 86 { found_sampled_image = true; }
            pos += wc;
        }
        assert!(found_sampled_image, "Expected OpSampledImage for Cube texture split");
    }

    #[test]
    fn test_split_combined_samplers_binding_doubling() {
        // Non-combined-sampler bindings should get their Binding value doubled,
        // except PC_RING_BUFFER_BINDING which stays unchanged.
        let mut spv = spirv_header(30);

        // OpEntryPoint Fragment %20 "main" %7 %15
        {
            let wc: u32 = 7;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 4); // Fragment
            push_word(&mut spv, 20);
            push_word(&mut spv, 0x6E69616D);
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 7);  // combined sampler
            push_word(&mut spv, 15); // non-combined var (e.g. a UBO)
        }

        // Combined sampler at set=0, binding=2
        spv.extend(encode_inst(71, &[7, 34, 0]));
        spv.extend(encode_inst(71, &[7, 33, 2]));
        // Non-combined var at set=0, binding=5
        spv.extend(encode_inst(71, &[15, 34, 0]));
        spv.extend(encode_inst(71, &[15, 33, 5]));
        // PC ring buffer var at set=3, binding=120
        spv.extend(encode_inst(71, &[16, 34, 3]));
        spv.extend(encode_inst(71, &[16, 33, PC_RING_BUFFER_BINDING]));

        // Types for combined sampler
        spv.extend(encode_inst(22, &[1, 32])); // OpTypeFloat
        spv.extend(encode_inst(25, &[2, 1, 1, 0, 0, 0, 1, 0])); // OpTypeImage 2D
        spv.extend(encode_inst(27, &[3, 2])); // OpTypeSampledImage
        spv.extend(encode_inst(32, &[4, 0, 3])); // OpTypePointer UC SampledImage
        spv.extend(encode_inst(19, &[8])); // OpTypeVoid
        spv.extend(encode_inst(33, &[9, 8])); // OpTypeFunction
        // Non-combined var types
        spv.extend(encode_inst(32, &[14, 2, 8])); // OpTypePointer Uniform void (dummy)

        // Variables
        spv.extend(encode_inst(59, &[4, 7, 0]));  // combined sampler
        spv.extend(encode_inst(59, &[14, 15, 2])); // non-combined (Uniform)
        spv.extend(encode_inst(59, &[14, 16, 2])); // PC ring buffer (Uniform)

        // Function
        spv.extend(encode_inst(54, &[8, 20, 0, 9]));
        spv.extend(encode_inst(248, &[21]));
        spv.extend(encode_inst(61, &[3, 12, 7])); // OpLoad combined
        spv.extend(encode_inst(253, &[]));
        spv.extend(encode_inst(56, &[]));

        let result = split_combined_samplers(&spv);

        // Check bindings in the output:
        // combined sampler (var 7): image binding = 2*2+1 = 5, sampler binding = 2*2 = 4
        // non-combined (var 15): binding = 5*2 = 10
        // PC ring buffer (var 16): binding = 120 (unchanged)
        let total_words = result.len() / 4;
        let mut pos = 5;
        let mut binding_15 = None;
        let mut binding_16 = None;
        let mut binding_7 = None;
        while pos < total_words {
            let w0 = read_word(&result, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 71 && wc >= 4 {
                let target = read_word(&result, pos + 1);
                let deco = read_word(&result, pos + 2);
                let val = read_word(&result, pos + 3);
                if deco == 33 { // Binding
                    if target == 15 { binding_15 = Some(val); }
                    if target == 16 { binding_16 = Some(val); }
                    if target == 7 { binding_7 = Some(val); }
                }
            }
            pos += wc;
        }
        assert_eq!(binding_15, Some(10), "Non-combined binding 5 should become 10 (doubled)");
        assert_eq!(binding_16, Some(PC_RING_BUFFER_BINDING),
            "PC ring buffer binding should remain unchanged");
        assert_eq!(binding_7, Some(5), "Combined sampler binding 2: image should get 2*2+1=5");
    }

    // ==================== fix_nonfinite_literals tests ====================

    #[test]
    fn test_fix_nonfinite_literals_infinity() {
        let mut module = Module::default();
        // Add an f32 infinity literal to global_expressions.
        module.global_expressions.append(
            Expression::Literal(Literal::F32(f32::INFINITY)),
            naga::Span::UNDEFINED,
        );
        fix_nonfinite_literals(&mut module);
        // Should be clamped to f32::MAX.
        for (_, expr) in module.global_expressions.iter() {
            if let Expression::Literal(Literal::F32(v)) = *expr {
                assert_eq!(v, f32::MAX, "Infinity should be clamped to f32::MAX");
            }
        }
    }

    #[test]
    fn test_fix_nonfinite_literals_neg_infinity() {
        let mut module = Module::default();
        module.global_expressions.append(
            Expression::Literal(Literal::F32(f32::NEG_INFINITY)),
            naga::Span::UNDEFINED,
        );
        fix_nonfinite_literals(&mut module);
        for (_, expr) in module.global_expressions.iter() {
            if let Expression::Literal(Literal::F32(v)) = *expr {
                assert_eq!(v, f32::MIN, "Negative infinity should be clamped to f32::MIN");
            }
        }
    }

    #[test]
    fn test_fix_nonfinite_literals_nan() {
        let mut module = Module::default();
        module.global_expressions.append(
            Expression::Literal(Literal::F32(f32::NAN)),
            naga::Span::UNDEFINED,
        );
        fix_nonfinite_literals(&mut module);
        for (_, expr) in module.global_expressions.iter() {
            if let Expression::Literal(Literal::F32(v)) = *expr {
                assert_eq!(v, f32::MAX, "NaN should be clamped to f32::MAX");
            }
        }
    }

    #[test]
    fn test_fix_nonfinite_literals_normal_unchanged() {
        let mut module = Module::default();
        module.global_expressions.append(
            Expression::Literal(Literal::F32(42.0)),
            naga::Span::UNDEFINED,
        );
        module.global_expressions.append(
            Expression::Literal(Literal::F32(-1.5)),
            naga::Span::UNDEFINED,
        );
        fix_nonfinite_literals(&mut module);
        let vals: Vec<f32> = module.global_expressions.iter()
            .filter_map(|(_, expr)| match *expr {
                Expression::Literal(Literal::F32(v)) => Some(v),
                _ => None,
            })
            .collect();
        assert_eq!(vals, vec![42.0, -1.5], "Normal values should be unchanged");
    }

    #[test]
    fn test_fix_nonfinite_literals_f64_infinity() {
        let mut module = Module::default();
        module.global_expressions.append(
            Expression::Literal(Literal::F64(f64::INFINITY)),
            naga::Span::UNDEFINED,
        );
        module.global_expressions.append(
            Expression::Literal(Literal::F64(f64::NAN)),
            naga::Span::UNDEFINED,
        );
        fix_nonfinite_literals(&mut module);
        let vals: Vec<f64> = module.global_expressions.iter()
            .filter_map(|(_, expr)| match *expr {
                Expression::Literal(Literal::F64(v)) => Some(v),
                _ => None,
            })
            .collect();
        assert_eq!(vals, vec![f64::MAX, f64::MAX],
            "f64 infinity/NaN should be clamped to f64::MAX");
    }

    #[test]
    fn test_fix_nonfinite_literals_in_function() {
        // Verify that literals inside functions (not just global_expressions) are fixed.
        let mut module = Module::default();
        let mut func = naga::Function::default();
        func.expressions.append(
            Expression::Literal(Literal::F32(f32::INFINITY)),
            naga::Span::UNDEFINED,
        );
        module.functions.append(func, naga::Span::UNDEFINED);
        fix_nonfinite_literals(&mut module);
        for (_, func) in module.functions.iter() {
            for (_, expr) in func.expressions.iter() {
                if let Expression::Literal(Literal::F32(v)) = *expr {
                    assert_eq!(v, f32::MAX, "Infinity in function should be clamped");
                }
            }
        }
    }

    #[test]
    fn test_fix_nonfinite_literals_in_entry_point() {
        // Verify that literals inside entry point functions are fixed.
        let mut module = Module::default();
        let mut ep = naga::EntryPoint {
            name: "main".to_string(),
            stage: naga::ShaderStage::Vertex,
            early_depth_test: None,
            workgroup_size: [0, 0, 0],
            workgroup_size_overrides: None,
            function: naga::Function::default(),
            mesh_info: None,
            task_payload: None,
        };
        ep.function.expressions.append(
            Expression::Literal(Literal::F32(f32::NEG_INFINITY)),
            naga::Span::UNDEFINED,
        );
        module.entry_points.push(ep);
        fix_nonfinite_literals(&mut module);
        for ep in &module.entry_points {
            for (_, expr) in ep.function.expressions.iter() {
                if let Expression::Literal(Literal::F32(v)) = *expr {
                    assert_eq!(v, f32::MIN, "Neg infinity in entry point should be clamped");
                }
            }
        }
    }

    // ==================== flatten_binding_arrays tests ====================

    #[test]
    fn test_flatten_binding_arrays_known_size() {
        use std::num::NonZeroU32;
        let mut module = Module::default();

        // First insert a base type for the binding array to reference.
        let base_ty = module.types.insert(
            Type {
                name: Some("sampler".to_string()),
                inner: TypeInner::Sampler { comparison: false },
            },
            naga::Span::UNDEFINED,
        );

        // Insert a BindingArray type with size > 1.
        let _ba_handle = module.types.insert(
            Type {
                name: Some("binding_array".to_string()),
                inner: TypeInner::BindingArray {
                    base: base_ty,
                    size: naga::ArraySize::Constant(NonZeroU32::new(8).unwrap()),
                },
            },
            naga::Span::UNDEFINED,
        );

        flatten_binding_arrays(&mut module);

        // Verify the binding array was flattened to size 1.
        let mut found = false;
        for (_, ty) in module.types.iter() {
            if let TypeInner::BindingArray { size, .. } = ty.inner {
                found = true;
                match size {
                    naga::ArraySize::Constant(n) => {
                        assert_eq!(n.get(), 1, "BindingArray should be flattened to size 1");
                    }
                    _ => panic!("Expected Constant size after flattening"),
                }
            }
        }
        assert!(found, "Should still have a BindingArray type");
    }

    #[test]
    fn test_flatten_binding_arrays_dynamic_size() {
        let mut module = Module::default();

        let base_ty = module.types.insert(
            Type {
                name: Some("sampler".to_string()),
                inner: TypeInner::Sampler { comparison: false },
            },
            naga::Span::UNDEFINED,
        );

        let _ba_handle = module.types.insert(
            Type {
                name: Some("dyn_array".to_string()),
                inner: TypeInner::BindingArray {
                    base: base_ty,
                    size: naga::ArraySize::Dynamic,
                },
            },
            naga::Span::UNDEFINED,
        );

        flatten_binding_arrays(&mut module);

        let mut found = false;
        for (_, ty) in module.types.iter() {
            if let TypeInner::BindingArray { size, .. } = ty.inner {
                found = true;
                match size {
                    naga::ArraySize::Constant(n) => {
                        assert_eq!(n.get(), 1, "Dynamic BindingArray should be flattened to size 1");
                    }
                    _ => panic!("Expected Constant size after flattening dynamic array"),
                }
            }
        }
        assert!(found, "Should still have a BindingArray type");
    }

    #[test]
    fn test_flatten_binding_arrays_size_one_unchanged() {
        use std::num::NonZeroU32;
        let mut module = Module::default();

        let base_ty = module.types.insert(
            Type {
                name: Some("sampler".to_string()),
                inner: TypeInner::Sampler { comparison: false },
            },
            naga::Span::UNDEFINED,
        );

        let _ba_handle = module.types.insert(
            Type {
                name: Some("already_one".to_string()),
                inner: TypeInner::BindingArray {
                    base: base_ty,
                    size: naga::ArraySize::Constant(NonZeroU32::new(1).unwrap()),
                },
            },
            naga::Span::UNDEFINED,
        );

        flatten_binding_arrays(&mut module);

        for (_, ty) in module.types.iter() {
            if let TypeInner::BindingArray { size, .. } = ty.inner {
                match size {
                    naga::ArraySize::Constant(n) => {
                        assert_eq!(n.get(), 1, "Size-1 array should remain at 1");
                    }
                    _ => panic!("Size should remain Constant(1)"),
                }
            }
        }
    }

    // ==================== fix_writeonly_storage tests ====================

    #[test]
    fn test_fix_writeonly_storage_store_only() {
        let mut module = Module::default();

        // We need a type to reference.
        let ty = module.types.insert(
            Type {
                name: Some("buf".to_string()),
                inner: TypeInner::Scalar(naga::Scalar { kind: naga::ScalarKind::Uint, width: 4 }),
            },
            naga::Span::UNDEFINED,
        );

        // Storage var with STORE only (no LOAD).
        module.global_variables.append(
            naga::GlobalVariable {
                name: Some("write_buf".to_string()),
                space: AddressSpace::Storage {
                    access: StorageAccess::STORE,
                },
                binding: Some(naga::ResourceBinding { group: 0, binding: 0 }),
                ty,
                init: None,
            },
            naga::Span::UNDEFINED,
        );

        fix_writeonly_storage(&mut module);

        for (_, var) in module.global_variables.iter() {
            if let AddressSpace::Storage { access } = var.space {
                assert!(
                    access.contains(StorageAccess::LOAD),
                    "STORE-only var should get LOAD added"
                );
                assert!(
                    access.contains(StorageAccess::STORE),
                    "STORE should still be present"
                );
            }
        }
    }

    #[test]
    fn test_fix_writeonly_storage_load_store_unchanged() {
        let mut module = Module::default();

        let ty = module.types.insert(
            Type {
                name: Some("buf".to_string()),
                inner: TypeInner::Scalar(naga::Scalar { kind: naga::ScalarKind::Uint, width: 4 }),
            },
            naga::Span::UNDEFINED,
        );

        // Storage var with both LOAD and STORE.
        module.global_variables.append(
            naga::GlobalVariable {
                name: Some("rw_buf".to_string()),
                space: AddressSpace::Storage {
                    access: StorageAccess::LOAD | StorageAccess::STORE,
                },
                binding: Some(naga::ResourceBinding { group: 0, binding: 0 }),
                ty,
                init: None,
            },
            naga::Span::UNDEFINED,
        );

        fix_writeonly_storage(&mut module);

        for (_, var) in module.global_variables.iter() {
            if let AddressSpace::Storage { access } = var.space {
                assert_eq!(
                    access,
                    StorageAccess::LOAD | StorageAccess::STORE,
                    "LOAD|STORE should be unchanged"
                );
            }
        }
    }

    #[test]
    fn test_fix_writeonly_storage_non_storage_unchanged() {
        let mut module = Module::default();

        let ty = module.types.insert(
            Type {
                name: Some("buf".to_string()),
                inner: TypeInner::Scalar(naga::Scalar { kind: naga::ScalarKind::Uint, width: 4 }),
            },
            naga::Span::UNDEFINED,
        );

        // Uniform var (not Storage) - should not be affected.
        module.global_variables.append(
            naga::GlobalVariable {
                name: Some("ubo".to_string()),
                space: AddressSpace::Uniform,
                binding: Some(naga::ResourceBinding { group: 0, binding: 0 }),
                ty,
                init: None,
            },
            naga::Span::UNDEFINED,
        );

        fix_writeonly_storage(&mut module);

        for (_, var) in module.global_variables.iter() {
            assert_eq!(var.space, AddressSpace::Uniform, "Uniform should be unchanged");
        }
    }

    // ==================== Additional end-to-end spirv_to_wgsl tests ====================

    /// Assemble a minimal valid SPIR-V compute shader.
    fn assemble_minimal_compute_shader() -> Vec<u8> {
        let mut spv = Vec::new();

        // Header
        push_word(&mut spv, 0x07230203);
        push_word(&mut spv, 0x00010300);
        push_word(&mut spv, 0);
        push_word(&mut spv, 10);
        push_word(&mut spv, 0);

        // OpCapability Shader
        spv.extend(encode_inst(17, &[1]));

        // OpMemoryModel Logical GLSL450
        spv.extend(encode_inst(14, &[0, 1]));

        // OpEntryPoint GLCompute %5 "main"
        {
            let wc: u32 = 5;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 5); // GLCompute execution model
            push_word(&mut spv, 5); // main function
            push_word(&mut spv, 0x6E69616D); // "main"
            push_word(&mut spv, 0x00000000);
        }

        // OpExecutionMode %5 LocalSize 1 1 1
        spv.extend(encode_inst(16, &[5, 17, 1, 1, 1])); // 17 = LocalSize

        // OpTypeVoid %1
        spv.extend(encode_inst(19, &[1]));
        // OpTypeFunction %2 %1
        spv.extend(encode_inst(33, &[2, 1]));

        // OpFunction %1 %5 None %2
        spv.extend(encode_inst(54, &[1, 5, 0, 2]));
        // OpLabel %6
        spv.extend(encode_inst(248, &[6]));
        // OpReturn
        spv.extend(encode_inst(253, &[]));
        // OpFunctionEnd
        spv.extend(encode_inst(56, &[]));

        spv
    }

    #[test]
    fn test_end_to_end_compute_shader() {
        let spirv = assemble_minimal_compute_shader();

        // Run through the full pipeline.
        let spirv_bytes = freeze_spec_constant_ops(&spirv);
        let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
        let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
        let spirv_bytes = infer_readonly_storage(&spirv_bytes);
        let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
        let spirv_bytes = split_combined_samplers(&spirv_bytes);
        let (spirv_bytes, _) = fix_depth2_images(&spirv_bytes);

        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let module = spv::parse_u8_slice(&spirv_bytes, &opts);
        assert!(module.is_ok(), "Compute SPIR-V parse failed: {:?}", module.err());
        let mut module = module.unwrap();

        fix_writeonly_storage(&mut module);
        fix_nonfinite_literals(&mut module);
        strip_point_size(&mut module);
        flatten_binding_arrays(&mut module);

        let info = Validator::new(ValidationFlags::all(), Capabilities::all())
            .validate(&module);
        assert!(info.is_ok(), "Compute validation failed: {:?}", info.err());
        let info = info.unwrap();

        use naga::back::PipelineConstants;
        use naga::back::pipeline_constants::process_overrides;
        let pipeline_constants = PipelineConstants::default();
        let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
            .expect("process_overrides failed");
        let mut module = module.into_owned();
        module.overrides = naga::Arena::new();

        use naga::back::wgsl;
        let wgsl_result = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty());
        assert!(wgsl_result.is_ok(), "Compute WGSL write failed: {:?}", wgsl_result.err());
        let wgsl = wgsl_result.unwrap();

        assert!(wgsl.contains("@compute"), "Expected @compute in WGSL output");
        assert!(wgsl.contains("@workgroup_size"), "Expected @workgroup_size in WGSL output");
    }

    /// Assemble a minimal compute shader with a storage buffer (tests infer_readonly).
    fn assemble_compute_with_storage_buffer() -> Vec<u8> {
        let mut spv = Vec::new();

        // Header
        push_word(&mut spv, 0x07230203);
        push_word(&mut spv, 0x00010300);
        push_word(&mut spv, 0);
        push_word(&mut spv, 30);
        push_word(&mut spv, 0);

        // === Section 1: Capabilities ===
        // OpCapability Shader
        spv.extend(encode_inst(17, &[1]));

        // === Section 2: Memory model ===
        // OpMemoryModel Logical GLSL450
        spv.extend(encode_inst(14, &[0, 1]));

        // === Section 3: Entry points ===
        // OpEntryPoint GLCompute %10 "main" %7
        {
            let wc: u32 = 6;
            push_word(&mut spv, (wc << 16) | 15);
            push_word(&mut spv, 5); // GLCompute
            push_word(&mut spv, 10);
            push_word(&mut spv, 0x6E69616D); // "main"
            push_word(&mut spv, 0x00000000);
            push_word(&mut spv, 7); // interface var (storage buffer)
        }

        // === Section 4: Execution modes ===
        // OpExecutionMode %10 LocalSize 1 1 1
        spv.extend(encode_inst(16, &[10, 17, 1, 1, 1]));

        // === Section 5: Annotations (all decorations) ===
        // OpDecorate %7 DescriptorSet 0
        spv.extend(encode_inst(71, &[7, 34, 0]));
        // OpDecorate %7 Binding 0
        spv.extend(encode_inst(71, &[7, 33, 0]));
        // OpDecorate %6 Block
        spv.extend(encode_inst(71, &[6, 2]));
        // OpDecorate %5 ArrayStride 4
        spv.extend(encode_inst(71, &[5, 6, 4])); // decoration 6 = ArrayStride
        // OpMemberDecorate %6 0 Offset 0
        spv.extend(encode_inst(72, &[6, 0, 35, 0])); // 72=OpMemberDecorate, 35=Offset

        // === Section 6: Type declarations ===
        // OpTypeVoid %1
        spv.extend(encode_inst(19, &[1]));
        // OpTypeFloat %3 32
        spv.extend(encode_inst(22, &[3, 32]));
        // OpTypeInt %11 32 0
        spv.extend(encode_inst(21, &[11, 32, 0]));
        // OpTypeRuntimeArray %5 %3  (op=29)
        spv.extend(encode_inst(29, &[5, 3]));
        // OpTypeStruct %6 %5
        spv.extend(encode_inst(30, &[6, 5]));
        // OpTypeFunction %2 %1
        spv.extend(encode_inst(33, &[2, 1]));
        // OpTypePointer %4 StorageBuffer %6  (StorageBuffer=12)
        spv.extend(encode_inst(32, &[4, 12, 6]));
        // OpTypePointer %13 StorageBuffer %3
        spv.extend(encode_inst(32, &[13, 12, 3]));

        // === Section 7: Global variables and constants ===
        // OpVariable %4 %7 StorageBuffer
        spv.extend(encode_inst(59, &[4, 7, 12]));
        // OpConstant %3 %8 1.0f
        spv.extend(encode_inst(43, &[3, 8, 0x3F800000]));
        // OpConstant %11 %12 0
        spv.extend(encode_inst(43, &[11, 12, 0]));

        // === Section 8: Functions ===
        // OpFunction %1 %10 None %2
        spv.extend(encode_inst(54, &[1, 10, 0, 2]));
        // OpLabel %14
        spv.extend(encode_inst(248, &[14]));
        // OpAccessChain %13 %15 %7 %12 %12  (get pointer to buf.data[0])
        spv.extend(encode_inst(65, &[13, 15, 7, 12, 12]));
        // OpLoad %3 %16 %15  (read from storage buffer - so it's read-only!)
        spv.extend(encode_inst(61, &[3, 16, 15]));
        // OpReturn
        spv.extend(encode_inst(253, &[]));
        // OpFunctionEnd
        spv.extend(encode_inst(56, &[]));

        spv
    }

    #[test]
    fn test_end_to_end_storage_buffer_infer_readonly() {
        let spirv = assemble_compute_with_storage_buffer();

        // Run through the full pipeline.
        let spirv_bytes = freeze_spec_constant_ops(&spirv);
        let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
        let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
        let spirv_bytes = infer_readonly_storage(&spirv_bytes);

        // Verify NonWritable was added (the buffer is only read, never stored)
        let total_words = spirv_bytes.len() / 4;
        let mut pos = 5;
        let mut found_nonwritable = false;
        while pos < total_words {
            let w0 = read_word(&spirv_bytes, pos);
            let wc = (w0 >> 16) as usize;
            let op = (w0 & 0xFFFF) as u16;
            if wc == 0 || pos + wc > total_words { break; }
            if op == 71 && wc >= 3 {
                let target = read_word(&spirv_bytes, pos + 1);
                let deco = read_word(&spirv_bytes, pos + 2);
                if target == 7 && deco == DECO_NON_WRITABLE {
                    found_nonwritable = true;
                }
            }
            pos += wc;
        }
        assert!(found_nonwritable, "Storage buffer var 7 (read-only) should get NonWritable");

        // Continue the pipeline to full WGSL generation.
        let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
        let spirv_bytes = split_combined_samplers(&spirv_bytes);
        let (spirv_bytes, _) = fix_depth2_images(&spirv_bytes);

        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let module = spv::parse_u8_slice(&spirv_bytes, &opts);
        assert!(module.is_ok(), "Storage buffer SPIR-V parse failed: {:?}", module.err());
        let mut module = module.unwrap();

        fix_writeonly_storage(&mut module);
        fix_nonfinite_literals(&mut module);
        strip_point_size(&mut module);
        flatten_binding_arrays(&mut module);

        let info = Validator::new(ValidationFlags::all(), Capabilities::all())
            .validate(&module);
        assert!(info.is_ok(), "Storage buffer validation failed: {:?}", info.err());
        let info = info.unwrap();

        use naga::back::PipelineConstants;
        use naga::back::pipeline_constants::process_overrides;
        let pipeline_constants = PipelineConstants::default();
        let (module, info) = process_overrides(&module, &info, None, &pipeline_constants)
            .expect("process_overrides failed");
        let mut module = module.into_owned();
        module.overrides = naga::Arena::new();

        use naga::back::wgsl;
        let wgsl_result = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty());
        assert!(wgsl_result.is_ok(), "Storage buffer WGSL write failed: {:?}", wgsl_result.err());
        let wgsl = wgsl_result.unwrap();

        // The buffer should be read-only since NonWritable was inferred.
        assert!(wgsl.contains("var<storage"), "Expected var<storage> in WGSL output");
        assert!(wgsl.contains("@compute"), "Expected @compute in WGSL output");
    }

    #[test]
    fn test_end_to_end_invalid_spirv_returns_error() {
        // Completely invalid SPIR-V (wrong magic, garbage data) should return
        // an error, not panic.
        let garbage: &[u8] = &[0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];

        // spirv_to_wgsl expects JsError, but we can test the pipeline directly.
        // The pipeline starts with freeze_spec_constant_ops, which won't crash.
        // The actual error should come from naga's parse_u8_slice.
        let spirv_bytes = freeze_spec_constant_ops(garbage);
        let spirv_bytes = rewrite_copy_logical(&spirv_bytes);
        let spirv_bytes = rewrite_terminate_invocation(&spirv_bytes);
        let spirv_bytes = infer_readonly_storage(&spirv_bytes);
        let spirv_bytes = convert_push_constants_to_uniforms(&spirv_bytes);
        let spirv_bytes = split_combined_samplers(&spirv_bytes);
        let (spirv_bytes, _) = fix_depth2_images(&spirv_bytes);

        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let result = spv::parse_u8_slice(&spirv_bytes, &opts);
        assert!(result.is_err(), "Invalid SPIR-V should fail parsing");
    }

    #[test]
    fn test_end_to_end_empty_spirv_returns_error() {
        // Empty byte slice should fail at parsing.
        let empty: &[u8] = &[];
        let opts = spv::Options {
            adjust_coordinate_space: true,
            strict_capabilities: false,
            block_ctx_dump_prefix: None,
        };
        let result = spv::parse_u8_slice(empty, &opts);
        assert!(result.is_err(), "Empty SPIR-V should fail parsing");
    }
}
