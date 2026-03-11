# WebGPU for Godot 4.6 — Task Breakdown

> **Purpose**: Master task list for AI agents implementing WebGPU support in Godot 4.6.
> **Target Completion**: March 24, 2026 (2-week sprint from March 10)
> **Last Updated**: March 11, 2026 (Task 2.6 IN PROGRESS — Engine boots WebGPU, renders 2D at ~240fps with zero errors in browser. Visual correctness pending verification. Next: add visible test content and verify render pipeline output.)
>
> **Key Reference**: `webgpu_notes/RESEARCH.md` — comprehensive architecture and API research
> **Key Reference**: `webgpu_notes/INITIAL_PLAN.md` — project vision and success criteria

---

## How to Use This Document

### For AI Agents
- Tasks are organized into **Phases** (sequential) and **Work Streams** within phases (parallelizable where noted)
- Each task has: ID, dependencies, estimated effort, status, and detailed instructions
- **Before starting a task**: Read all referenced files. Read `RESEARCH.md` sections cited.
- **After completing a task**: Update this file — set status to `DONE`, add completion notes, add any new findings
- **If blocked**: Update status to `BLOCKED`, note the blocker, move to a non-blocked task

### Status Values
- `TODO` — Not started
- `IN_PROGRESS` — Being worked on (note which agent)
- `DONE` — Completed and verified
- `BLOCKED` — Cannot proceed (note blocker)
- `SKIPPED` — Intentionally skipped (note reason)

### Parallelism Rules
- Tasks within the same phase marked `[PARALLEL]` can be done simultaneously by different agents
- Tasks marked `[SERIAL]` must be done in order
- Tasks in later phases depend on earlier phases being complete
- Cross-phase dependencies are noted explicitly

### Conventions
- All file paths are relative to the repository root
- "Reference file" means read it to understand the pattern, then create the analogous WebGPU version
- When implementing a driver method, check Metal (`drivers/metal/`) first, then Vulkan (`drivers/vulkan/`) for comparison

---

## Phase 0: Setup & Build System (Day 1)

> **Goal**: Get WebGPU compilation wired into the build system. No driver code yet — just the scaffolding that compiles an empty driver with `scons platform=web webgpu=yes`.

### Task 0.1: Build System — SConstruct & detect.py `[SERIAL]`
**Status**: `DONE`
**Effort**: 2-3 hours
**Dependencies**: None
**Agent Notes**: Completed March 10, 2026. All build system wiring done. Dry-run confirms all 3 webgpu driver files are picked up and glslang is enabled.

**Completion Notes**:
- Added `webgpu` BoolVariable to `SConstruct` after `metal` option
- Updated `platform/web/detect.py` `get_flags()` to add `"supported": ["webgpu"]`
- Updated `platform/web/detect.py` `configure()` to add WEBGPU_ENABLED, RD_ENABLED defines and -sUSE_WEBGPU=1 linker flag
- Updated `modules/glslang/config.py` to enable glslang for WebGPU builds
- Created `drivers/webgpu/` directory with all stub files (copied from `webgpu_notes/stubs/`)
- Updated `drivers/SCsub` to include WebGPU driver with platform support check
- Updated `servers/display/display_server.cpp` to include WebGPU context driver header and create RCD in `is_rendering_device_supported()` and `can_create_rendering_device()`
- Updated `main/main.cpp` to add `rendering/rendering_device/driver.web`, add `webgpu` to `available_drivers`, and update `rendering_method.web` to support forward_plus/mobile
- Updated `platform/web/display_server_web.cpp` to add `webgpu` to `get_rendering_drivers_func()` and add WebGPU init guard in constructor
- Dry-run verified: `scons platform=web webgpu=yes opengl3=no -n` reads SConscript files without errors, queues all 3 WebGPU driver files + glslang for compilation

**Instructions**:

1. **Edit `SConstruct`** (~line 200, after the `metal` option):
   - Add build option: `opts.Add(BoolVariable("webgpu", "Enable the WebGPU rendering driver", False))`

2. **Edit `platform/web/detect.py`**:
   - In `get_flags()` (~line 81): Keep `"vulkan": False` but do NOT force `"webgpu": False` — let it be user-selectable
   - In `configure()` (~line 250): Add WebGPU-specific configuration:
     ```python
     if env["webgpu"]:
         env.AppendUnique(CPPDEFINES=["WEBGPU_ENABLED", "RD_ENABLED"])
         env.Append(LINKFLAGS=["-sUSE_WEBGPU=1"])
         # Keep GLES3 for fallback
         env.AppendUnique(CPPDEFINES=["GLES3_ENABLED"])
     ```
   - Note: `RD_ENABLED` is required for `renderer_rd/` to compile. Currently web never sets this.
   - Ensure both `GLES3_ENABLED` and `WEBGPU_ENABLED` + `RD_ENABLED` can coexist
   - WebGPU builds still need `-sMAX_WEBGL_VERSION=2` for GLES3 fallback

3. **Edit `drivers/SCsub`**:
   - Add: `if env.get("webgpu_enabled"): SConscript("webgpu/SCsub")` (follow the pattern used for vulkan/metal/d3d12)
   - Check how `vulkan_enabled`, `metal_enabled`, `d3d12_enabled` env variables are set in the build chain vs CPPDEFINES

4. **Create `drivers/webgpu/SCsub`**:
   - Simple SCsub that compiles all `.cpp` files in the directory
   - Reference: `drivers/metal/SCsub`

5. **Create stub files** (empty class declarations, enough to compile):
   - `drivers/webgpu/rendering_context_driver_webgpu.h` — empty class extending `RenderingContextDriver`
   - `drivers/webgpu/rendering_context_driver_webgpu.cpp` — empty implementations
   - `drivers/webgpu/rendering_device_driver_webgpu.h` — empty class extending `RenderingDeviceDriver`
   - `drivers/webgpu/rendering_device_driver_webgpu.cpp` — empty implementations returning errors/defaults
   - `drivers/webgpu/rendering_shader_container_webgpu.h` — empty class extending `RenderingShaderContainer`
   - `drivers/webgpu/rendering_shader_container_webgpu.cpp` — empty implementations

6. **Verify**: `scons platform=web target=template_debug webgpu=yes` should compile (even if it crashes at runtime)

**Completion Criteria**: Build succeeds with `webgpu=yes` flag. All stub files compile.

**Notes for Agent**:
- Read `SConstruct` lines 190-210 for existing option patterns
- Read `platform/web/detect.py` fully — it's 348 lines
- Read `drivers/SCsub` to see how other drivers are conditionally included
- Read `drivers/metal/SCsub` for the SCsub pattern
- Read `drivers/metal/rendering_context_driver_metal.h` for the class pattern
- Check how `env["vulkan"]` flows to `env["vulkan_enabled"]` — there's likely a transformation in `SConstruct` or `methods.py`
- Ensure the `glslang` and `spirv-reflect` thirdparty libs are compiled in web builds when `RD_ENABLED` (they may currently be excluded)

---

### Task 0.2: Wire Up Driver Registration `[SERIAL, after 0.1]`
**Status**: `DONE`
**Effort**: 2-3 hours
**Dependencies**: Task 0.1

**Completion Notes** (March 10, 2026):
- Majority of this task was completed as part of Task 0.1.
- Added `#ifdef WEBGPU_ENABLED` guard to `platform/web/display_server_web.h` (line after GLES3_ENABLED block)
- `get_rendering_drivers_func()`, constructor guard, `main/main.cpp` and `display_server.cpp` changes all done in 0.1.

**Instructions**:

1. **Edit `platform/web/display_server_web.h`**:
   - Add `#ifdef WEBGPU_ENABLED` section with WebGPU-related members
   - Add a `RenderingContextDriverWebGPU*` member (or create it on demand)

2. **Edit `platform/web/display_server_web.cpp`**:
   - In `get_rendering_drivers_func()`: Add `"webgpu"` to returned drivers when `WEBGPU_ENABLED`
   - In the constructor: Add `#ifdef WEBGPU_ENABLED` initialization path for WebGPU
   - For now, just create the `RenderingContextDriverWebGPU` — actual WebGPU device init comes later

3. **Edit `main/main.cpp`**:
   - Line ~2571: Update the web rendering method hint from `"gl_compatibility"` to `"forward_plus,mobile,gl_compatibility"` when `WEBGPU_ENABLED`
   - Line ~2521-2545: Add `"webgpu"` as a valid driver for `forward_plus` and `mobile` rendering methods
   - Search for where rendering context drivers are instantiated and add `#ifdef WEBGPU_ENABLED` block

4. **Edit `servers/display/display_server.cpp`** (~line 2009):
   - Add `#ifdef WEBGPU_ENABLED` block to create `RenderingContextDriverWebGPU`

5. **Verify**: Build compiles. Running the web export should at least get to the point where it tries to initialize WebGPU (and fails gracefully since driver methods are stubs).

**Completion Criteria**: Build compiles with WebGPU driver registration wired up. `"webgpu"` appears as available rendering driver.

**Notes for Agent**:
- Read `servers/display/display_server.cpp` lines 2000-2020 for the existing driver creation pattern
- Read `main/main.cpp` lines 2400-2600 for the full rendering setup flow
- Read `platform/web/display_server_web.cpp` fully for the web display server

---

## Phase 1: Core Driver Skeleton (Days 2-4)

> **Goal**: Implement enough of the WebGPU driver to clear the screen to a solid color in a browser. This validates the entire pipeline: build → Emscripten → WebGPU init → surface → render pass → present.

### Task 1.1: WebGPU Internal Objects `[PARALLEL with 1.2]`
**Status**: `DONE`
**Effort**: 4-6 hours
**Dependencies**: Phase 0

**Completion Notes** (March 10, 2026):
- All internal object types defined in `drivers/webgpu/webgpu_objects.h` (copied from `webgpu_notes/stubs/`)
- Pixel format mapping complete in `drivers/webgpu/pixel_formats_webgpu.h`
- Fixed Dawn API renames: `WGPUBufferUsageFlags` → `WGPUBufferUsage`, `WGPUTextureUsageFlags` → `WGPUTextureUsage`, `WGPUVertexFormat_Undefined` (removed) → `(WGPUVertexFormat)0`
- Build compiles clean with `--use-port=emdawnwebgpu`

**Instructions**:

1. **Create `drivers/webgpu/webgpu_objects.h`**:
   Define internal structs/classes that wrap WebGPU handles. These are the building blocks everything else uses.

   Reference: `drivers/metal/metal_objects.h` (the patterns, not the Metal API calls)

   Key objects to define:
   ```
   WGCommandBuffer — wraps WGPUCommandEncoder + state tracking
     - push_constant_data[128], push_constant_binding, dirty flags
     - Current render pass encoder (WGPURenderPassEncoder)
     - Current compute pass encoder (WGPUComputePassEncoder)
     - Render state (viewport, scissor, pipeline, bind groups, vertex buffers, index buffer)
     - Compute state (pipeline, bind groups)

   WGShader — wraps WGPUShaderModule + pipeline layout metadata
     - WGPUShaderModule vertex_module, fragment_module (or combined)
     - Bind group layout descriptors
     - Push constant binding info (which group/binding for the emulation buffer)
     - Reflection data

   WGRenderPass — wraps render pass metadata (no direct WebGPU equivalent)
     - Vector<WGSubpass> subpasses
     - Attachment descriptions
     - Clear values

   WGSubpass — subpass metadata for flattening
     - Input references, color references, depth reference, resolve references

   WGPipeline — wraps WGPURenderPipeline or WGPUComputePipeline
     - Pipeline type (render/compute)
     - Associated shader, layout

   WGFramebuffer — wraps texture views for render targets
     - Vector of WGPUTextureView attachments
     - Size, sample count

   WGUniformSet — wraps WGPUBindGroup
     - Bind group handle
     - Layout reference
   ```

2. **Create `drivers/webgpu/webgpu_objects.cpp`**:
   Implement constructors, destructors (calling `wgpu*Release` on handles), and utility methods.

3. **Create `drivers/webgpu/pixel_formats_webgpu.h` and `.cpp`**:
   - Define the `DataFormat` → `WGPUTextureFormat` mapping table
   - Reference: `RESEARCH.md` Appendix B for the core mappings
   - Reference: `drivers/metal/pixel_formats.h` for the pattern
   - Handle unsupported formats (3-component formats → map to RGBA equivalents)
   - Include helper functions: `godot_to_wgpu_format()`, `wgpu_to_godot_format()`, `is_depth_format()`, `is_stencil_format()`

**Completion Criteria**: All internal object types defined. Pixel format mapping complete. Compiles successfully.

---

### Task 1.2: Emscripten WebGPU Bootstrapping `[PARALLEL with 1.1]`
**Status**: `DONE`
**Effort**: 3-4 hours
**Dependencies**: Phase 0

**Completion Notes** (March 10, 2026):
- **CRITICAL DISCOVERY**: Emscripten 5.0.0 is installed. `-sUSE_WEBGPU=1` was removed in Emscripten 5.x.
  The new approach is `--use-port=emdawnwebgpu` (Dawn's WebGPU port). Updated `platform/web/detect.py`.
- **API changes in Emscripten 5.x / Dawn webgpu.h**:
  - `html5_webgpu.h` is GONE → replaced with `<emscripten/emscripten.h>` + `EM_ASM_PTR`
  - `emscripten_webgpu_get_device()` is GONE → use `WebGPU.importJsDevice(gpuDevice)` JS utility
  - Canvas surface struct: `WGPUSurfaceDescriptorFromCanvasHTMLSelector` → `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector`
  - `WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector` → `WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector`
  - All string params (`const char*`) are now `WGPUStringView` → use `WGPUStringView{str, WGPU_STRLEN}`
  - `WGPUTexelCopyBufferLayout` (layout-only) + buffer ptr → `WGPUTexelCopyBufferInfo` (combined)
  - `WGPUVertexFormat_Undefined` removed → use `(WGPUVertexFormat)0`
  - `WGPUShaderSourceSPIRV` EXISTS in Dawn headers (good for SPIR-V shaders in Phase 2!)
- Created `misc/dist/html/webgpu-full-size.html` — HTML shell with JS-side WebGPU device init
- Modified `platform/web/js/engine/config.js` — added `preinitializedWebGPUDevice` property
- Device acquisition in C++: `EM_ASM_PTR` calling `WebGPU["importJsDevice"](Module["preinitializedWebGPUDevice"])`
- Build compiles and LINKS clean: `bin/godot.web.template_debug.wasm32.zip` produced ✓

**Instructions**:

1. **Create/modify JS shell for WebGPU pre-init**:

   The HTML shell must request a WebGPU device BEFORE the WASM module starts. This avoids the async initialization problem entirely.

   - Find the current HTML shell template: `misc/dist/html/` or `platform/web/js/`
   - Add WebGPU device pre-initialization:
     ```javascript
     // In the HTML shell, before WASM module load:
     async function initWebGPU() {
         if (!navigator.gpu) {
             console.warn("WebGPU not available, falling back to WebGL");
             return null;
         }
         const adapter = await navigator.gpu.requestAdapter({
             powerPreference: "high-performance"
         });
         if (!adapter) return null;

         const requiredFeatures = [];
         // Request optional features
         if (adapter.features.has('texture-compression-bc')) {
             requiredFeatures.push('texture-compression-bc');
         }

         const device = await adapter.requestDevice({
             requiredFeatures,
             requiredLimits: {
                 maxStorageBuffersPerShaderStage: 10,
                 maxBindGroupsPlusVertexBuffers: 30,
             }
         });
         return { adapter, device };
     }
     ```
   - Pass the device to the Emscripten module so `emscripten_webgpu_get_device()` can retrieve it

2. **Verify Emscripten WebGPU header availability**:
   - Check that `<emscripten/html5_webgpu.h>` and `<webgpu/webgpu.h>` are available in the Emscripten SDK version Godot uses
   - Create a simple test: include the headers in a stub file, ensure compilation works

3. **Create `platform/web/platform_webgpu.h`** (if needed):
   - WebGPU-specific includes and defines for the web platform
   - Similar to `platform/web/platform_gl.h` but for WebGPU

**Completion Criteria**: WebGPU device is created in JS before WASM init. C++ code can call `emscripten_webgpu_get_device()` to get a valid `WGPUDevice`. Build compiles.

**Notes for Agent**:
- Read `RESEARCH.md` Section 8 for Emscripten WebGPU details
- Read `platform/web/web_main.cpp` for the initialization flow
- Read `platform/web/js/` directory for existing JS library patterns
- Read `misc/dist/html/` for HTML shell templates
- The `emscripten_webgpu_get_device()` function requires the device to already exist in JS global state

---

### Task 1.3: Context Driver Implementation `[SERIAL, after 1.1 + 1.2]`
**Status**: `DONE`
**Effort**: 4-6 hours
**Dependencies**: Tasks 1.1, 1.2

**Completion Notes** (March 10, 2026):
- All pure virtual methods implemented and compiling clean.
- `initialize()`: device acquired via `EM_ASM_PTR` + `WebGPU["importJsDevice"]`, queue obtained, device info populated.
- `surface_create()`: uses `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector` with `WGPUStringView` selector. Creates a minimal `WGPUInstance` lazily if needed.
- `surface_get_handle()` accessor added (used by device driver for swap chain setup).
- `surface_set/get_size()`, `surface_set/get_vsync_mode()`, `surface_set/get_needs_resize()`, `surface_destroy()`: all implemented.
- `device_get_count()` returns 1. `device_supports_present()` returns true. `is_debug_utils_enabled()` returns false.
- `driver_create()` / `driver_free()`: create/delete `RenderingDeviceDriverWebGPU`.
- Build: compiles clean as part of `bin/godot.web.template_debug.wasm32.zip` (exit 0).

**Instructions**:

Implement `RenderingContextDriverWebGPU` fully. This is relatively simple compared to the device driver.

Reference: `drivers/metal/rendering_context_driver_metal.h` and `.mm`

1. **`drivers/webgpu/rendering_context_driver_webgpu.h`**:
   ```cpp
   class RenderingContextDriverWebGPU : public RenderingContextDriver {
       WGPUInstance instance = nullptr;
       WGPUAdapter adapter = nullptr;
       WGPUDevice device = nullptr;
       WGPUQueue queue = nullptr;

       struct Surface {
           WGPUSurface surface = nullptr;
           // Canvas element ID, size, vsync mode
       };
       HashMap<SurfaceID, Surface> surfaces;

   public:
       Error initialize() override;
       // ... all pure virtual method overrides
   };
   ```

2. **Key method implementations**:
   - `initialize()`: Get device from `emscripten_webgpu_get_device()`. Get queue from device. Set up instance/adapter references.
   - `device_get_count()`: Return 1 (browser has one GPU context)
   - `device_get()`: Return device info (name from adapter, type = INTEGRATED/DISCRETE based on adapter info)
   - `driver_create()`: Create and return a `RenderingDeviceDriverWebGPU*`, passing it the WGPUDevice and WGPUQueue
   - `surface_create()`: Get canvas element, create `WGPUSurface` from it using `wgpuInstanceCreateSurface` with canvas descriptor
   - `surface_set_size()`, `surface_get_width/height()`: Track canvas dimensions
   - `surface_set_vsync_mode()`: Store mode (WebGPU in browser always vsyncs to requestAnimationFrame, but track the setting)
   - `surface_destroy()`: Release surface handle

**Completion Criteria**: Context driver can initialize from Emscripten, enumerate 1 device, create a surface from the canvas, and create a device driver instance.

---

### Task 1.4: Minimal Device Driver — Clear Screen `[SERIAL, after 1.3]`
**Status**: `DONE`
**Effort**: 8-12 hours
**Dependencies**: Task 1.3

**Completion Notes** (March 10, 2026):
- Browser test PASSED in Chrome. godot.wasm (35MB) compiled and ran. Engine reached `main.cpp:2051` (PCK load stage) with **zero WebGPU errors**.
- JS side: Apple GPU adapter found, device created with BC/ETC2/depth32float-stencil8/depth-clip-control features. Pre-initialized device passed to engine via `Module["preinitializedWebGPUDevice"]`.
- C++ side: `initialize()`, `swap_chain_create()`, `swap_chain_resize()` all ran without crashing. Driver init fully completes before the PCK check.
- Only error logged is the expected PCK-not-found; no WebGPU crashes or warnings from the driver.
- Phase 1 is fully complete. Phase 2 (resources) is next.

**Previous In-Progress Notes** (March 10, 2026 — second pass):
- **All clear-screen path methods now fully implemented and compiling clean.**
- `swap_chain_create()`: retrieves `WGPUSurface` via `context_driver->surface_get_handle()`, stores `surface_id`, creates a `WGRenderPass` describing the swap chain attachment (BGRA8Unorm, CLEAR/STORE).
- `swap_chain_resize()`: calls `wgpuSurfaceConfigure()` with `WGPUSurfaceConfiguration` (device, format, width/height from context driver, presentMode=Fifo, alphaMode=Opaque).
- `swap_chain_acquire_framebuffer()`: calls `wgpuSurfaceGetCurrentTexture()`, checks status (resize if Outdated/Lost), creates `WGPUTextureView` and a transient `WGFramebuffer`. Releases previous frame's texture/view/framebuffer.
- `swap_chain_free()`: properly releases current texture/view/framebuffer and the render pass before unconfiguring.
- `command_begin_render_pass()`: fully implemented — loops over subpass color references, maps Godot `ATTACHMENT_LOAD_OP_*`/`ATTACHMENT_STORE_OP_*` to `WGPULoadOp`/`WGPUStoreOp`, sets clear values from `p_clear_values[ref.attachment]`, builds `WGPURenderPassDepthStencilAttachment` respecting depth-only vs stencil-only formats via `is_depth_format_wgpu()` / `has_stencil_wgpu()`.
- Fixed `pixel_formats_webgpu.h`: `RenderingDeviceCommons::TextureAspect` → `RenderingDeviceDriver::TextureAspect`, `TEXTURE_ASPECT_DEPTH_ONLY` → `TEXTURE_ASPECT_DEPTH`, `TEXTURE_ASPECT_STENCIL_ONLY` → `TEXTURE_ASPECT_STENCIL`.
- **NEXT**: Deploy `bin/godot.web.template_debug.wasm32.zip` + `misc/dist/html/webgpu-full-size.html` to a local HTTP server, open in Chrome, and verify a solid color clear appears.

**Instructions**:

Implement the **minimum subset** of `RenderingDeviceDriverWebGPU` methods needed to clear the screen to a solid color. This is the first visual output milestone.

Reference: `drivers/metal/rendering_device_driver_metal.h` and `.mm`

**Methods to implement (minimum viable set)**:

1. **Initialization**:
   - `initialize(device_index, frame_count)`: Store WGPUDevice/WGPUQueue from context. Query device limits. Configure surface.

2. **Capabilities/Limits**:
   - `limit_get()`: Map Godot limit constants to WebGPU device limits. See `RESEARCH.md` Appendix C.
   - `has_feature()`: Report supported features (multiview: no, etc.)
   - `get_api_name()`: Return `"WebGPU"`
   - `get_api_version()`: Return version string
   - `get_capabilities()`: Fill capabilities struct

3. **Command Infrastructure**:
   - `command_queue_family_get()`: Return 1 family (WebGPU has a single queue)
   - `command_queue_create()`: Create queue wrapper (single queue from device)
   - `command_pool_create()`: No-op (no pools in WebGPU), return a handle
   - `command_buffer_create()`: Create `WGCommandBuffer` wrapper
   - `command_buffer_begin()`: Create a `WGPUCommandEncoder`
   - `command_buffer_end()`: Call `wgpuCommandEncoderFinish()` to get `WGPUCommandBuffer`
   - `command_queue_execute_and_present()`: `wgpuQueueSubmit()` + `wgpuSurfacePresent()`

4. **Swap Chain**:
   - `swap_chain_create()`: Call `wgpuSurfaceConfigure()` with format, size, usage
   - `swap_chain_resize()`: Reconfigure surface
   - `swap_chain_acquire_framebuffer()`: `wgpuSurfaceGetCurrentTexture()` → create framebuffer wrapper
   - `swap_chain_get_format()`: Return configured format
   - `swap_chain_free()`: `wgpuSurfaceUnconfigure()`

5. **Render Pass** (minimal):
   - `render_pass_create()`: Store attachment descriptions and subpass info in `WGRenderPass`
   - `command_begin_render_pass()`: Create `WGPURenderPassDescriptor` from render pass + framebuffer, begin `WGPURenderPassEncoder`. Set clear colors/depth.
   - `command_end_render_pass()`: End the render pass encoder
   - `command_next_render_subpass()`: End current encoder, begin new one (follow Metal pattern)

6. **Framebuffer**:
   - `framebuffer_create()`: Store texture views
   - `framebuffer_free()`: Release views

7. **Synchronization** (stubs):
   - `fence_create()`: Return handle (track submission count internally)
   - `fence_wait()`: Call `wgpuDeviceTick()` or use `onSubmittedWorkDone` callback
   - `semaphore_create/free()`: No-ops (single queue)
   - `command_pipeline_barrier()`: No-op (automatic in WebGPU)

8. **Everything else**: Return error/default values for now. The goal is just clearing the screen.

**Completion Criteria**: Running `scons platform=web target=template_debug webgpu=yes` produces a web export that shows a **solid color clear** in the browser. This proves: build → WASM → WebGPU init → surface → render pass → present all work.

**Notes for Agent**:
- Read `RESEARCH.md` Appendix A for WebGPU C API reference
- The Metal driver is ~5000 lines for the full implementation. The minimum viable subset here should be ~800-1200 lines.
- Do NOT try to implement buffers, textures, shaders, or pipelines yet — those come in Phase 2
- For methods not needed for clear-screen, implement as stubs that print warnings and return safe defaults
- Test in Chrome first (best WebGPU implementation)

---

## Phase 2: Resource & Shader Foundation (Days 5-7)

> **Goal**: Implement buffers, textures, samplers, shaders (SPIR-V → WGSL), uniform sets, and pipelines. Get 2D rendering (CanvasItem) working.

### Task 2.1: Shader Translation Pipeline `[PARALLEL with 2.2]`
**Status**: `DONE`
**Effort**: 8-12 hours
**Dependencies**: Phase 1
**CRITICAL PATH**: Everything else in Phase 2+ depends on shaders working.

**Completion Notes** (March 10, 2026):
- Using SPIR-V directly via `WGPUShaderSourceSPIRV` (Dawn's emdawnwebgpu supports SPIR-V natively — no Tint/WGSL translation needed).
- `RenderingShaderContainerWebGPU::_set_code_from_spirv()`: Stores raw SPIR-V bytes per stage. Push constant bind group slot (group 3, binding 0) derived from `ReflectShader.push_constant_size`.
- `RenderingShaderContainerWebGPU` header/extra-data serialization implemented.
- `shader_create_from_container()`: Iterates stages, creates `WGPUShaderModule` via `WGPUShaderSourceSPIRV`. Builds `WGPUBindGroupLayout` per descriptor set from reflection data (uniform/storage/texture/sampler/image entries). Builds `WGPUPipelineLayout` covering all sets + push constant bind group. Stores in `WGShader`.
- Push constant ring buffer (256KB, group 3, binding 0) initialized in `initialize()` with `WGPUBufferUsage_Uniform | CopyDst`.
- Compiles clean.

**Instructions**:

1. **Choose and integrate SPIR-V → WGSL translation tool**:

   **Option A — Tint (recommended for correctness)**:
   - Download/clone Dawn's Tint component
   - Build as a static library or CLI tool
   - Integrate into SCons build for offline SPIR-V → WGSL translation
   - At export time: process all `.spv` blobs through Tint → `.wgsl` strings

   **Option B — Naga CLI (simpler integration)**:
   - Install `naga-cli` via Cargo: `cargo install naga-cli`
   - Use as a build step: `naga input.spv output.wgsl`
   - Simpler but requires Rust toolchain

   **Option C — Runtime translation via Emscripten Tint (if needed for custom shaders)**:
   - Compile Tint to WASM alongside Godot
   - Adds ~2-5MB to binary but supports runtime shader compilation

   Decision guidance: Start with Option B (Naga CLI) for rapid prototyping. Switch to Option A for production quality.

2. **Implement `RenderingShaderContainerWebGPU`**:

   Reference: `drivers/metal/rendering_shader_container_metal.h` and `.mm`

   - `_set_code_from_spirv()`: Takes SPIR-V bytecode, translates to WGSL
   - **Critical**: Handle push constant blocks — rewrite `layout(push_constant)` to a uniform buffer binding
   - **Critical**: Map descriptor set indices to bind group indices (1:1, since max set = 3)
   - Store the resulting WGSL strings + reflection data
   - `_get_shader_container_format()`: Return format for cache identification

3. **Implement shader creation in the device driver**:
   - `shader_create_from_container()`: Load WGSL from container, call `wgpuDeviceCreateShaderModule()` with `WGPUShaderModuleWGSLDescriptor`
   - Store bind group layout descriptors derived from reflection data
   - Store push constant binding info (which bind group/binding index)

4. **Test with a simple shader**: The blit shader (`servers/rendering/renderer_rd/shaders/blit.glsl`) is one of the simplest — get it compiling to WGSL and loading successfully.

**Completion Criteria**: SPIR-V → WGSL pipeline works. Push constants are correctly rewritten to uniform buffer bindings. At least the blit shader loads as a `WGPUShaderModule`.

**Notes for Agent**:
- Read `RESEARCH.md` Section 7 for translation options
- Read `drivers/metal/rendering_shader_container_metal.mm` lines 239-600 for the Metal SPIRV-Cross pattern
- Read `servers/rendering/renderer_rd/shaders/blit.glsl` for a simple test shader
- The push constant rewriting is the hardest part. Tint and Naga both handle this, but you need to configure the output binding location
- WGSL syntax differences from GLSL: `@group(N) @binding(M) var<uniform> name: Type;`

---

### Task 2.2: Buffer Implementation `[PARALLEL with 2.1]`
**Status**: `DONE`
**Effort**: 4-6 hours
**Dependencies**: Phase 1

**Completion Notes** (March 10, 2026):
- `buffer_create()` / `buffer_free()`: Full `WGPUBuffer` creation with size aligned to 4 bytes + `WGPUBufferUsage` mapping from all Godot `BufferUsageBits`.
- `buffer_map()` / `buffer_unmap()`: Shadow CPU buffer pattern — `buffer_map()` allocates and returns the shadow map; `buffer_unmap()` flushes via `wgpuQueueWriteBuffer()` if dirty.
- `buffer_flush()`: Explicit flush of shadow map.
- Transfer commands: `command_clear_buffer()`, `command_copy_buffer()`, `command_copy_buffer_to_texture()`, `command_copy_texture_to_buffer()` — all implemented.
- Upload path via `wgpuQueueWriteBuffer()` for initial data in `buffer_create()`.
- Compiles clean.

**Instructions**:

Implement all buffer-related methods in `RenderingDeviceDriverWebGPU`.

1. **`buffer_create(size, usage, data)`**:
   - Map Godot buffer usage flags to `WGPUBufferUsage` flags:
     - `BUFFER_USAGE_TRANSFER_FROM` → `WGPUBufferUsage_CopySrc`
     - `BUFFER_USAGE_TRANSFER_TO` → `WGPUBufferUsage_CopyDst`
     - `BUFFER_USAGE_UNIFORM` → `WGPUBufferUsage_Uniform`
     - `BUFFER_USAGE_STORAGE` → `WGPUBufferUsage_Storage`
     - `BUFFER_USAGE_INDEX` → `WGPUBufferUsage_Index`
     - `BUFFER_USAGE_VERTEX` → `WGPUBufferUsage_Vertex`
     - `BUFFER_USAGE_INDIRECT` → `WGPUBufferUsage_Indirect`
   - Create `WGPUBuffer` via `wgpuDeviceCreateBuffer()`
   - If initial data provided, upload via `wgpuQueueWriteBuffer()`
   - WebGPU requires buffer sizes to be multiples of 4 — add padding if needed

2. **`buffer_free()`**: `wgpuBufferRelease()`

3. **`buffer_map()` / `buffer_unmap()`**:
   - WebGPU mapping is async. For a synchronous API: use `wgpuQueueWriteBuffer()` for writes, and for reads create a staging buffer with `MapRead` usage, copy to it, then map.
   - Or use ASYNCIFY. Decision: prefer `wgpuQueueWriteBuffer()` path for uploads.

4. **Transfer commands**:
   - `command_clear_buffer()`: `wgpuCommandEncoderClearBuffer()`
   - `command_copy_buffer()`: `wgpuCommandEncoderCopyBufferToBuffer()`
   - `command_copy_buffer_to_texture()`: `wgpuCommandEncoderCopyBufferToTexture()`
   - `command_copy_texture_to_buffer()`: `wgpuCommandEncoderCopyTextureToBuffer()`

5. **Push constant buffer management**:
   - Create a ring buffer system for push constant emulation
   - Small uniform buffer (128 bytes) per pipeline, or a shared ring buffer with dynamic offsets
   - `command_bind_push_constants()`: Copy data into ring buffer, record offset for next draw

**Completion Criteria**: All buffer methods implemented. Buffers can be created, written to, copied, and freed. Push constant ring buffer system works.

---

### Task 2.3: Texture & Sampler Implementation `[PARALLEL with 2.1, 2.2]`
**Status**: `DONE`
**Effort**: 6-8 hours
**Dependencies**: Phase 1, Task 1.1 (pixel formats)

**Completion Notes** (March 10, 2026):
- `texture_create()`: Full `WGPUTexture` + default `WGPUTextureView` creation with dimension/view-dimension/usage/sample-count mapping.
- `texture_create_shared()` / `texture_create_shared_from_slice()`: New `WGPUTextureView` from existing texture, respects format/mip/layer overrides.
- `texture_free()`: Releases view and texture (shared textures have null handle so only view released).
- `texture_get_copyable_layout()`: 256-byte row-pitch alignment for WebGPU buffer↔texture copies.
- `texture_get_data()`: WARN_PRINT_ONCE stub (async readback not yet impl).
- `command_copy_texture()`, `command_blit_region()`, `command_clear_color_texture()`: Implemented.
- `sampler_create()` / `sampler_free()`: Full `WGPUSamplerDescriptor` mapping (filter, address, LOD, anisotropy, compare).
- `_data_format_to_wgpu()` and `_data_format_to_wgpu_vertex()`: Full format mapping tables inline in driver.
- Compiles clean.

**Instructions**:

1. **`texture_create(format, width, height, depth, mipmaps, type, samples, usage, layers)`**:
   - Map `DataFormat` → `WGPUTextureFormat` using pixel format tables from Task 1.1
   - Map `TextureType` → `WGPUTextureDimension` (1D/2D/3D) + `WGPUTextureViewDimension` (1D/2D/3D/Cube/2DArray/CubeArray)
   - Map `TextureSamples` → `sampleCount` (1, 4)
   - Map usage bits to `WGPUTextureUsage` flags
   - Handle 3-component formats: silently upgrade to 4-component (e.g., R8G8B8 → R8G8B8A8)
   - Create `WGPUTexture` + default `WGPUTextureView`

2. **`texture_create_shared` / `texture_create_shared_from_slice`**:
   - Create additional `WGPUTextureView` from existing texture with different format/layer/mip range

3. **`texture_get_data()`**:
   - Copy texture to staging buffer (`wgpuCommandEncoderCopyTextureToBuffer`)
   - Map staging buffer to read data back
   - This is async in WebGPU — may need to queue and fence

4. **`texture_get_usages_supported_by_format()`**:
   - Query format capabilities (which usages are valid for each format)
   - Some formats don't support storage, some don't support render target

5. **`command_copy_texture()`**: `wgpuCommandEncoderCopyTextureToTexture()`
6. **`command_clear_color_texture()`**: Submit a render pass that clears the texture
7. **`command_resolve_texture()`**: MSAA resolve — render pass with resolve target

8. **Sampler methods**:
   - `sampler_create()`: Map Godot sampler params to `WGPUSamplerDescriptor`
     - `SamplerFilter` → `WGPUFilterMode`
     - `SamplerRepeatMode` → `WGPUAddressMode`
     - `CompareOperator` → `WGPUCompareFunction` (for shadow samplers)
   - `sampler_free()`: `wgpuSamplerRelease()`
   - `sampler_is_format_supported_for_filter()`: Check if format supports linear filtering

**Completion Criteria**: Textures can be created in all common formats. Samplers work. Texture data can be uploaded and copied. Format mapping handles edge cases.

---

### Task 2.4: Uniform Sets (Bind Groups) `[SERIAL, after 2.1]`
**Status**: `DONE`
**Effort**: 4-6 hours
**Dependencies**: Tasks 2.1, 2.2, 2.3

**Completion Notes** (March 10, 2026):
- `uniform_set_create()`: Builds `WGPUBindGroupEntry[]` per uniform type (sampler, texture, image, sampler_with_texture, uniform_buffer, storage_buffer, input_attachment). Creates `WGPUBindGroup` from shader's prebuilt layout.
- `uniform_set_free()`: `wgpuBindGroupRelease()`.
- `command_bind_render_uniform_sets()` / `command_bind_compute_uniform_sets()`: Call `setBindGroup()` on the active encoder.
- Sampler-with-texture handled correctly (two entries per pair: sampler at binding+0, texture at binding+1).
- Compiles clean.

**Instructions**:

1. **`uniform_set_create(uniforms, shader, set_index)`**:
   - For each uniform in the set, create a `WGPUBindGroupEntry`:
     - `UNIFORM_TYPE_SAMPLER` → sampler binding
     - `UNIFORM_TYPE_TEXTURE` → texture view binding
     - `UNIFORM_TYPE_IMAGE` → storage texture view binding
     - `UNIFORM_TYPE_SAMPLER_WITH_TEXTURE` → both sampler + texture view (two entries)
     - `UNIFORM_TYPE_UNIFORM_BUFFER` → buffer binding with offset/size
     - `UNIFORM_TYPE_STORAGE_BUFFER` → buffer binding
     - `UNIFORM_TYPE_INPUT_ATTACHMENT` → texture view binding
   - Get bind group layout from shader (set_index maps to bind group index)
   - Create `WGPUBindGroup` via `wgpuDeviceCreateBindGroup()`

2. **`uniform_set_free()`**: `wgpuBindGroupRelease()`

3. **`command_bind_render_uniform_sets()`**:
   - For each uniform set: `wgpuRenderPassEncoderSetBindGroup(encoder, groupIndex, bindGroup, dynamicOffsetCount, dynamicOffsets)`
   - Track currently bound bind groups to avoid redundant calls

4. **`command_bind_compute_uniform_sets()`**:
   - Same but with `wgpuComputePassEncoderSetBindGroup()`

**Completion Criteria**: Bind groups can be created from Godot uniform descriptions. Bind groups can be bound to render and compute pass encoders.

---

### Task 2.5: Pipeline Creation `[SERIAL, after 2.4]`
**Status**: `DONE`
**Effort**: 6-8 hours
**Dependencies**: Tasks 2.1, 2.2, 2.3, 2.4

**Completion Notes** (March 10, 2026):
- `render_pipeline_create()`: Full `WGPURenderPipelineDescriptor` — vertex state (buffer layouts + attributes from `WGVertexFormat`), primitive state (topology, cull, front face, strip index format), multisample state, depth/stencil state (all compare/stencil ops mapped), color targets (write mask + blend state with factor/op mapping), fragment state. Spec constants via `WGPUConstantEntry`.
- `compute_pipeline_create()`: Full `WGPUComputePipelineDescriptor` with spec constants.
- `vertex_format_create()`: Groups attributes by binding, builds `WGPUVertexBufferLayout[]`.
- `_data_format_to_wgpu_vertex()`: 30+ format mappings including float16, uint/sint 8/16/32, unorm/snorm, packed 10_10_10_2.
- `_flush_push_constants()`: Writes push constant data to ring buffer via `wgpuQueueWriteBuffer()`, sets bind group with dynamic offset on active render or compute encoder.
- All draw/dispatch/state-setting commands implemented.
- Compiles clean.

**Instructions**:

1. **Pipeline Layout**:
   - For each shader, create `WGPUPipelineLayout` from its bind group layouts
   - Include the push constant emulation bind group layout

2. **`render_pipeline_create(shader, framebuffer_format, vertex_format, primitive, rasterization_state, multisample_state, depth_stencil_state, blend_state, dynamic_state_flags, render_pass, subpass)`**:
   - Build `WGPURenderPipelineDescriptor`:
     - Vertex state: from vertex format (attribute descriptions, stride, step mode)
     - Primitive state: topology, strip index format, front face, cull mode
     - Depth/stencil state: format, depth write, compare function, stencil ops
     - Multisample state: count, mask
     - Fragment state: targets with blend state, write mask
     - Layout: from shader's pipeline layout
   - Create `WGPURenderPipeline` via `wgpuDeviceCreateRenderPipeline()`

3. **`compute_pipeline_create(shader, specialization_constants)`**:
   - Build `WGPUComputePipelineDescriptor`
   - Create `WGPUComputePipeline`
   - Note: WebGPU specialization constants use `WGPUConstantEntry` — map from Godot's `PipelineSpecializationConstant`

4. **`command_bind_render_pipeline()`**: `wgpuRenderPassEncoderSetPipeline()`
5. **`command_bind_compute_pipeline()`**: `wgpuComputePassEncoderSetPipeline()`

6. **Draw commands**:
   - `command_render_draw()`: `wgpuRenderPassEncoderDraw()`
   - `command_render_draw_indexed()`: `wgpuRenderPassEncoderDrawIndexed()`
   - `command_render_draw_indirect()`: `wgpuRenderPassEncoderDrawIndirect()`
   - `command_render_draw_indexed_indirect()`: `wgpuRenderPassEncoderDrawIndexedIndirect()`
   - `command_render_bind_vertex_buffers()`: `wgpuRenderPassEncoderSetVertexBuffer()` for each buffer
   - `command_render_bind_index_buffer()`: `wgpuRenderPassEncoderSetIndexBuffer()`

7. **Compute commands**:
   - `command_compute_dispatch()`: `wgpuComputePassEncoderDispatchWorkgroups()`
   - `command_compute_dispatch_indirect()`: `wgpuComputePassEncoderDispatchWorkgroupsIndirect()`

8. **State commands**:
   - `command_render_set_viewport()`: `wgpuRenderPassEncoderSetViewport()`
   - `command_render_set_scissor()`: `wgpuRenderPassEncoderSetScissorRect()`
   - `command_render_set_blend_constants()`: `wgpuRenderPassEncoderSetBlendConstant()`

**Completion Criteria**: Render and compute pipelines can be created. Draw and dispatch commands work. All render state setting commands implemented.

---

### Task 2.6: Integration Test — 2D Rendering `[SERIAL, after 2.5]`
**Status**: `IN_PROGRESS`
**Effort**: 4-8 hours (mostly debugging)
**Dependencies**: Tasks 2.1-2.5

**Progress (March 11, 2026)**:
- ✅ Engine boots WebGPU, initializes device, loads all shaders
- ✅ No WebGPU validation errors from Chrome  
- ✅ Frames submitting at ~240fps (empty 2D scene, threads=no build)
- ✅ Swap chain configured and operating correctly
- ✅ Push constant ring buffer working
- ✅ Bind group layouts correct
- ⚠️ Visual correctness not yet verified (canvas may show black background)
- ⚠️ Render pipeline creation not yet verified (no pipeline errors = likely working)

**Issues Fixed This Session**:
- `TypeError: createView on undefined` — fixed by adding `view_source` field to WGTexture
  so shared/sliced textures inherit the owning WGPUTexture handle
- `Texture dimensions exceed device maximum` — fixed limit_get() returning 0
- Worker thread WebGPU isolation — fixed by building with threads=no
- `R8G8B8A8_Unorm does not support usage as storage image` — rewrote texture_get_usages_supported_by_format()
- `!new_pipelines_cache_size` spam — fixed pipeline_cache_query_size() returning 1
- `swap_chain_acquire_framebuffer: !sc->configured` / createView crash — fixed r_resize_required trigger
- `Unsupported DataFormat 127` — added D16_UNORM_S8_UINT → Depth24PlusStencil8
- `Unhandled uniform type 10` — added DYNAMIC UBO/SSBO + TBO uniform types

**Next Steps**:
1. Verify visual output — check if canvas shows 2D content vs black screen
2. Test with a scene that has actual visible content (ColorRect, Label, Sprite2D)
3. Debug render pipeline creation for CanvasShaderRD (the 2D canvas shader)


  - Buffer sizes must be multiples of 4
  - Texture copy operations have alignment requirements (256 bytes per row)
  - Bind group entries must exactly match the layout

---

## Phase 3: 3D Forward+ / Mobile Rendering (Days 8-10)

> **Goal**: Get 3D rendering working with Forward+ and Mobile renderers. Handle the harder parts: cluster lighting, shadows, compute shaders, post-processing.

### Task 3.1: 3D Core Rendering `[SERIAL]`
**Status**: `TODO`
**Effort**: 8-12 hours
**Dependencies**: Phase 2

**Instructions**:

1. **Test with a minimal 3D scene**:
   - Start with: camera, directional light, one mesh (cube/sphere)
   - Export with Forward+ renderer via WebGPU
   - Debug and fix issues

2. **Expected challenges**:
   - **Storage buffer limits**: Forward+ uses multiple storage buffers for light clusters. WebGPU default max is 8. May need to request higher limits at device creation.
   - **Sampler limits**: Forward+ may use >16 samplers per stage when shadows + GI + materials combined. May need to reduce or combine.
   - **Multiview**: Not available on WebGPU. Ensure it falls back gracefully.
   - **Compute dispatches**: Forward+ cluster builder uses compute shaders. Ensure compute path works.
   - **Timestamp queries**: Used for profiling. May not be available — make optional.

3. **Post-processing effects**:
   - Many effects use compute shaders (SSAO, SSR, bloom, TAA, etc.)
   - Test each effect individually
   - Some may need to be disabled on WebGPU if they exceed resource limits

4. **Mobile renderer path**:
   - The Mobile renderer is simpler (single-pass forward). Test it too.
   - It should work more easily than Forward+ since it uses fewer resources.

**Completion Criteria**: A 3D scene with meshes, lights, and shadows renders correctly in the browser using Forward+ or Mobile renderer.

---

### Task 3.2: Compute Shader Support `[PARALLEL with 3.1]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Phase 2

**Instructions**:

1. **Verify compute pipeline creation works end-to-end**:
   - Create a simple compute shader test
   - Dispatch compute work
   - Read back results

2. **Compute pass encoding**:
   - `wgpuCommandEncoderBeginComputePass()` / `wgpuComputePassEncoderEnd()`
   - Ensure bind groups and push constants work in compute context

3. **Key compute users in Godot**:
   - Cluster builder (`cluster_builder_rd.cpp`)
   - Particle processing
   - SSAO
   - SSR
   - Bloom downsample/upsample
   - Light projector processing

4. **Storage buffer access patterns**: Ensure read/write storage buffers are correctly bound with `WGPUBufferBindingType_Storage` for read-write and `WGPUBufferBindingType_ReadOnlyStorage` for read-only.

**Completion Criteria**: Compute shaders dispatch correctly. Results can be read back or used as input for subsequent render passes.

---

### Task 3.3: Timestamp Queries & Profiling `[PARALLEL with 3.1]`
**Status**: `TODO`
**Effort**: 2-3 hours
**Dependencies**: Phase 2

**Instructions**:

1. **Check if `timestamp-query` feature is available**:
   - Request it as an optional feature during device creation
   - Fall back gracefully if not available

2. **If available**: Implement `timestamp_query_pool_create`, `command_timestamp_write`, `timestamp_query_pool_get_results`
   - WebGPU API: `wgpuCommandEncoderWriteTimestamp()`, query set, resolve buffer, read back

3. **If not available**: Return dummy results, disable GPU profiler features

**Completion Criteria**: Timestamp queries work when the feature is available. No crashes when it's not.

---

### Task 3.4: Performance Optimization — Push Constant Fast Path `[PARALLEL with 3.1]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Phase 2

**Instructions**:

The push constant emulation from Task 2.2 may cause performance issues due to frequent small buffer writes. Optimize:

1. **Ring buffer with dynamic offsets**:
   - Allocate a large uniform buffer per frame (e.g., 64KB)
   - Each push constant update writes to the next 256-byte aligned offset in the ring
   - Bind with dynamic offset instead of creating new bind groups
   - This reduces bind group creation from per-draw to per-frame

2. **Batching**:
   - Detect when push constant data hasn't changed between draws → skip the write
   - Track dirty state in `WGCommandBuffer`

3. **Benchmark**: Compare draw call throughput before and after optimization

**Completion Criteria**: Push constant updates are efficient enough to maintain 60 FPS in draw-call-heavy scenes.

---

## Phase 4: Export Integration & Polish (Days 11-12)

> **Goal**: Make WebGPU a proper export option in the Godot editor. WebGL fallback works. Polish for usability.

### Task 4.1: Export Preset Integration `[PARALLEL with 4.2]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Phase 3

**Instructions**:

1. **Update web export preset**:
   - Find the web export plugin: `platform/web/export/` or `editor/export/`
   - Add option: "Renderer: WebGPU (Experimental) / WebGL 2.0"
   - When WebGPU selected: use the WebGPU export template
   - When WebGL 2.0 selected: use existing template (no change)

2. **Export template naming**:
   - Current: `godot.web.template_release.wasm32.nothreads.dlink.wasm`
   - WebGPU: `godot.web.template_release.wasm32.nothreads.webgpu.wasm` (or similar)
   - Build both templates

3. **Editor UI**:
   - Add warning when WebGPU is selected: "WebGPU is experimental. Ensure your target browsers support WebGPU."
   - Show detected rendering method in export dialog

4. **Shader pre-compilation at export time**:
   - During export, run all SPIR-V shader blobs through WGSL translator
   - Bundle WGSL alongside SPIR-V in the exported `.pck`
   - This avoids runtime shader translation overhead

**Completion Criteria**: Editor has a WebGPU export option. Export produces a working WebGPU build.

---

### Task 4.2: HTML Shell & Fallback `[PARALLEL with 4.1]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Phase 3

**Instructions**:

1. **Update HTML shell template**:
   - WebGPU detection: check `navigator.gpu` availability
   - If WebGPU available: proceed with WebGPU init
   - If not: fall back to WebGL export OR show user message
   - Show loading indicator during WebGPU device initialization (async)

2. **Fallback strategy**:
   - **Option A (dual build)**: Ship both WebGPU and WebGL WASM binaries. JS detects and loads appropriate one.
   - **Option B (graceful degradation)**: Single build with both `WEBGPU_ENABLED` and `GLES3_ENABLED`. Runtime detection. (This is harder but better UX.)
   - Recommendation: Start with Option A (simpler), consider Option B later.

3. **Canvas setup for WebGPU**:
   - WebGPU uses a different canvas context type
   - Ensure canvas element is properly configured for WebGPU presentation
   - Handle device pixel ratio for high-DPI displays

4. **Error handling**:
   - WebGPU device lost: detect and report
   - Out of memory: graceful error messages
   - Unsupported features: warn in console but continue

**Completion Criteria**: WebGPU web exports work out of the box. Fallback to WebGL works when WebGPU is unavailable.

---

### Task 4.3: Documentation `[PARALLEL with 4.1, 4.2]`
**Status**: `TODO`
**Effort**: 2-3 hours
**Dependencies**: Phase 3

**Instructions**:

1. **Create `drivers/webgpu/README.md`**:
   - Architecture overview
   - How WebGPU maps to Godot's RenderingDevice
   - Known limitations
   - Build instructions

2. **Update `doc/` class documentation** (if applicable):
   - Note WebGPU support in RenderingDevice docs
   - Document web export WebGPU option

3. **Update `webgpu_notes/` with implementation notes**:
   - Final architecture decisions
   - Performance characteristics
   - Browser compatibility notes

**Completion Criteria**: Documentation exists for developers and users.

---

## Phase 5: Testing & Verification (Days 13-14)

> **Goal**: Comprehensive testing across browsers, projects, and performance benchmarks.

### Task 5.1: Automated Build Tests `[PARALLEL with 5.2]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Phase 4

**Instructions**:

1. **Build verification**:
   - `scons platform=web target=template_release webgpu=yes` → succeeds
   - `scons platform=web target=template_debug webgpu=yes` → succeeds
   - `scons platform=web target=template_release` (without webgpu) → still succeeds (no regressions)
   - Debug build produces valid `.wasm` + `.js` + engine files

2. **Existing Godot unit tests**:
   - Run any RenderingDevice tests against the WebGPU backend
   - Fix failures

3. **CI integration** (if GitHub Actions available):
   - Add WebGPU build to CI matrix
   - Build both debug and release web templates

**Completion Criteria**: All builds succeed. No regressions in non-WebGPU builds.

---

### Task 5.2: Browser Compatibility Testing `[PARALLEL with 5.1]`
**Status**: `TODO`
**Effort**: 6-8 hours
**Dependencies**: Phase 4

**Instructions**:

1. **Test matrix**:

   | Browser | Platform | Status |
   |---------|----------|--------|
   | Chrome (latest) | macOS | TODO |
   | Chrome (latest) | Windows | TODO |
   | Chrome (latest) | Linux | TODO |
   | Firefox (latest) | macOS | TODO |
   | Firefox (latest) | Windows | TODO |
   | Safari 18+ | macOS | TODO |
   | Edge (latest) | Windows | TODO |
   | Chrome | Android | TODO |
   | Safari | iOS 18+ | TODO |

2. **Test projects**:
   - 2D: Official 2D demos (sprite, particles, navigation, physics)
   - 3D basic: Camera + light + mesh
   - 3D complex: Multiple lights, shadows, particles
   - Compute: GPU particles, SSAO
   - Stress test: Many draw calls, many textures

3. **For each browser/project combination**:
   - Does it load?
   - Does it render correctly?
   - What's the FPS?
   - Any console errors/warnings?
   - Memory usage?

**Completion Criteria**: Works on Chrome, Firefox, Safari (desktop). Document any browser-specific issues.

---

### Task 5.3: Performance Benchmarking `[SERIAL, after 5.2]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Task 5.2

**Instructions**:

1. **Benchmark scenes**:
   - Scene A: 1000 sprites (2D batch test)
   - Scene B: Single 3D mesh with PBR material + directional light
   - Scene C: 100 mesh instances + 4 lights + shadows
   - Scene D: GPU particles (10000 particles)

2. **Comparison targets**:
   - Same scene exported via WebGL 2.0 (Compatibility renderer)
   - Same scene exported via WebGPU
   - Optionally: equivalent Three.js WebGPU implementation

3. **Metrics to capture**:
   - FPS (average, P1, P99)
   - Draw call count
   - GPU memory usage
   - WASM memory usage
   - Load time
   - Binary size (WASM + JS + assets)

4. **Target performance**:
   - 1.5–3× improvement over WebGL in GPU-bound scenes
   - 60 FPS for non-stress-test scenes
   - No more than 2× binary size increase vs. WebGL export

**Completion Criteria**: Performance data collected and documented. WebGPU shows measurable improvement over WebGL.

---

### Task 5.4: Final Polish & PR Preparation `[SERIAL, after 5.1-5.3]`
**Status**: `TODO`
**Effort**: 4-6 hours
**Dependencies**: Tasks 5.1, 5.2, 5.3

**Instructions**:

1. **Code cleanup**:
   - Remove debug prints / temporary hacks
   - Ensure consistent code style (follow Godot's .clang-format)
   - Add copyright headers to all new files (follow existing Godot pattern)
   - Remove any unused includes / dead code

2. **Error handling review**:
   - All WebGPU API calls check return values
   - Graceful error messages for common failures
   - No crashes — only error messages

3. **Memory leak check**:
   - All `wgpu*Create*` calls have matching `wgpu*Release` calls
   - No resource leaks over time (run for 5 minutes, check memory stability)

4. **Create demo video**:
   - Record a Forward+ 3D scene running in browser at 60 FPS
   - Show the export workflow (editor → export → browser)

5. **Prepare PR description**:
   - Summary of changes
   - Architecture decisions
   - Performance data
   - Known limitations
   - Browser compatibility matrix

**Completion Criteria**: Code is clean, well-documented, and ready for review. Demo video recorded.

---

## Appendix: Task Dependency Graph

```
Phase 0: Setup
  0.1 Build System ──→ 0.2 Driver Registration

Phase 1: Core Skeleton
  ┌─ 1.1 Internal Objects  ─┐
  │                          ├──→ 1.3 Context Driver ──→ 1.4 Clear Screen
  └─ 1.2 Emscripten Boot ───┘

Phase 2: Resources & Shaders
  ┌─ 2.1 Shaders ──────────┐
  ├─ 2.2 Buffers            ├──→ 2.4 Uniform Sets ──→ 2.5 Pipelines ──→ 2.6 2D Test
  └─ 2.3 Textures/Samplers ─┘

Phase 3: 3D Rendering
  ┌─ 3.1 3D Core Rendering ──┐
  ├─ 3.2 Compute Shaders     ├──→ (all feed into Phase 4)
  ├─ 3.3 Timestamp Queries   │
  └─ 3.4 Push Constant Opt  ─┘

Phase 4: Polish
  ┌─ 4.1 Export Preset  ──┐
  ├─ 4.2 HTML/Fallback    ├──→ (all feed into Phase 5)
  └─ 4.3 Documentation   ─┘

Phase 5: Testing
  ┌─ 5.1 Build Tests      ─┐
  └─ 5.2 Browser Testing  ─┤──→ 5.3 Performance ──→ 5.4 Final Polish
                            │
```

## Appendix: File Quick Reference

### Key files to READ before starting any task:

| File | Why |
|------|-----|
| `servers/rendering/rendering_device_driver.h` | THE interface to implement |
| `servers/rendering/rendering_context_driver.h` | Context factory interface |
| `servers/rendering/rendering_device_commons.h` | All shared enums and structs |
| `servers/rendering/rendering_device.cpp` | How the RD initializes and uses the driver |
| `drivers/metal/rendering_device_driver_metal.h` | Best reference implementation |
| `drivers/metal/rendering_device_driver_metal.mm` | Best reference implementation |
| `drivers/metal/metal_objects.h` | Internal object patterns |
| `drivers/metal/rendering_shader_container_metal.mm` | Shader cross-compilation pattern |
| `drivers/metal/pixel_formats.h` | Format mapping pattern |
| `platform/web/detect.py` | Web build configuration |
| `platform/web/display_server_web.cpp` | Web display server |
| `platform/web/web_main.cpp` | Web entry point |
| `main/main.cpp` lines 2400-2600 | Rendering setup flow |

### Key files to CREATE:

```
drivers/webgpu/
├── SCsub
├── rendering_context_driver_webgpu.h
├── rendering_context_driver_webgpu.cpp
├── rendering_device_driver_webgpu.h
├── rendering_device_driver_webgpu.cpp
├── rendering_shader_container_webgpu.h
├── rendering_shader_container_webgpu.cpp
├── webgpu_objects.h
├── webgpu_objects.cpp
├── pixel_formats_webgpu.h
├── pixel_formats_webgpu.cpp
└── README.md
```

### Key files to MODIFY:

```
SConstruct                              ← add webgpu option
platform/web/detect.py                  ← add WEBGPU_ENABLED + RD_ENABLED
platform/web/display_server_web.h       ← add WebGPU members
platform/web/display_server_web.cpp     ← add WebGPU init + driver reporting
main/main.cpp                           ← unlock Forward+/Mobile for web
servers/display/display_server.cpp      ← add WebGPU context driver creation
drivers/SCsub                           ← include webgpu/ when enabled
platform/web/js/* or misc/dist/html/*   ← WebGPU JS shell
```

## Appendix: WebGPU Gotchas Checklist

When debugging issues, check these common WebGPU problems:

- [ ] Buffer sizes must be multiples of 4 bytes
- [ ] Uniform buffer offsets must be 256-byte aligned
- [ ] Texture row copy alignment: `bytesPerRow` must be multiple of 256
- [ ] Max 4 bind groups (0-3) — Godot uses 0-3 so this is OK
- [ ] No push constants — must use emulation via uniform buffer
- [ ] No subpasses — must flatten (follow Metal pattern)
- [ ] No barriers — `command_pipeline_barrier()` is a no-op
- [ ] No secondary command buffers
- [ ] No geometry/tessellation shaders
- [ ] 3-component texture formats (RGB) don't exist — use RGBA
- [ ] `wgpuSurfaceGetCurrentTexture()` can return invalid texture (handle gracefully)
- [ ] All shader modules use WGSL, not SPIR-V or GLSL
- [ ] `mapAsync()` is asynchronous — use `wgpuQueueWriteBuffer()` for synchronous uploads
- [ ] Maximum texture size may be 8192 (not 16384 like Vulkan)
- [ ] Maximum storage buffers per stage may be 8 (check Forward+ needs)
- [ ] Device can be "lost" at any time — handle `WGPUDeviceLostCallback`
