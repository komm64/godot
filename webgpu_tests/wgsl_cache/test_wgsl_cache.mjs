#!/usr/bin/env node
/**
 * test_wgsl_cache.mjs — Unit tests for the build-time WGSL precompilation system.
 *
 * Tests:
 * 1. naga_convert_cli.mjs single-file conversion
 * 2. naga_convert_cli.mjs batch conversion
 * 3. Generated header C++ syntax validation
 * 4. Precompiled table binary search contract
 *
 * These tests exercise the Node.js/WASM naga converter and validate
 * the generated header format without requiring a full engine build.
 *
 * Usage:
 *   node test_wgsl_cache.mjs
 */

import { readFileSync, writeFileSync, existsSync, unlinkSync, mkdtempSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { execFileSync, execSync } from 'child_process';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, '..', '..');
const NAGA_CLI = join(REPO_ROOT, 'drivers', 'webgpu', 'naga_convert_cli.mjs');
const GEN_HEADER = join(REPO_ROOT, 'drivers', 'webgpu', 'wgsl_precompiled.gen.h');

let passed = 0;
let failed = 0;

function assert(cond, msg) {
	if (!cond) {
		console.error(`  FAIL: ${msg}`);
		failed++;
	} else {
		console.log(`  PASS: ${msg}`);
		passed++;
	}
}

// =========================================================================
// Helper: create a minimal valid SPIR-V module
// =========================================================================

/**
 * Create a minimal valid SPIR-V compute shader that naga can convert.
 * This is a hand-crafted binary that represents:
 *   #version 450
 *   layout(local_size_x = 1) in;
 *   void main() { }
 */
function createMinimalSpirv() {
	// Minimal SPIR-V compute shader (OpCapability Shader, OpEntryPoint GLCompute)
	const words = [
		0x07230203, // Magic
		0x00010300, // Version 1.3
		0x00000000, // Generator
		0x00000006, // Bound = 6
		0x00000000, // Schema

		// OpCapability Shader
		0x00020011, 0x00000001,

		// OpMemoryModel Logical GLSL450
		0x0003000E, 0x00000000, 0x00000001,

		// OpEntryPoint GLCompute %main "main"
		// = 5 words: opcode(0x000F) | count(5) << 16, ExecutionModel(5=GLCompute), %main(1), "main\0"
		0x0005000F, 0x00000005, 0x00000001, 0x6E69616D, 0x00000000,

		// OpExecutionMode %main LocalSize 1 1 1
		0x00060010, 0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001,

		// OpTypeVoid %2
		0x00020013, 0x00000002,

		// OpTypeFunction %3 %2
		0x00030021, 0x00000003, 0x00000002,

		// OpFunction %2 None %3
		0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,

		// OpLabel %4
		0x000200F8, 0x00000004,

		// OpReturn
		0x000100FD,

		// OpFunctionEnd
		0x00010038,
	];

	const buffer = Buffer.alloc(words.length * 4);
	for (let i = 0; i < words.length; i++) {
		buffer.writeUInt32LE(words[i], i * 4);
	}
	return buffer;
}


// =========================================================================
// Test 1: naga CLI single-file conversion
// =========================================================================
console.log('\n=== Test 1: naga CLI single-file conversion ===');

if (existsSync(NAGA_CLI)) {
	const spv = createMinimalSpirv();
	const tmpSpv = join(__dirname, '_test_minimal.spv');
	writeFileSync(tmpSpv, spv);

	try {
		const wgsl = execFileSync('node', [NAGA_CLI, tmpSpv], {
			encoding: 'utf-8',
			timeout: 30000,
		});
		assert(wgsl.length > 0, 'naga produced non-empty WGSL output');
		assert(wgsl.includes('fn'), 'WGSL contains function declaration');
		assert(wgsl.includes('main'), 'WGSL contains main entry point');
	} catch (e) {
		assert(false, `naga CLI conversion failed: ${e.message}`);
	}

	try { unlinkSync(tmpSpv); } catch {}
} else {
	console.log('  SKIP: naga_convert_cli.mjs not found');
}


// =========================================================================
// Test 2: naga CLI batch conversion
// =========================================================================
console.log('\n=== Test 2: naga CLI batch conversion ===');

if (existsSync(NAGA_CLI)) {
	const spv = createMinimalSpirv();
	const tmpSpv1 = join(__dirname, '_test_batch1.spv');
	const tmpSpv2 = join(__dirname, '_test_batch2.spv');
	writeFileSync(tmpSpv1, spv);
	writeFileSync(tmpSpv2, spv);

	try {
		const jsonOut = execFileSync('node', [NAGA_CLI, '--batch', tmpSpv1, tmpSpv2], {
			encoding: 'utf-8',
			timeout: 30000,
		});
		const results = JSON.parse(jsonOut);

		assert(typeof results === 'object', 'Batch output is JSON object');
		assert(tmpSpv1 in results, 'Result contains first file key');
		assert(tmpSpv2 in results, 'Result contains second file key');
		assert(typeof results[tmpSpv1] === 'string', 'First result is WGSL string');
		assert(results[tmpSpv1].includes('fn'), 'First result contains function');
		assert(results[tmpSpv1] === results[tmpSpv2], 'Identical inputs produce identical WGSL');
	} catch (e) {
		assert(false, `naga CLI batch conversion failed: ${e.message}`);
	}

	try { unlinkSync(tmpSpv1); } catch {}
	try { unlinkSync(tmpSpv2); } catch {}
} else {
	console.log('  SKIP: naga_convert_cli.mjs not found');
}


// =========================================================================
// Test 3: naga CLI error handling
// =========================================================================
console.log('\n=== Test 3: naga CLI error handling ===');

if (existsSync(NAGA_CLI)) {
	// Test with invalid SPIR-V (random bytes).
	const tmpBad = join(__dirname, '_test_bad.spv');
	writeFileSync(tmpBad, Buffer.from([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07]));

	try {
		execFileSync('node', [NAGA_CLI, tmpBad], {
			encoding: 'utf-8',
			timeout: 30000,
		});
		assert(false, 'Expected naga to fail on invalid SPIR-V');
	} catch (e) {
		assert(e.status !== 0, 'naga exits with non-zero on invalid SPIR-V');
	}

	// Test batch mode with one good and one bad.
	const spv = createMinimalSpirv();
	const tmpGood = join(__dirname, '_test_good.spv');
	writeFileSync(tmpGood, spv);

	try {
		const jsonOut = execFileSync('node', [NAGA_CLI, '--batch', tmpGood, tmpBad], {
			encoding: 'utf-8',
			timeout: 30000,
		});
		const results = JSON.parse(jsonOut);
		assert(typeof results[tmpGood] === 'string', 'Batch: good file converted successfully');
		assert(typeof results[tmpBad] === 'object' && results[tmpBad].error,
			'Batch: bad file has error object');
	} catch (e) {
		assert(false, `naga CLI batch error handling failed: ${e.message}`);
	}

	try { unlinkSync(tmpBad); } catch {}
	try { unlinkSync(tmpGood); } catch {}
} else {
	console.log('  SKIP: naga_convert_cli.mjs not found');
}


// =========================================================================
// Test 4: Generated header format validation
// =========================================================================
console.log('\n=== Test 4: Generated header validation ===');

if (existsSync(GEN_HEADER)) {
	const content = readFileSync(GEN_HEADER, 'utf-8');

	assert(content.includes('// Auto-generated by'),
		'Header has auto-generated comment');
	assert(content.includes('#pragma once'), 'Header has include guard');
	assert(content.includes('#include <cstdint>'), 'Header includes cstdint');
	assert(content.includes('struct WgslPrecompiledEntry'), 'Header has struct definition');
	assert(content.includes('_wgsl_precompiled_count'), 'Header has count variable');

	// Extract count.
	const countMatch = content.match(/_wgsl_precompiled_count\s*=\s*(\d+)/);
	assert(countMatch !== null, 'Count variable has numeric value');
	const count = parseInt(countMatch[1]);
	assert(count > 0, `Header has ${count} entries (> 0)`);
	assert(count >= 100, `Header has ${count} entries (>= 100 expected for full shader set)`);

	// Verify hashes are sorted (critical for binary search).
	const hashRegex = /0x([0-9A-Fa-f]+)ULL/g;
	const hashes = [];
	let match;
	while ((match = hashRegex.exec(content)) !== null) {
		hashes.push(BigInt('0x' + match[1]));
	}
	assert(hashes.length === count, `Found ${hashes.length} hashes matching count ${count}`);

	let sorted = true;
	for (let i = 1; i < hashes.length; i++) {
		if (hashes[i] <= hashes[i - 1]) {
			sorted = false;
			break;
		}
	}
	assert(sorted, 'All hashes are strictly sorted (ascending)');

	// Check for valid WGSL content (should contain common WGSL keywords).
	assert(content.includes('fn '), 'Header contains WGSL function declarations');
	assert(content.includes('var<'), 'Header contains WGSL variable declarations');

	// Verify raw string literal syntax.
	const rawStringCount = (content.match(/R"wgsl\(/g) || []).length;
	assert(rawStringCount === count, `Found ${rawStringCount} raw string literals matching count ${count}`);
} else {
	console.log('  SKIP: wgsl_precompiled.gen.h not found (run scons build first)');
}


// =========================================================================
// Test 5: Binary search contract simulation
// =========================================================================
console.log('\n=== Test 5: Binary search contract ===');

{
	// Simulate the C++ binary search on the sorted hash array.
	function binarySearch(entries, target) {
		let lo = 0, hi = entries.length;
		while (lo < hi) {
			const mid = lo + ((hi - lo) >> 1);
			if (entries[mid].hash < target) lo = mid + 1;
			else if (entries[mid].hash > target) hi = mid;
			else return entries[mid].wgsl;
		}
		return null;
	}

	// Test with known entries.
	const testEntries = [
		{ hash: 1n, wgsl: 'fn a() {}' },
		{ hash: 100n, wgsl: 'fn b() {}' },
		{ hash: 1000n, wgsl: 'fn c() {}' },
		{ hash: 10000n, wgsl: 'fn d() {}' },
		{ hash: 100000n, wgsl: 'fn e() {}' },
	];

	assert(binarySearch(testEntries, 1n) === 'fn a() {}', 'Binary search: find first entry');
	assert(binarySearch(testEntries, 100000n) === 'fn e() {}', 'Binary search: find last entry');
	assert(binarySearch(testEntries, 1000n) === 'fn c() {}', 'Binary search: find middle entry');
	assert(binarySearch(testEntries, 42n) === null, 'Binary search: miss returns null');
	assert(binarySearch(testEntries, 0n) === null, 'Binary search: below range returns null');
	assert(binarySearch(testEntries, 999999n) === null, 'Binary search: above range returns null');
	assert(binarySearch([], 42n) === null, 'Binary search: empty table returns null');

	// Stress test: random lookups on large sorted array.
	const large = [];
	for (let i = 0; i < 500; i++) {
		large.push({ hash: BigInt(i * 7 + 3), wgsl: `fn f${i}() {}` });
	}
	assert(binarySearch(large, 3n) === 'fn f0() {}', 'Binary search stress: first element');
	assert(binarySearch(large, BigInt(499 * 7 + 3)) === 'fn f499() {}', 'Binary search stress: last element');
	assert(binarySearch(large, BigInt(250 * 7 + 3)) === 'fn f250() {}', 'Binary search stress: middle element');
	assert(binarySearch(large, 1n) === null, 'Binary search stress: gap miss');
}


// =========================================================================
// Summary
// =========================================================================
console.log(`\n${'='.repeat(50)}`);
console.log(`Results: ${passed} passed, ${failed} failed`);
if (failed > 0) {
	process.exit(1);
}
