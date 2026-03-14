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