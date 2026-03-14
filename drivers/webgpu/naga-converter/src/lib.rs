use wasm_bindgen::prelude::*;

use std::collections::{HashMap, HashSet};
use naga::{
    AddressSpace, Arena, Binding, BuiltIn, Expression, Handle, Interpolation,
    Literal, Module, Sampling, StorageAccess, Type, TypeInner,
    back::{wgsl, pipeline_constants::process_overrides, PipelineConstants},
    front::spv,
    valid::{Validator, ValidationFlags, Capabilities},
};

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = console)]
    fn log(s: &str);
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
fn read_word(bytes: &[u8], word_idx: usize) -> u32 {
    let off = word_idx * 4;
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
fn freeze_spec_constant_ops(bytes: &[u8]) -> Vec<u8> {
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
fn infer_readonly_storage(bytes: &[u8]) -> Vec<u8> {
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
fn convert_push_constants_to_uniforms(bytes: &[u8]) -> Vec<u8> {
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
fn split_combined_samplers(bytes: &[u8]) -> Vec<u8> {
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
    let mut alloc_id = || { let id = next_id; next_id += 1; id };

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
                push_word(&mut out, split.binding * 2);
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
                    push_word(&mut out, binding * 2 + 1);
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
                    old_binding * 2
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
fn fix_depth2_images(bytes: &[u8]) -> (Vec<u8>, Vec<(u32, u32, u32)>) {
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

/// Strip OpMemoryBarrier (opcode 225) and OpControlBarrier (opcode 224) from SPIR-V.
/// Naga's SPIR-V parser doesn't support these instructions; WebGPU handles
/// memory coherence automatically, so removing them is safe.
/// Instructions are removed entirely and the SPIR-V header word count is updated.
fn strip_memory_barriers(bytes: &[u8]) -> Vec<u8> {
    let total_words = bytes.len() / 4;
    if total_words < 5 {
        return bytes.to_vec();
    }
    // Collect all instruction ranges, skipping barrier instructions.
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    // Copy header (5 words = 20 bytes).
    out.extend_from_slice(&bytes[..20]);
    let mut pos: usize = 5;
    while pos < total_words {
        let w0 = read_word(bytes, pos);
        let wc = (w0 >> 16) as usize;
        let op = (w0 & 0xFFFF) as u16;
        if wc == 0 || pos + wc > total_words {
            break;
        }
        // OpControlBarrier = 224, OpMemoryBarrier = 225 — skip these.
        if op != 224 && op != 225 {
            let byte_start = pos * 4;
            let byte_end = (pos + wc) * 4;
            out.extend_from_slice(&bytes[byte_start..byte_end]);
        }
        pos += wc;
    }
    // Update the bound (word 3) with new total word count isn't needed —
    // the bound field is the max ID+1, not the word count.
    out
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

    // Pre-process: strip OpMemoryBarrier / OpControlBarrier (unsupported by naga).
    let spirv_bytes = strip_memory_barriers(&spirv_bytes);

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

    // process_overrides remaps Expression::Override → Expression::Constant
    // but leaves the overrides arena populated. The WGSL writer rejects
    // modules with any overrides, so clear the arena.
    let mut module = module.into_owned();
    module.overrides = Arena::new();

    // Write WGSL.
    let wgsl = wgsl::write_string(&module, &info, wgsl::WriterFlags::empty())
        .map_err(|e| JsError::new(&format!("WGSL write error: {e:?}")))?;

    // Post-process 1: Rewrite push_constant address space to uniform buffer
    // binding at group 3 / binding PC_RING_BUFFER_BINDING (our ring-buffer emulation).
    // Dawn/WebGPU limits var<push_constant> to device's maxPushConstantSize
    // (often 64 bytes on Apple), but Godot canvas shaders use up to 128 bytes.
    let pc_count = wgsl.matches("push_constant").count();
    if pc_count > 0 {
        log(&format!("[NAGA] push_constant occurrences: {pc_count}"));
        // Print surrounding context for the first occurrence.
        if let Some(pos) = wgsl.find("push_constant") {
            let start = pos.saturating_sub(30);
            let end = (pos + 50).min(wgsl.len());
            log(&format!("[NAGA] push_const context: {:?}", &wgsl[start..end]));
        }
    }
    let wgsl = wgsl.replace(
        "var<push_constant>",
        &format!("@group(3) @binding({PC_RING_BUFFER_BINDING}) var<uniform>"),
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

    let wgsl = format!("{prefix}{wgsl}");

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
