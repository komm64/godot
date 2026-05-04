# WebGPU Test Suite

Automated tests for the Godot WebGPU rendering backend. These tests validate the SPIR-V→WGSL shader pipeline, resource lifecycle management, and cross-browser rendering correctness.

## Test Categories

| Test | What it validates | Runtime |
|------|------------------|---------|
| [Shader Corpus](shader_corpus/) | SPIR-V → WGSL conversion via naga-converter WASM | ~1s (Node.js) |
| [Resource Lifecycle](resource_lifecycle/) | Rapid create/destroy of buffers, textures, pipelines | ~30s (browser) |
| [Screenshot Comparison](screenshot_comparison/) | Visual regression across Chrome and Firefox | ~60s (browser) |

## Quick Start

```bash
# Shader corpus (no browser needed)
cd shader_corpus && node run_tests.mjs

# Resource lifecycle (needs Playwright)
cd resource_lifecycle && node run_tests.mjs

# Screenshot comparison (needs Playwright + browsers)
cd screenshot_comparison && node screenshot_tests.mjs --update-baselines
```

## CI

The `.github/workflows/webgpu_tests.yml` workflow runs all tests on push to the `webgpu-4.6.2` branch or PRs touching `drivers/webgpu/` or `webgpu_tests/`.

## Dependencies

- **Node.js 20+** — All test runners
- **glslangValidator** — Shader corpus fixture compilation
- **Playwright** — Browser-based tests (resource lifecycle, screenshots)
