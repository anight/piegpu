// JetGpu.hpp - Jet's pixel work on the Pi Zero's V3D (through pgl).
//
// Jet transforms, lights and queues triangles on the Pico as before; where it
// would write pixels, the GPU does instead:
//
//   * Rasterizer::drawTriangle (JetGpu.cpp replaces Renderer.cpp): Jet's own
//     lighting per vertex, the triangles batched into GL draws in Jet's order
//     (painter's order: no example uses a depth buffer)
//   * Scene::clearBuffers: the background colour or per-row gradient
//   * Scene::drawSprites: textured (colour-keyed, scaled, flipped, mirrored)
//     or solid quads, alpha or additive
//   * the CRT post-effect: scanlines as a blended overlay
//
// The hooks in Jet are marked "pico-gpu" (Scene.cpp, under JET_GPU).
#pragma once

#include <cstdint>

namespace Renderer { struct Sprite2D; class Texture; }

namespace JetGpu {

/// Once, after pglInit: the scene's size and its place on the panel (top row).
void init(int width, int height, int top);

/// Around each frame (the runtime).
void beginFrame();
void endFrame();

/// Scene hooks.
void clear(const uint16_t* gradient, uint16_t color, int rows);
void sprite(const Renderer::Sprite2D& sprite, uint8_t alpha);
void crt(uint8_t intensity);

/// Frame statistics: GL draws and triangles of the last frame.
int lastDraws();
int lastTriangles();

/// JET_PROFILE: microseconds spent since the last call in texture checks
/// (and uploads), in GL draw calls (flush), and in drawTriangle.
void profile(uint32_t* textures, uint32_t* draws, uint32_t* triangles);

}  // namespace JetGpu
