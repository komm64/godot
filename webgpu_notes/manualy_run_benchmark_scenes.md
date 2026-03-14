## WebGL scene A:

fps ~119
scene loads with sprites visible in different colors, as expected

command:
```
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_a && open -a "Google Chrome" http://localhost:8080 && python3 -m http.server 8080
```

## WebGPU scene A:
black screen after the initial godot loading screen
console info:
```
Godot Engine v4.6.stable.custom_build.6c3d5c68f (2026-03-13 20:33:46 UTC) - https://godotengine.org
index.js:475 Build configuration: Emscripten 5.0.0, single-threaded, no GDExtension support.
index.js:475 BENCHMARK_RESULT|scene_a_sprites|avg=119.9|p1=120.0|p99=122.0|frames=1313|duration=11.0
```
command:
```
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_a && open -a "Google Chrome" http://localhost:8080 && python3 -m http.server 8080
```


____


Single Sprite Examples

Both exported. Here are the commands:

## WebGL (1 sprite):



```
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_minimal && open -a "Google Chrome" http://localhost:8080 && python3 -m http.server 8080
```



## WebGPU (1 sprite):

```
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_minimal && open -a "Google Chrome" http://localhost:8080 && python3 -m http.server 8080
```

You should see a single red circle bouncing around. If WebGPU still shows a black screen, it confirms the rendering pipeline issue (not a scene complexity problem).



___





**IMPORTANT:** `serve.py` now auto-kills any old server on port 8080. Start the server first (background `&`), wait 0.5s, then open Chrome. This ensures the right scene is served.

WebGPU (run one at a time — the next command auto-kills the previous server):
```
# Scene A - Sprites  tmp/benchmarks/scene_a_sprites/benchmark.gd
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_a_sprites --export-release WebGPU
# 5k sprites @ 120fps (maxed out)
# 10k sprites @ 120fps (still)
# 20k sprites @ 60fps (vsync cap hit)
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_a && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene B - PBR
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_b && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene C - Instances  tmp/benchmarks/scene_c_instances/benchmark.gd
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_c_instances --export-release WebGPU
# 14.1 fps on 5k cubes
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_c && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene D - Particles  tmp/benchmarks/scene_d_particles/benchmark.gd
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_d_particles --export-release WebGPU
# 37.7fps on 500k particles
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgpu/scene_d && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080
```


WebGL (same pattern):
```
# Scene A - Sprites  tmp/benchmarks/scene_a_sprites/benchmark.gd
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_a_sprites --export-release WebGL
# 5k sprites @ 38fps
# 10k sprites @ 17fps (linear drop, vsync-bound)
# 20k sprites @ 7.5fps (still linear drop)
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_a && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene B - PBR
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_b && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene C - Instances
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_c_instances --export-release WebGL

# 31.5 fps fps on 5k cubes
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_c && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080

# Scene D - Particles
bin/godot.macos.editor.arm64 --headless --path tmp/benchmarks/scene_d_particles --export-release WebGL
# 23.4fps on 500k particles
cd /Users/dwalter/Documents/projects/godotwebgpu/godot/tmp/benchmarks/exports/webgl/scene_d && python3 ../../../serve.py & sleep 0.5 && open -a "Google Chrome" http://localhost:8080
```

