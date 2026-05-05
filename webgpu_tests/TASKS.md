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
| **Unit tests (driver)** | Missing | *Nothing* | Isolated tests for `RenderingDeviceDriverWebGPU` methods — pipeline creation, buffer management, command encoding, descriptor set logic, ring buffer, shadow buffer, etc. |
| **Unit tests (naga-converter)** | Missing | *Nothing* (only integration through WASM) | Rust `#[test]` for each rewriting pass (freeze spec constants, split samplers, push->uniform, readonly inference, etc.) |
| **Regression test suite** | Missing | No named/tracked regression cases | A suite of known-bug repros that run in CI to prevent recurrence |
| **Fuzz testing** | Missing | *Nothing* | Fuzz the naga-converter with random/corrupted SPIR-V to find panics or incorrect transforms |
| **Multi-browser CI** | Done | Scene smoketest runs 18 scenes on Chrome + Firefox + Safari; CI runs Chrome + Firefox | Safari CI automation (requires macOS runner) |
| **Performance benchmarks** | Missing | *Nothing* | Frame-time / draw-call benchmarks to catch performance regressions (shader compile time, IPC overhead, buffer upload) |
| **Async readback tests** | Missing | *Nothing dedicated* | Test viewport capture, GPU->CPU buffer readback timing, fence/callback correctness |
| **Error path testing** | Missing | *Nothing* | Verify graceful handling of device-lost, OOM, invalid API usage, context loss recovery |

## Task Breakdown

### 1. Unit tests: naga-converter (Rust `#[test]`)

Add Rust unit tests for each SPIR-V rewriting pass in `drivers/webgpu/naga-converter/src/`:

- [ ] `freeze_spec_constant_ops` — verify OpSpecConstantOp is evaluated to plain constants
- [ ] `rewrite_copy_logical` — verify OpCopyLogical becomes OpCopyObject
- [ ] `rewrite_terminate_invocation` — verify OpTerminateInvocation becomes OpKill
- [ ] `infer_readonly_storage` — verify NonWritable is added to read-only SSBOs
- [ ] `convert_push_constants_to_uniforms` — verify PushConstant becomes StorageBuffer at group(3)/binding(120)
- [ ] `split_combined_samplers` — verify combined image samplers become separate texture + sampler
- [ ] `fix_depth2_images` — verify depth=2 (unknown) becomes depth=1 for comparison sampling
- [ ] End-to-end: minimal SPIR-V in, valid WGSL out

### 2. Unit tests: WebGPU driver (C++ or JS isolation)

Test individual driver subsystems without running the full engine:

- [ ] Ring buffer allocation and wrap-around
- [ ] Shadow buffer copy correctness
- [ ] Descriptor set / bind group layout generation
- [ ] Pipeline state hashing and caching
- [ ] Command buffer encoding sequences
- [ ] Texture format mapping (Godot format -> WebGPU format)
- [ ] Buffer alignment and offset calculations
- [ ] Uniform buffer packing (std140 layout)

### 3. Fuzz testing (naga-converter)

- [ ] Set up `cargo-fuzz` target for the SPIR-V -> WGSL pipeline
- [ ] Seed corpus from engine-compiled `.spv` files
- [ ] CI job to run fuzzer for N minutes on each PR (or nightly)

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
