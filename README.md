<p align="center">
  <img src="logo_outlined.svg" width="100" alt="Godot Engine logo">
</p>

<h1 align="center">Godot <strong>WebGPU</strong></h1>

<p align="center">
  <strong>The full power of GPU rendering in the browser.</strong>
</p>

<p align="center">
  <a href="https://godotwebgpu.com"><img src="https://godotwebgpu.com/screenshots/3d_platformer_final.png" width="720" alt="Godot WebGPU — 3D Platformer Demo"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Godot-4.6.2-478cbf?style=flat-square" alt="Godot 4.6.2">
  <img src="https://img.shields.io/badge/WebGPU-1.0-478cbf?style=flat-square" alt="WebGPU 1.0">
  <img src="https://img.shields.io/badge/Chrome-113+-478cbf?style=flat-square" alt="Chrome 113+">
  <img src="https://img.shields.io/badge/Safari-18+-478cbf?style=flat-square" alt="Safari 18+">
  <img src="https://img.shields.io/badge/Firefox-120+-478cbf?style=flat-square" alt="Firefox 120+">
  <img src="https://img.shields.io/badge/Chrome_Android-supported-478cbf?style=flat-square" alt="Chrome Android">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Compute_Shaders-supported-478cbf?style=flat-square" alt="Compute Shaders">
  <img src="https://img.shields.io/badge/Mobile_Renderer-supported-478cbf?style=flat-square" alt="Mobile Renderer">
  <img src="https://img.shields.io/badge/CI-passing-3fb950?style=flat-square" alt="CI Passing">
  <img src="https://img.shields.io/badge/All_Demos-passing-3fb950?style=flat-square" alt="All Demos Passing">
  <img src="https://img.shields.io/badge/AI_Generated-Claude_Opus_4.6-8957e5?style=flat-square" alt="AI Generated">
</p>

<p align="center">
  <a href="https://shinygen.ai"><img src="https://img.shields.io/badge/Made_for-Shiny_Gen-8957e5?style=flat-square" alt="Made for Shiny Gen"></a>
  <a href="#documentation"><img src="https://img.shields.io/badge/Documentation-view-478cbf?style=flat-square" alt="Documentation"></a>
  <a href="https://github.com/nicemicro/godot-webgpu"><img src="https://img.shields.io/badge/GitHub-Repo-478cbf?style=flat-square" alt="GitHub Repo"></a>
  <a href="https://github.com/godotengine/godot-proposals/issues/6646#issuecomment-4362021374"><img src="https://img.shields.io/badge/Proposal-%236646-478cbf?style=flat-square" alt="Proposal #6646"></a>
  <img src="https://img.shields.io/badge/Free_%26_Open_Source-MIT-478cbf?style=flat-square" alt="Free & Open Source">
</p>

<br>

<table align="center">
  <tr>
    <td align="center"><strong>10</strong><br><sub>Demos</sub></td>
    <td align="center"><strong>6</strong><br><sub>Benchmarks</sub></td>
    <td align="center"><strong>0</strong><br><sub>GPU Errors</sub></td>
    <td align="center"><strong>146</strong><br><sub>Shaders Converted</sub></td>
    <td align="center"><strong>296</strong><br><sub>New Files</sub></td>
    <td align="center"><strong>198K</strong><br><sub>Lines Added</sub></td>
    <td align="center"><strong>5x</strong><br><sub>FPS vs WebGL</sub></td>
    <td align="center"><strong>80%</strong><br><sub>FPS vs Native</sub></td>
  </tr>
</table>

<br>

> **Looking for the original Godot Engine README?** See [GODOT_README.md](GODOT_README.md).

---

## Frequently Asked Questions

Common questions about the WebGPU backend — architecture, performance, compatibility, and more.

**[Read the FAQ &rarr;](webgpu_site/FAQ.md)**

---

## Development Journey

I'm [David Walter](https://x.com/davidpwalter), the developer of [Shiny Gen](https://shinygen.ai). I needed better performance on Godot Web and compute shader support for [Shiny Gen](https://shinygen.ai). Instead of switching to three.js, I decided to stick with Godot and implement WebGPU support. Now Shiny Gen runs on Godot WebGPU — super optimized and with compute shaders.

| Date | Milestone |
|------|-----------|
| **March 10, 2026** | Started development. Forked Godot 4.6.2, began implementing the WebGPU rendering driver. |
| **March — May 2026** | Built the full WebGPU backend over ~2 months, working for 4-12 hours a day with Claude Opus 4.6. |
| **May 10, 2026** | Public beta release. 146 shaders precompiled, 10 demos, 6 benchmarks, zero GPU errors across Chrome, Firefox, and Safari. |

---

## Live Demos

Try each demo in your browser at **[godotwebgpu.com](https://godotwebgpu.com)**. WebGPU requires Chrome 113+ or Safari 18+.

<table>
<tr>
<td align="center" width="33%">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/3d_platformer_final.png" width="300" alt="3D Platformer"><br>
<strong>3D Platformer</strong>
</a><br>
<sub>3D &bull; PBR materials, directional shadows, CharacterBody3D</sub>
</td>
<td align="center" width="33%">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/3d_lights_and_shadows_final.png" width="300" alt="Lights & Shadows"><br>
<strong>Lights & Shadows</strong>
</a><br>
<sub>3D &bull; Directional, omni, spot lights with PCSS shadows</sub>
</td>
<td align="center" width="33%">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/3d_particles_final.png" width="300" alt="GPU Particles 3D"><br>
<strong>GPU Particles 3D</strong>
</a><br>
<sub>3D &bull; Compute-driven particles: fire, burst effects &bull; <strong>NEW on Web</strong></sub>
</td>
</tr>
<tr>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/2d_particles_final.png" width="300" alt="GPU Particles 2D"><br>
<strong>GPU Particles 2D</strong>
</a><br>
<sub>2D &bull; Particle trails, collision, multiple emitters &bull; <strong>NEW on Web</strong></sub>
</td>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/2d_platformer_final.png" width="300" alt="2D Platformer"><br>
<strong>2D Platformer</strong>
</a><br>
<sub>2D &bull; Sprites, parallax backgrounds, physics</sub>
</td>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/2d_sprite_shaders_final.png" width="300" alt="Sprite Shaders"><br>
<strong>Sprite Shaders</strong>
</a><br>
<sub>2D &bull; Outline, blur, shadow, silhouette effects</sub>
</td>
</tr>
<tr>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/compute_heightmap_final.png" width="300" alt="Compute Heightmap"><br>
<strong>Compute Heightmap</strong>
</a><br>
<sub>Compute &bull; GPU compute shader generating terrain heightmap &bull; <strong>NEW on Web</strong></sub>
</td>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/compute_texture_final.png" width="300" alt="Compute Texture"><br>
<strong>Compute Texture</strong>
</a><br>
<sub>Compute &bull; Compute shader populating textures in real-time &bull; <strong>NEW on Web</strong></sub>
</td>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/viewport_gui_in_3d_final.png" width="300" alt="GUI in 3D"><br>
<strong>GUI in 3D</strong>
</a><br>
<sub>Viewport &bull; SubViewport rendering 2D GUI on a 3D surface</sub>
</td>
</tr>
<tr>
<td align="center">
<a href="https://godotwebgpu.com/#demos">
<img src="https://godotwebgpu.com/screenshots/gui_control_gallery_final.png" width="300" alt="Control Gallery"><br>
<strong>Control Gallery</strong>
</a><br>
<sub>UI &bull; Full showcase of all Godot UI controls</sub>
</td>
<td></td>
<td></td>
</tr>
</table>

---

## Performance Benchmarks

Stress tests comparing WebGPU (Forward Mobile) vs WebGL (Compatibility) at high object counts. FPS measured on Mac Studio M3 Ultra, Chrome 134.

Try all benchmarks live at **[godotwebgpu.com/#benchmarks](https://godotwebgpu.com/#benchmarks)**.

| | Benchmark | Description | WebGPU FPS | WebGL FPS |
|---|-----------|-------------|:----------:|:---------:|
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_a_final.png" width="120"> | **20K Sprites** | 20,000 bouncing sprites with random colors | **60.1** | 8.8 |
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_b_final.png" width="120"> | **PBR Sphere** | High-poly PBR sphere with directional shadow | **60.1** | 60.0 |
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_c_final.png" width="120"> | **5K Cubes + 5 Lights** | 5,000 rotating cubes with 5 shadow-casting lights | **60.1** | 43.5 |
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_d_final.png" width="120"> | **500K GPU Particles** | 500,000 GPU particles with gradient colors | **60.2** | 32.3 |
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_e_final.png" width="120"> | **Skeletal Animation** | 20 GPU-skinned cylinders with bone animation | **60.2** | N/A |
| <img src="https://godotwebgpu.com/screenshots/webgpu_scene_f_final.png" width="120"> | **PostFX Stack** | SSAO + Bloom + SubViewport with 5 PBR cubes | **60.1** | N/A |

---

## Building

```bash
# WebGPU-only release template:
scons platform=web target=template_release dlink_enabled=yes webgpu=yes opengl3=no threads=no
```

Requirements:
- Emscripten 4.0.10+ (for the emdawnwebgpu port)
- No Rust toolchain needed (naga converter ships as a prebuilt WASM binary)
- Standard Godot build dependencies (SCons, Python, C++ compiler)

---

## Documentation

In-depth technical documentation for the WebGPU backend.

### [Architecture & Design](webgpu_site/ARCHITECTURE_AND_DESIGN.md)

High-level architecture, key design decisions, shader pipeline, and how the WebGPU backend compares to Vulkan/Metal.

### [Technical Reference](webgpu_site/TECHNICAL_REFERENCE.md)

Driver core, command recording, bind groups, shader pipeline details, texture/buffer operations, and build system.

### [Performance & Optimization](webgpu_site/PERFORMANCE_AND_OPTIMIZATION.md)

The journey from 3.25x slower to parity — staging buffers, shadow pass merging, instance batching, and IPC reduction.

### [Correctness & Compatibility](webgpu_site/CORRECTNESS_AND_COMPATIBILITY.md)

Resource lifecycle guarantees, cross-browser compatibility, error handling, testing coverage, and known limitations.

### [Frequently Asked Questions](webgpu_site/FAQ.md)

Common questions about the WebGPU backend — architecture, performance, compatibility, and more.

---

## Key Stats

| Metric | Value |
|--------|-------|
| Total new code | ~12,600 lines |
| Driver implementation | 7,733 lines (single `.cpp`) |
| Shaders converted | 146 (SPIR-V → WGSL via patched Naga) |
| Renderer | Forward Mobile |
| Performance vs native | ~80% of Vulkan/Metal FPS |
| Performance vs WebGL | Up to 5x faster |
| GPU errors | 0 across Chrome, Firefox, Safari |
| Browser support | Chrome 113+, Firefox 120+, Safari 18+ |

---

<p align="center">
  Sponsored by <a href="https://shinygen.ai">Shiny Gen AI</a>
</p>

<p align="center">
  <sub>Godot WebGPU, Shiny Gen AI, and David Walter are not directly affiliated with or endorsed by the Godot Foundation. It uses the GODOT&reg; name and logos under a permissive license granted by the Godot Foundation. Godot WebGPU is a free and open source fork of the Godot Engine. Shiny Gen is a game maker app built with Godot. And David Walter is the developer of Shiny Gen and Godot WebGPU.</sub>
</p>
