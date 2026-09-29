# Jet on piegpu

picojet's demos (`~/picojet`) running on the Pico with the RPi's V3D doing
the pixel work: Jet still transforms, lights, culls and sorts on the Pico, and
where it would rasterise, the GPU draws instead. The Pico keeps no framebuffer.

| Program | Scene | Source |
|---|---|---|
| `jet-template-cube` | Rotating cube | JetExamples |
| `jet-particles` | Particle Lab | JetExamples |
| `jet-textured-boxes` | Textured crate | JetExamples |
| `jet-texture-features` | Texture Lab | JetExamples |
| `jet-postfx-crt` | CRT / Arcade | JetExamples |
| `jet-lod-billboards` | Woodland | JetExamples |
| `jet-sprite-controls` | Air Mail | JetExamples |
| `jet-tropical-island` | Tropical island | JetExamples |
| `jet-sprites-blending` | After Hours | JetExamples |
| `jet-viewer` | Model viewer | picojet's `app/main.cpp`, as a scene |

## Sources

- `Jet/`: Jet (github.com/CubeCoders/Jet) at b412c8793098, as picojet has it:
  MIT, Copyright (c) 2026 CubeCoders Limited (`Jet/LICENSE`). `Renderer.cpp`
  is kept for reference and not built. `Scene.cpp` has three hooks, marked
  "piegpu" under `JET_GPU`: the clear, the CRT effect and the sprite pass.
- `examples/`: nine JetExamples scenes as vendored by picojet (c09f5e566dd6),
  unchanged: MIT, CubeCoders (`examples/LICENSE`).
- `runtime/`: `Runtime.hpp` is upstream's contract (plus a caption line);
  `esp_heap_caps.h`, `esp_system.h` are picojet's; `Runtime.cpp`,
  `Display.hpp` and `JetConfigGpu.hpp` are written for the GPU after picojet's.
- `viewer/`: picojet's model viewer; its `firmware/JetConfig.hpp` is picojet's
  `app/JetConfig.hpp`. Models in `../assets/` (see the note in each header).

## How

`gpu/JetGpu.cpp` replaces Jet's `Renderer.cpp`:

- `Rasterizer::drawTriangle` lights each vertex with Jet's own formula (squared
  Lambert, view-facing specular, per-channel ambient, blow-out above 255) and
  adds the triangle to a batch; batches are GL draws in Jet's order (the
  examples use painter's order, no depth buffer). `shaders/jet.*` interpolate,
  texture (colour key, wrap/clamp/zero addressing, texture LOD) and blend.
- Textures are cached per `Texture` and sent again when they change (Jet has
  no notice of that: textures in RAM are sampled each frame; those in flash
  are const and not looked at - reading them costs ~3 ms a frame through the
  XIP cache); palette textures when their offset moves.
- Sprites are quads (flips, mirrors, scale, alpha or additive); the gradient
  background is a one-texel-wide texture; the CRT scanlines are one quad.
- `WATER_REFLECT` is `shaders/jetwater.*`: Jet's row mirror with its ripple,
  reading the previous frame. A scene with water is drawn into one of two
  textures and then onto the panel, so the next frame can mirror it (copying
  the panel into a texture costs the RPi's ARM about 15 ms a frame). Sprites
  are drawn after that, on the panel: the water never mirrors them (the lens
  flare), as upstream, which composites sprites at scanout.
- Pick queries (the lens flare's occlusion) are answered per triangle on the
  Pico, closest wins, as Jet does.

The runtime keeps one frame in flight: the Pico builds the next frame while
the RPi renders this one. `cmake -DJET_PROFILE=ON` prints where the Pico's
frame time goes (update, render, drawTriangle, texture checks, GL draws, wait).

Not done: PHONG is lit per vertex, perspective-incorrect (affine) texturing
is drawn perspective-correct, and the post-effects the examples don't use
(FXAA, bloom, motion blur, chromatic aberration, pixelate) are not ported.
