/**
 * SPIR-V Dump Validator
 *
 * Validates all .spv files in a given directory through naga-converter WASM.
 * Designed to run after `GODOT_DUMP_SPIRV=<dir>` produces SPIR-V files during
 * engine shader compilation.
 *
 * Known failures are tracked in expected_failures.json — these are Vulkan-only
 * shader variants that the WebGPU runtime never uses (different defines/code paths).
 * CI only fails on NEW failures beyond the baseline (regressions).
 *
 * Usage:
 *   node validate_spirv_dump.mjs <spirv-directory>
 *   node validate_spirv_dump.mjs ./spirv_dump/
 *   node validate_spirv_dump.mjs ./spirv_dump/ --update-baseline
 *
 * Exit codes:
 *   0 = no regressions (all failures are expected)
 *   1 = new failures detected beyond expected baseline
 */

import { readFileSync, readdirSync, writeFileSync, mkdirSync, existsSync } from 'fs';
import { join, basename } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const NAGA_WASM_PATH = join(__dirname, '../../drivers/webgpu/naga-converter/prebuilt/naga_wasm_bg.wasm');
const EXPECTED_FAILURES_PATH = join(__dirname, 'expected_failures.json');

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
    const updateBaseline = process.argv.includes('--update-baseline');

    if (!spvDir) {
        console.error('Usage: node validate_spirv_dump.mjs <spirv-directory> [--update-baseline]');
        process.exit(1);
    }

    console.log('╔══════════════════════════════════════════════════════════╗');
    console.log('║   SPIR-V Dump Validator (naga-converter)                 ║');
    console.log('╚══════════════════════════════════════════════════════════╝\n');

    // Load expected failures baseline
    let expectedFailures = new Set();
    if (existsSync(EXPECTED_FAILURES_PATH) && !updateBaseline) {
        const baseline = JSON.parse(readFileSync(EXPECTED_FAILURES_PATH, 'utf8'));
        expectedFailures = new Set(baseline.expected_failures || []);
        console.log(`Expected failures baseline: ${expectedFailures.size} shaders\n`);
    }

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
    let expectedFailed = 0;
    const failures = [];
    const regressions = [];

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
            const isExpected = expectedFailures.has(spvFile);

            if (isExpected) {
                console.log(`XFAIL (${elapsed}ms)  [expected]`);
                expectedFailed++;
            } else {
                console.log(`FAIL  (${elapsed}ms)`);
                console.log(`         ${e.message || e}`);
                regressions.push({ file: spvFile, error: e.message || String(e) });
            }
            failed++;
            failures.push({ file: spvFile, error: e.message || String(e) });
        }
    }

    // Summary
    console.log('\n' + '─'.repeat(60));
    console.log(`\nResults: ${passed} passed, ${failed} failed (${expectedFailed} expected), ${spvFiles.length} total`);
    console.log(`Pass rate: ${((passed / spvFiles.length) * 100).toFixed(1)}%`);

    if (regressions.length > 0) {
        console.log(`\n⚠️  REGRESSIONS (${regressions.length} new failures):`);
        for (const f of regressions) {
            console.log(`  - ${f.file}: ${f.error.substring(0, 100)}`);
        }
    }

    if (expectedFailed > 0 && !updateBaseline) {
        console.log(`\nExpected failures (${expectedFailed}):`);
        for (const f of failures.filter(f => expectedFailures.has(f.file))) {
            console.log(`  - ${f.file}`);
        }
    }

    // Write report
    const report = {
        timestamp: new Date().toISOString(),
        spirv_directory: spvDir,
        total: spvFiles.length,
        passed,
        failed,
        expected_failed: expectedFailed,
        regressions: regressions.length,
        failures,
    };

    const reportPath = join(spvDir, 'validation_report.json');
    writeFileSync(reportPath, JSON.stringify(report, null, 2));
    console.log(`\nReport: ${reportPath}`);

    // Update baseline if requested
    if (updateBaseline) {
        const baseline = {
            description: 'Expected SPIR-V validation failures — Vulkan-only shader variants not used by WebGPU runtime',
            updated: new Date().toISOString(),
            expected_failures: failures.map(f => f.file).sort(),
            failure_categories: categorizeFailures(failures),
        };
        writeFileSync(EXPECTED_FAILURES_PATH, JSON.stringify(baseline, null, 2) + '\n');
        console.log(`\nBaseline updated: ${EXPECTED_FAILURES_PATH} (${failures.length} expected failures)`);
        process.exit(0);
    }

    // Exit: fail only on regressions
    if (regressions.length > 0) {
        console.error(`\nFAILED: ${regressions.length} new shader(s) cannot be converted.`);
        console.error('If these are expected Vulkan-only variants, run with --update-baseline');
        process.exit(1);
    }

    console.log('\nPASSED: No regressions detected.');
    process.exit(0);
}

function categorizeFailures(failures) {
    const categories = {};
    for (const f of failures) {
        let category = 'unknown';
        if (f.error.includes('CopyLogical')) category = 'CopyLogical (SPIR-V 1.4+ struct copy)';
        else if (f.error.includes('TerminateInvocation')) category = 'TerminateInvocation (modern discard)';
        else if (f.error.includes('UnsupportedStorageClass')) category = 'UnsupportedStorageClass (Vulkan-only)';
        else if (f.error.includes('UnsupportedBuiltIn')) category = 'UnsupportedBuiltIn (e.g. PointSize)';
        else if (f.error.includes('ComparisonSamplingMismatch')) category = 'ComparisonSamplingMismatch (depth texture)';
        else if (f.error.includes('InvalidId')) category = 'InvalidId (spec constant cascade)';
        else if (f.error.includes('InvalidImage')) category = 'InvalidImage (image type mismatch)';
        else if (f.error.includes('InvalidBinaryOperandTypes')) category = 'InvalidBinaryOperandTypes (type width mismatch)';
        else if (f.error.includes('InvalidTypeWidth')) category = 'InvalidTypeWidth (16-bit types)';
        else if (f.error.includes('InvalidImageWriteType')) category = 'InvalidImageWriteType';
        else if (f.error.includes('IncompleteData')) category = 'IncompleteData (truncated SPIR-V)';
        else if (f.error.includes('BuiltinArgumentsInvalid')) category = 'BuiltinArgumentsInvalid';

        if (!categories[category]) categories[category] = [];
        categories[category].push(f.file);
    }
    return categories;
}

main();
