/**
 * Shader Corpus Test: SPIR-V → WGSL Validation
 *
 * Loads the naga-converter WASM module and validates that all test SPIR-V files
 * convert to valid WGSL. Tests the same pipeline used at runtime in the browser.
 *
 * Usage: node run_tests.mjs
 */

import { readFileSync, readdirSync, writeFileSync, mkdirSync } from 'fs';
import { join, basename } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Paths
const NAGA_WASM_PATH = join(__dirname, '../../drivers/webgpu/naga-converter/out/naga_converter_bg.wasm');
const FIXTURES_DIR = join(__dirname, 'fixtures');
const RESULTS_DIR = join(__dirname, 'results');

// Inline the wasm-bindgen glue (adapted from naga_converter.js for Node.js)
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
                // Suppress naga console.log during tests unless verbose
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

// ─── Test Runner ───────────────────────────────────────────────────────────

function runTests() {
    console.log('╔══════════════════════════════════════════════════════════╗');
    console.log('║     Shader Corpus Test: SPIR-V → WGSL Validation        ║');
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

    // Find all .spv files
    mkdirSync(RESULTS_DIR, { recursive: true });
    const spvFiles = readdirSync(FIXTURES_DIR).filter(f => f.endsWith('.spv')).sort();

    if (spvFiles.length === 0) {
        console.error('ERROR: No .spv files found in fixtures/. Run compile_fixtures.sh first.');
        process.exit(1);
    }

    console.log(`Found ${spvFiles.length} SPIR-V test files:\n`);

    let passed = 0;
    let failed = 0;
    const results = [];

    for (const spvFile of spvFiles) {
        const name = basename(spvFile, '.spv');
        const spvPath = join(FIXTURES_DIR, spvFile);
        const spvBytes = readFileSync(spvPath);

        process.stdout.write(`  [TEST] ${name.padEnd(30)}`);

        const startTime = performance.now();
        try {
            const wgsl = spirv_to_wgsl(new Uint8Array(spvBytes));
            const elapsed = (performance.now() - startTime).toFixed(2);

            // Basic WGSL validation checks
            const issues = validateWgsl(wgsl, name);

            if (issues.length === 0) {
                console.log(`PASS  (${elapsed}ms, ${wgsl.length} chars)`);
                passed++;
            } else {
                console.log(`WARN  (${elapsed}ms) - ${issues.join(', ')}`);
                passed++; // Warnings are not failures
            }

            // Save WGSL output for inspection
            writeFileSync(join(RESULTS_DIR, `${name}.wgsl`), wgsl);

            results.push({
                name,
                status: issues.length === 0 ? 'PASS' : 'WARN',
                time_ms: parseFloat(elapsed),
                wgsl_size: wgsl.length,
                spv_size: spvBytes.length,
                issues,
            });

        } catch (e) {
            const elapsed = (performance.now() - startTime).toFixed(2);
            console.log(`FAIL  (${elapsed}ms)`);
            console.log(`         Error: ${e.message || e}`);
            failed++;

            results.push({
                name,
                status: 'FAIL',
                time_ms: parseFloat(elapsed),
                spv_size: spvBytes.length,
                error: e.message || String(e),
            });
        }
    }

    // Summary
    console.log('\n' + '─'.repeat(60));
    console.log(`\nResults: ${passed} passed, ${failed} failed, ${spvFiles.length} total`);

    // Write results JSON
    const report = {
        timestamp: new Date().toISOString(),
        naga_wasm_size: wasmBytes.length,
        total_tests: spvFiles.length,
        passed,
        failed,
        results,
    };
    writeFileSync(join(RESULTS_DIR, 'report.json'), JSON.stringify(report, null, 2));
    console.log(`\nDetailed report: results/report.json`);
    console.log(`WGSL outputs:    results/*.wgsl\n`);

    if (failed > 0) {
        process.exit(1);
    }
}

/**
 * Basic validation of generated WGSL — checks for known issues.
 */
function validateWgsl(wgsl, name) {
    const issues = [];

    // Check for empty output
    if (!wgsl || wgsl.trim().length === 0) {
        issues.push('empty output');
        return issues;
    }

    // Check for remaining SPIR-V artifacts that shouldn't appear in WGSL
    if (wgsl.includes('OpVariable')) {
        issues.push('contains SPIR-V opcodes');
    }

    // Check that push constants were converted (should be storage buffer, not push_constant)
    if (wgsl.includes('var<push_constant>')) {
        issues.push('unconverted push_constant');
    }

    // Check for NaN/Inf literals (should be replaced)
    if (wgsl.match(/\binf\b/) && !wgsl.includes('0x1.fffffep')) {
        issues.push('possible raw inf literal');
    }

    // Check that entry points exist
    if (!wgsl.includes('@vertex') && !wgsl.includes('@fragment') && !wgsl.includes('@compute')) {
        issues.push('no entry point found');
    }

    // Check for diagnostic suppression (expected from naga-converter post-processing)
    if (wgsl.includes('diagnostic(off, derivative_uniformity)')) {
        // Good - this is expected
    }

    return issues;
}

// ─── Main ──────────────────────────────────────────────────────────────────

runTests();
