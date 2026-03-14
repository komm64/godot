# naga-converter

A Rust/WASM library that converts Godot's SPIR-V shader bytecode to WGSL at runtime in the browser.

## Purpose

The WebGPU rendering driver submits SPIR-V bytecode (produced by glslang) to this WASM module, which:
1. Runs several SPIR-V preprocessing passes (push-constant → UBO, readonly storage inference, binding array flattening, etc.)
2. Translates SPIR-V → WGSL via a patched version of [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga)
3. Returns the WGSL string to the C++ engine via a JavaScript bridge in `platform/web/js/engine/engine.js`

## Pre-built binary

The compiled WASM is committed at `prebuilt/naga_wasm_bg.wasm`. It is automatically included in the web export template zip during a WebGPU build (`scons platform=web webgpu=yes`).

## Rebuilding

Requirements: Rust (stable), `wasm-pack`

```bash
cd drivers/webgpu/naga-converter
wasm-pack build --target web --release
cp pkg/naga_converter_bg.wasm prebuilt/naga_wasm_bg.wasm
```

Then rebuild the web template to pick up the new WASM:
```bash
source /path/to/emsdk/emsdk_env.sh
scons platform=web target=template_release webgpu=yes opengl3=no threads=no
```

## naga-patched

The `naga-patched/` directory contains a vendored copy of naga with patches applied for Godot's needs:
- SPIR-V `BuiltIn` mappings for Godot's custom builtins
- WGSL output tuned for WebGPU spec compliance with Dawn/Chrome

The patched naga is referenced via `Cargo.toml`'s `[patch.crates-io]` section.
