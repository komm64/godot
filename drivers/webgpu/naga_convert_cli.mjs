#!/usr/bin/env node
/**
 * naga_convert_cli.mjs — Build-time SPIR-V → WGSL converter using the
 * prebuilt naga WASM module.
 *
 * Usage:
 *   node naga_convert_cli.mjs <spirv_file>          # single file → stdout
 *   node naga_convert_cli.mjs --batch f1.spv f2.spv  # batch → JSON to stdout
 *
 * In batch mode, outputs a JSON object mapping each input file path to its
 * WGSL output string (or to an object { "error": "..." } on failure).
 */

import { readFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const wasmPath = join(__dirname, 'naga-converter', 'prebuilt', 'naga_wasm_bg.wasm');

// ── WASM runtime glue (mirrors engine.js wasm-bindgen helpers) ──────────

let nagaWasm = null;
let cachedMem = null;
let WASM_VEC_LEN = 0;

function mem() {
	if (cachedMem === null || cachedMem.byteLength === 0) {
		cachedMem = new Uint8Array(nagaWasm.memory.buffer);
	}
	return cachedMem;
}

function passArray8(arg) {
	const ptr = nagaWasm.__wbindgen_malloc(arg.length, 1) >>> 0;
	mem().set(arg, ptr);
	WASM_VEC_LEN = arg.length;
	return ptr;
}

const cachedDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
cachedDecoder.decode();

function getString(ptr, len) {
	ptr = ptr >>> 0;
	return cachedDecoder.decode(mem().subarray(ptr, ptr + len));
}

function takeFromTable(idx) {
	const val = nagaWasm.__wbindgen_externrefs.get(idx);
	nagaWasm.__externref_table_dealloc(idx);
	return val;
}

// ── Load naga WASM ──────────────────────────────────────────────────────

const wasmBytes = readFileSync(wasmPath);

const imports = {
	'./naga_converter_bg.js': {
		__wbg_Error_83742b46f01ce22d: function (arg0, arg1) {
			return new Error(getString(arg0, arg1));
		},
		__wbg_log_2173688eed3d74ed: function (_arg0, _arg1) {
			// Suppress naga's console.log during batch conversion.
		},
		__wbindgen_init_externref_table: function () {
			const table = nagaWasm.__wbindgen_externrefs;
			const offset = table.grow(4);
			table.set(0, undefined);
			table.set(offset + 0, undefined);
			table.set(offset + 1, null);
			table.set(offset + 2, true);
			table.set(offset + 3, false);
		},
	},
};

const mod = new WebAssembly.Module(wasmBytes);
const inst = new WebAssembly.Instance(mod, imports);
nagaWasm = inst.exports;
cachedMem = null;
nagaWasm.__wbindgen_start();

// ── Conversion function ─────────────────────────────────────────────────

function convertSpirvToWgsl(spirvBytes) {
	const ptr0 = passArray8(spirvBytes);
	const len0 = WASM_VEC_LEN;
	const ret = nagaWasm.spirv_to_wgsl(ptr0, len0);
	const ptr2 = ret[0];
	const len2 = ret[1];
	if (ret[3]) {
		const err = takeFromTable(ret[2]);
		throw new Error(err && err.message ? err.message : String(err));
	}
	const wgsl = getString(ptr2, len2);
	nagaWasm.__wbindgen_free(ptr2, len2, 1);
	return wgsl;
}

/**
 * Convert SPIR-V to WGSL with override declarations preserved.
 * Specialization constants become `@id(N) override` declarations in the WGSL
 * output, and derived expressions (OpSpecConstantOp, OpSpecConstantComposite)
 * are emitted as function-body expressions referencing the overrides.
 */
function convertSpirvToWgslWithOverrides(spirvBytes) {
	const ptr0 = passArray8(spirvBytes);
	const len0 = WASM_VEC_LEN;
	const ret = nagaWasm.spirv_to_wgsl_with_overrides(ptr0, len0);
	const ptr2 = ret[0];
	const len2 = ret[1];
	if (ret[3]) {
		const err = takeFromTable(ret[2]);
		throw new Error(err && err.message ? err.message : String(err));
	}
	const wgsl = getString(ptr2, len2);
	nagaWasm.__wbindgen_free(ptr2, len2, 1);
	return wgsl;
}

// ── CLI entry point ─────────────────────────────────────────────────────

const args = process.argv.slice(2);

if (args.length === 0) {
	console.error('Usage: naga_convert_cli.mjs [--batch] [--override] <spirv_file> [spirv_file ...]');
	console.error('  --override  Preserve specialization constants as WGSL override declarations');
	process.exit(1);
}

// Parse flags.
const flags = new Set();
const positional = [];
for (const a of args) {
	if (a.startsWith('--')) flags.add(a);
	else positional.push(a);
}
const batchMode = flags.has('--batch');
const overrideMode = flags.has('--override');
const files = positional;

const converter = overrideMode ? convertSpirvToWgslWithOverrides : convertSpirvToWgsl;

if (batchMode) {
	// Batch mode: convert all files, output JSON.
	const results = {};
	for (const file of files) {
		try {
			const spirv = readFileSync(file);
			results[file] = converter(spirv);
		} catch (e) {
			results[file] = { error: e.message };
		}
	}
	process.stdout.write(JSON.stringify(results));
} else {
	// Single file mode: output WGSL to stdout.
	if (files.length === 0) {
		console.error('No input file specified.');
		process.exit(1);
	}
	try {
		const spirv = readFileSync(files[0]);
		const wgsl = converter(spirv);
		process.stdout.write(wgsl);
	} catch (e) {
		console.error('SPIR-V → WGSL error:', e.message);
		process.exit(1);
	}
}
