# WebGPU Test Suite

Automated tests for the Godot WebGPU rendering backend. Validates the full shader pipeline from GLSL compilation through SPIR-V → WGSL conversion to in-browser execution.

## Test Categories

| Test | What it validates | Runtime |
|------|------------------|---------|
| [Shader Corpus](shader_corpus/) | SPIR-V → WGSL conversion via naga-converter WASM | ~1s (Node.js) |
| [Test Project](test_project/) | 100% shader path coverage via comprehensive Godot scene | ~90s (build + run) |
| [Resource Lifecycle](resource_lifecycle/) | Rapid create/destroy of buffers, textures, pipelines | ~30s (browser) |
| [Screenshot Comparison](screenshot_comparison/) | Visual regression across Chrome and Firefox | ~60s (browser) |

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  CI Pipeline (.github/workflows/webgpu_tests.yml)               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐                                            │
│  │ shader-corpus   │  GLSL→SPIR-V→WGSL (standalone, fast)      │
│  └─────────────────┘                                            │
│                                                                 │
│  ┌─────────────────┐     ┌──────────────────┐                  │
│  │ build-webgpu    │────▶│ validate-spirv   │                  │
│  │ (engine build + │     │ (ALL engine .spv  │                  │
│  │  SPIR-V dump)   │     │  through naga)   │                  │
│  │                 │     └──────────────────┘                  │
│  │                 │     ┌──────────────────┐                  │
│  │                 │────▶│ smoke-test       │                  │
│  └─────────────────┘     │ (headless Chrome │                  │
│                          │  full scene run) │                  │
│                          └──────────────────┘                  │
│                                                                 │
│  ┌────────────────────────┐  ┌─────────────────────────────┐   │
│  │ resource-lifecycle     │  │ screenshot-comparison        │   │
│  │ (stress test in Chrome)│  │ (Chrome + Firefox visual)    │   │
│  └────────────────────────┘  └─────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## Quick Start

```bash
# Shader corpus (no browser needed)
cd shader_corpus && node run_tests.mjs

# Validate dumped SPIR-V (after running engine with GODOT_DUMP_SPIRV)
node shader_corpus/validate_spirv_dump.mjs /path/to/spirv_dump/

# Resource lifecycle (needs Playwright)
cd resource_lifecycle && node run_tests.mjs

# Screenshot comparison (needs Playwright + browsers)
cd screenshot_comparison && node screenshot_tests.mjs --update-baselines
```

## SPIR-V Dump (Engine Integration)

The engine includes a `GODOT_DUMP_SPIRV` environment variable that causes all compiled SPIR-V to be written to disk during shader compilation:

```bash
GODOT_DUMP_SPIRV=/tmp/spirv_dump godot --headless --path test_project/ --quit
node shader_corpus/validate_spirv_dump.mjs /tmp/spirv_dump/
```

This validates that **every shader in the engine** converts successfully through naga-converter.

## CI

The `.github/workflows/webgpu_tests.yml` workflow runs on push/PR to `webgpu-4.6.2` when `drivers/webgpu/`, `servers/rendering/`, or `webgpu_tests/` are modified.

**Jobs:**
1. `shader-corpus` — Fast standalone SPIR-V→WGSL test (no build)
2. `build-webgpu` — Full engine build + SPIR-V dump + export
3. `validate-spirv` — All dumped engine SPIR-V through naga
4. `smoke-test` — Exported project in headless Chrome
5. `resource-lifecycle` — GPU resource stress test
6. `screenshot-comparison` — Visual regression (Chrome + Firefox)

## Dependencies

- **Node.js 20+** — All test runners
- **glslangValidator** — Shader corpus fixture compilation
- **Playwright** — Browser-based tests
- **Emscripten 4.0.11** — WebGPU template build
- **SCons** — Godot build system
