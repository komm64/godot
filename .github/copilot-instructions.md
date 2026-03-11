# Copilot Instructions — Godot WebGPU Backend

## Project Goal

Add a WebGPU rendering driver to Godot 4.6, targeting browser export via Emscripten. The driver must implement the `RenderingDeviceDriver` / `RenderingContextDriver` / `RenderingShaderContainerFormat` interfaces (same as the Vulkan, D3D12, and Metal backends). Deadline: March 24, 2026.

## Read These First

All design decisions, API mappings, and tasks are documented in `webgpu_notes/`:

| File | Read When |
|------|-----------|
| `webgpu_notes/TASKS.md` | Starting any session — gives current phase and next task |
| `webgpu_notes/BUILD_BLOCKERS.md` | Phase 0: wiring the build system (Day 1) |
| `webgpu_notes/DESIGN.md` | Implementing any RDD method — has exact WebGPU API calls |
| `webgpu_notes/RESEARCH.md` | Background on architecture, prior art, WebGPU constraints |
| `webgpu_notes/stubs/` | Starting point files to copy into `drivers/webgpu/` |

**Do not re-research what is already in these files.** Read them first.

## Repository Layout (relevant paths)

```
drivers/webgpu/           ← target location for the new driver (copy from stubs/)
servers/rendering/        ← RenderingDeviceDriver, RenderingContextDriver interfaces
drivers/metal/            ← best reference driver (closest to WebGPU)
platform/web/             ← Emscripten platform, DisplayServerWeb
modules/glslang/          ← GLSL→SPIR-V (needs webgpu added to can_build())
```

## Godot Coding Conventions

- **C++ style:** snake_case everywhere. Classes: `PascalCase`. Member variables: `_prefixed` or plain snake_case.
- **Memory:** use `memnew(T)` / `memdelete(ptr)` instead of `new`/`delete`. Vectors use `Vector<T>`, hashmaps use `HashMap<K,V>`.
- **Error handling:** return `ERR_*` constants; use `ERR_FAIL_COND_V(cond, val)` / `ERR_FAIL_NULL_V(ptr, val)` macros.
- **Printing:** `print_verbose(...)`, `WARN_PRINT(...)`, `ERR_PRINT(...)`. Never `printf` or `std::cout`.
- **IDs:** RDD uses opaque integer IDs cast to/from pointers via the `ID` helper. See `webgpu_notes/DESIGN.md` Section 1 for the exact pattern.
- **Includes:** use `"..."` for project headers, `<...>` for system/third-party. No `.h` extension for standard library headers.

## Key Architectural Decisions (already made)

1. **Push constants:** Emulated via a 256KB ring buffer (group 3, binding 0) with 256-byte aligned slots. See `DESIGN.md` Section 5.
2. **Subpasses:** Flattened — each Godot subpass becomes a separate `WGPURenderPassEncoder`. See `DESIGN.md` Section 6.
3. **Barriers:** All no-ops — WebGPU tracks hazards automatically.
4. **Buffer mapping:** Shadow CPU buffer pattern — maintain a CPU copy, flush via `wgpuQueueWriteBuffer` on unmap.
5. **Device init:** JS shell pre-initializes WebGPU; C++ retrieves it via `emscripten_webgpu_get_device()`.
6. **Shader translation:** GLSL → SPIR-V (glslang) → WGSL (Tint from Dawn). See `DESIGN.md` Section 7.

## WebGPU Constraints to Keep in Mind

- Max 4 bind groups (Godot uses sets 0–3 → fits exactly, but set 3 is used for push constant emulation)
- No push constants → ring buffer emulation
- No subpasses → flatten each subpass into a separate render pass
- No synchronous buffer map → shadow buffer approach
- 256-byte row alignment for buffer↔texture copies
- No 3-component texture formats (RGB8, RGB16, RGB32 unsupported as textures)
- WGSL has no `gl_FragDepth` → use `@builtin(frag_depth)`; no `discard` keyword → use `discard` statement in WGSL (same word, different syntax)
- No multi-draw-indirect → loop over individual draws

## Phase Status

Check `webgpu_notes/TASKS.md` for current phase. As of project start:
- **Phase 0 (Day 1):** Build system + driver registration → see `BUILD_BLOCKERS.md`
- **Phase 1 (Days 2–4):** Core skeleton — copy stubs, boot Emscripten, clear screen
- **Phase 2 (Days 5–7):** Resources (shaders, buffers, textures, pipelines)
- **Phase 3 (Days 8–10):** 3D, compute, timestamps
- **Phase 4–5 (Days 11–14):** Export, HTML shell, testing

## Build Commands

```bash
# Build web template with WebGPU (after Phase 0 changes):
scons platform=web target=template_debug webgpu=yes opengl3=no -j$(nproc)

# Build macOS editor (for testing non-web code paths):
scons platform=macos target=editor -j$(nproc)

# Run static checks before committing:
python misc/scripts/check_style.py
```

## Emscripten Notes

- Requires Emscripten 4.0.0+: `emcc --version`
- WebGPU flag: `-sUSE_WEBGPU=1`  
- Device retrieval: `#include <emscripten/html5_webgpu.h>` → `emscripten_webgpu_get_device()`
- Surface from canvas: `WGPUSurfaceDescriptorFromCanvasHTMLSelector` with `selector = "#canvas"`
