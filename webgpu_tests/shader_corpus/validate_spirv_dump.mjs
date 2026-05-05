/**
 * SPIR-V Dump Validator
 *
 * Validates all .spv files in a given directory through naga-converter WASM.
 * Designed to run after `GODOT_DUMP_SPIRV=<dir>` produces SPIR-V files during
 * engine shader compilation.
 *
 * Usage:
 *   node validate_spirv_dump.mjs <spirv-directory>
 *   node validate_spirv_dump.mjs ./spirv_dump/
 *
 * Exit codes:
 *   0 = all shaders converted successfully
 *   1 = one or more shaders failed conversion
 */

import { readFileSync, readdirSync, writeFileSync, mkdirSync } from 'fs';
import { join, basename } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const NAGA_WASM_PATH = join(__dirname, '../../drivers/webgpu/naga-converter/out/naga_converter_bg.wasm');

// ─── WASM Bindings (same as run_tests.mjs) ────────────────────────────────────

let wasm;
let cachedUint8ArrayMemory0 = null;
let WASM_VECTOR_LEN = 0;

function getUint8ArrayMemory0() {
    if (cachedUint8ArrayMemory0 === null || cachedUint8ArrayMemory0.byteLength === 0) {
        cachedUint8ArrayMemory0 = new Uint8Array(wasm.memory.buffer);
    }
    return cachedUint8ArrayMemory0;
}

function passArray8ToWasm0(arg, malloc) {
    const ptr = malloc(arg.length * 1, 1) >>> 0;
    getUint8ArrayMemory0().set(arg, ptr / 1);
    WASM_VECTOR_LEN = arg.length;
    return ptr;
}

let cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
cachedTextDecoder.decode();

function getStringFromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    return cachedTextDecoder.decode(getUint8ArrayMemory0().subarray(ptr, ptr + len));
}

function takeFromExternrefTable0(idx) {
    const value = wasm.__wbindgen_externrefs.get(idx);
    wasm.__externref_table_dealloc(idx);
    return value;
}

function spirv_to_wgsl(spirv_bytes) {
    let deferred3_0;
    let deferred3_1;
    try {
        const ptr0 = passArray8ToWasm0(spirv_bytes, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.spirv_to_wgsl(ptr0, len0);
        var ptr2 = ret[0];
        var len2 = ret[1];
        if (ret[3]) {
            ptr2 = 0; len2 = 0;
            throw takeFromExternrefTable0(ret[2]);
        }
        deferred3_0 = ptr2;
        deferred3_1 = len2;
        return getStringFromWasm0(ptr2, len2);
    } finally {
        wasm.__wbindgen_free(deferred3_0, deferred3_1, 1);
    }
}

function initWasm(wasmBytes) {
    const imports = {
        './naga_converter_bg.js': {
            __wbg_Error_83742b46f01ce22d: function(arg0, arg1) {
                return Error(getStringFromWasm0(arg0, arg1));
            },
            __wbg_log_2173688eed3d74ed: function(arg0, arg1) {
                if (process.env.VERBOSE) {
                    console.log('[naga]', getStringFromWasm0(arg0, arg1));
                }
            },
            __wbindgen_init_externref_table: function() {
                const table = wasm.__wbindgen_externrefs;
                const offset = table.grow(4);
                table.set(0, undefined);
                table.set(offset + 0, undefined);
                table.set(offset + 1, null);
                table.set(offset + 2, true);
                table.set(offset + 3, false);
            },
        },
    };

    const module = new WebAssembly.Module(wasmBytes);
    const instance = new WebAssembly.Instance(module, imports);
    wasm = instance.exports;
    cachedUint8ArrayMemory0 = null;
    wasm.__wbindgen_start();
}

// ─── Main ─────────────────────────────────────────────────────────────────────

function main() {
    const spvDir = process.argv[2];
    if (!spvDir) {
        console.error('Usage: node validate_spirv_dump.mjs <spirv-directory>');
        process.exit(1);
    }

    console.log('╔══════════════════════════════════════════════════════════╗');
    console.log('║   SPIR-V Dump Validator (naga-converter)                 ║');
    console.log('╚══════════════════════════════════════════════════════════╝\n');

    // Load naga WASM
    console.log(`Loading naga WASM from: ${NAGA_WASM_PATH}`);
    const wasmBytes = readFileSync(NAGA_WASM_PATH);
    console.log(`  WASM size: ${(wasmBytes.length / 1024).toFixed(1)} KB`);

    try {
        initWasm(wasmBytes);
        console.log('  Naga WASM initialized successfully.\n');
    } catch (e) {
        console.error('FATAL: Failed to initialize naga WASM:', e.message);
        process.exit(1);
    }

    // Find all .spv files recursively
    let spvFiles;
    try {
        spvFiles = readdirSync(spvDir).filter(f => f.endsWith('.spv')).sort();
    } catch (e) {
        console.error(`ERROR: Cannot read directory: ${spvDir}`);
        console.error(e.message);
        process.exit(1);
    }

    if (spvFiles.length === 0) {
        console.error(`ERROR: No .spv files found in ${spvDir}`);
        console.error('Did you run with GODOT_DUMP_SPIRV set?');
        process.exit(1);
    }

    console.log(`Found ${spvFiles.length} SPIR-V files in ${spvDir}\n`);

    let passed = 0;
    let failed = 0;
    const failures = [];

    for (const spvFile of spvFiles) {
        const spvPath = join(spvDir, spvFile);
        const spvBytes = readFileSync(spvPath);

        process.stdout.write(`  ${spvFile.padEnd(55)}`);

        const startTime = performance.now();
        try {
            const wgsl = spirv_to_wgsl(new Uint8Array(spvBytes));
            const elapsed = (performance.now() - startTime).toFixed(1);
            console.log(`PASS  (${elapsed}ms, ${wgsl.length} chars)`);
            passed++;
        } catch (e) {
            const elapsed = (performance.now() - startTime).toFixed(1);
            console.log(`FAIL  (${elapsed}ms)`);
            console.log(`         ${e.message || e}`);
            failed++;
            failures.push({ file: spvFile, error: e.message || String(e) });
        }
    }

    // Summary
    console.log('\n' + '─'.repeat(60));
    console.log(`\nResults: ${passed} passed, ${failed} failed, ${spvFiles.length} total`);

    if (failures.length > 0) {
        console.log('\nFailed shaders:');
        for (const f of failures) {
            console.log(`  - ${f.file}: ${f.error.substring(0, 100)}`);
        }
    }

    // Write report
    const report = {
        timestamp: new Date().toISOString(),
        spirv_directory: spvDir,
        total: spvFiles.length,
        passed,
        failed,
        failures,
    };

    const reportPath = join(spvDir, 'validation_report.json');
    writeFileSync(reportPath, JSON.stringify(report, null, 2));
    console.log(`\nReport: ${reportPath}`);

    process.exit(failed > 0 ? 1 : 0);
}

main();
