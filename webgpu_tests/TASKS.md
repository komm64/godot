# WebGPU Test Suite: Status & Tasks

## Testing Coverage: What Exists vs What's Lacking

| Category | Status | What Exists | What's Missing |
|----------|--------|-------------|----------------|
| **Shader pipeline (SPIR-V -> WGSL)** | Done | 9 hand-crafted GLSL fixtures compiled & validated through naga-converter WASM | Fuzz testing of the shader pipeline (random/malformed SPIR-V inputs) |
| **Full engine SPIR-V validation** | Done | All 309 engine-compiled shaders validated offline via `validate_spirv_dump.mjs` with expected-failures baseline | — |
| **End-to-end smoke test** | Done | Headless Chrome runs exported Godot project, checks no shader errors / device-lost | Multi-browser smoke test (Firefox/Safari headless) |
| **Resource lifecycle stress** | Done | Rapid create/destroy of buffers, textures, pipelines in standalone browser test | — |
| **Screenshot comparison** | Done | Visual regression across Chrome + Firefox (warning-only in CI) | Merge-blocking visual regression; Safari baselines |
| **Scene smoketest** | Done | Multiple scene exports validated in headless browser (`scene_smoketest/`) | — |
| **CI pipeline** | Done | GitHub Actions workflow with parallel jobs, path-triggered, merge-gating | — |
| **Shader coverage scene** | Done | GDScript scene exercising 100% of RenderingDevice shader paths (all material/lighting/post-process combos) | — |
| **Unit tests (driver)** | Done | 305 JS tests covering ring buffer, shadow buffer, bind group layout, pipeline hashing, command buffer, format mapping, buffer alignment, std140 packing, texture layout, texture conversion, bind group compatibility | — |
| **Unit tests (naga-converter)** | Done | 71 Rust `#[test]` covering all rewriting passes, post-parse fixes, end-to-end SPIR-V → WGSL, and error cases | — |
| **Regression test suite** | Missing | No named/tracked regression cases | A suite of known-bug repros that run in CI to prevent recurrence |
| **Fuzz testing** | Done | 3 cargo-fuzz targets (spirv_to_wgsl, preprocess_passes, split_samplers) with seeded corpus; found & fixed 4 bugs | — |
| **Multi-browser CI** | Done | Scene smoketest runs 18 scenes on Chrome + Firefox + Safari; CI runs Chrome + Firefox | Safari CI automation (requires macOS runner) |
| **Performance benchmarks** | Missing | *Nothing* | Frame-time / draw-call benchmarks to catch performance regressions (shader compile time, IPC overhead, buffer upload) |
| **Async readback tests** | Missing | *Nothing dedicated* | Test viewport capture, GPU->CPU buffer readback timing, fence/callback correctness |
| **Error path testing** | Missing | *Nothing* | Verify graceful handling of device-lost, OOM, invalid API usage, context loss recovery |

## Task Breakdown

### 1. Unit tests: naga-converter (Rust `#[test]`) — DONE

50 Rust unit tests in `drivers/webgpu/naga-converter/src/lib.rs`:

- [x] `freeze_spec_constant_ops` — 6 tests (no-op, rewrite, evaluate IAdd, bool, SpecId stripping, composites)
- [x] `rewrite_copy_logical` — 5 tests (no-op, single/multiple replace, preserves others, too-small input)
- [x] `rewrite_terminate_invocation` — 4 tests (no-op, single/multiple replace, too-small input)
- [x] `infer_readonly_storage` — 6 tests (no storage, adds NonWritable, written var, access-chain write, no duplicate, mixed)
- [x] `convert_push_constants_to_uniforms` — 3 tests (no push constants, rewrites storage class, injects decorations)
- [x] `split_combined_samplers` — 2 tests (no combined, basic split)
- [x] `fix_depth2_images` — 3 tests (depth=0 unchanged, depth=2→1, depth=1 preserved)
- [x] `eval_spec_op` — 10 tests (arithmetic, logical, select, comparisons, bitwise, unknown)
- [x] `fix_fmax_literals` — 3 tests (positive, negative, no-match)
- [x] End-to-end: 2 tests (minimal vertex shader, fragment with OpTerminateInvocation)
- [x] Edge cases: 3 tests (empty input, header-only, idempotency)

### 2. Unit tests: WebGPU driver (JS isolation) — DONE

191 JavaScript unit tests in `webgpu_tests/driver_unit_tests/` (algorithms extracted from C++ driver):

- [x] Ring buffer allocation and wrap-around (17 tests)
- [x] Shadow buffer copy correctness (16 tests)
- [x] Descriptor set / bind group layout generation (25 tests)
- [x] Pipeline state hashing and caching (12 tests)
- [x] Command buffer encoding sequences (28 tests)
- [x] Texture format mapping (Godot format → WebGPU format) (23 tests)
- [x] Buffer alignment and offset calculations (33 tests)
- [x] Uniform buffer packing (std140 layout) (23 tests)

### 3. Fuzz testing (naga-converter) — DONE

3 cargo-fuzz targets in `drivers/webgpu/naga-converter/fuzz/`:

- [x] `spirv_to_wgsl` — full pipeline (all passes + naga parse + WGSL gen), catch_unwind for naga panics
- [x] `preprocess_passes` — all 7 SPIR-V rewriting passes chained, no naga parsing
- [x] `split_samplers` — most complex pass (~500 lines) in isolation
- [x] Seeded corpus: 9 .spv fixtures + 2 regression files + 6 synthetic edge cases
- [x] CI job runs all 3 targets for 60s each on every push/PR
- [x] Local CI (`local_ci.sh`) Stage 3 runs fuzz tests when Rust nightly available

Bugs found and fixed by fuzzing:
- Integer overflows in `split_combined_samplers` (3 multiplication sites → `wrapping_mul`/`wrapping_add`)
- Out-of-bounds read in `read_word` (added bounds check)
- `alloc_id` overflow when SPIR-V bound = u32::MAX (`next_id += 1` → `wrapping_add`)
- Conditional compilation fix for `log()` and `spirv_to_wgsl` outside WASM runtime

### 4. Regression test suite

- [ ] Collect known-bug SPIR-V samples that previously caused failures
- [ ] Add a `regressions/` directory with named test cases
- [ ] Each test case: input `.spv` + expected outcome (pass/fail/specific WGSL output)
- [ ] CI validates no regressions reappear

### 5. Multi-browser smoke tests — DONE

- [x] Add Firefox to the scene smoketest (Playwright with `dom.webgpu.enabled`)
- [x] Add Safari to the scene smoketest (real Safari via AppleScript, macOS only)
- [x] Update CI workflow to run scene smoketest on Chrome + Firefox
- [ ] Safari CI automation (requires macOS runner — currently local-only)

### 6. Performance benchmarks

- [ ] Shader compile time benchmark (measure naga-converter throughput)
- [ ] Frame-time benchmark scene (stable scene, measure ms/frame variance)
- [ ] Buffer upload throughput benchmark
- [ ] CI job that runs benchmarks and reports regressions (not merge-blocking, warning-only)

### 7. Error path testing

- [ ] Device-lost simulation and recovery
- [ ] OOM handling (allocate until failure, verify graceful degradation)
- [ ] Invalid API usage (e.g., submit destroyed buffer) — verify no crash
- [ ] Context loss and re-creation

### 8. Async readback tests

- [ ] Viewport capture correctness (render known scene, read back pixels, verify)
- [ ] GPU->CPU buffer readback timing (verify callbacks fire)
- [ ] Fence/sync correctness under rapid submit cycles
