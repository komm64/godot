# 4.6-webgpu Branch

This branch is an experimental WebGPU distribution branch for the komm64 Godot fork.

Base:

- `dwalter/godotwebgpu:webgpu-4.6.2`

Additional changes:

- The normal `komm64/godot:4.6` fork patches.
- Small WebGPU driver fixes needed by RD-based browser exports.
- CI changes for manual WebGPU web template builds.

The branch is intentionally separate from `4.6` so it can be replaced by an
official Godot WebGPU implementation later without entangling the stable fork
history.
