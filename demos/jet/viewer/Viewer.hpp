// Viewer.hpp - picojet's model viewer (~/picojet/app/main.cpp) as a scene for
// the Jet runtime (jet/runtime): its nine textured models, one at a time,
// changing every six seconds, tumbling at 43 degrees/s about X and 61 about Y
// under picojet's sun and ambient, the camera drifting round at 0.35 rad/s.
// No stick here, so it always drifts. With the GPU drawing, every model fits:
// picojet's memory budget (render queue plus two framebuffers) is gone.
#pragma once
#include "Scene.hpp"
#include "Display.hpp"
#include "Runtime.hpp"
#include <cmath>
#include <cstdio>
#include "assets/mesh_cube.h"
#include "assets/mesh_f117.h"
#include "assets/mesh_f22.h"
#include "assets/mesh_efa.h"
#include "assets/mesh_sphere.h"
#include "assets/mesh_crab.h"
#include "assets/mesh_radio.h"
#include "assets/mesh_column.h"
#include "assets/mesh_biplane.h"
#include "assets/tex_pikuma.h"
#include "assets/tex_f117.h"
#include "assets/tex_f22.h"
#include "assets/tex_efa.h"
#include "assets/tex_crab.h"
#include "assets/tex_radio.h"
#include "assets/tex_column.h"
#include "assets/tex_biplane.h"

namespace Viewer {
using namespace Renderer;

struct Model {
    const char* name;
    const int16_t (*verts)[8];   // x, y, z, u, v, nx, ny, nz (picojet's converter)
    int vertCount;
    const uint16_t (*tris)[3];
    int triCount;
    const uint16_t* texels;      // 128x128 RGB565
};

#define MODEL(n, N, tex, name) \
    { name, mesh_##n##_verts, MESH_##N##_VERTS, mesh_##n##_tris, MESH_##N##_TRIS, tex_##tex##_data }

// picojet's order (app/models.hpp): cube with the pikuma texture, the sphere
// with the crab's
inline const Model models[] = {
    MODEL(cube,    CUBE,    pikuma,  "CUBE"),
    MODEL(f117,    F117,    f117,    "F-117"),
    MODEL(f22,     F22,     f22,     "F-22"),
    MODEL(efa,     EFA,     efa,     "EF-2000"),
    MODEL(sphere,  SPHERE,  crab,    "SPHERE"),
    MODEL(crab,    CRAB,    crab,    "CRAB"),
    MODEL(radio,   RADIO,   radio,   "RADIO"),
    MODEL(column,  COLUMN,  column,  "COLUMN"),
    MODEL(biplane, BIPLANE, biplane, "BIPLANE"),
};
#undef MODEL
constexpr int MODEL_COUNT = int(sizeof models / sizeof models[0]);

inline constexpr int32_t S = JET32_WORLD_SCALE;
inline Scene* scene = nullptr;
inline Camera camera;
inline DirectionalLight* sun = nullptr;
inline AmbientLight* ambient = nullptr;
inline Material material(0xFFFF);
inline Texture texture(1, 1, nullptr, false, 0, false, CLAMP);
inline Object placeholder;
inline Object* loaded = nullptr;
inline int slot = 0, index = -1;
inline float spinX = 0, spinY = 0, angle = 0, shown = 0;
inline char caption[48];

// picojet's build_model: the mesh as Jet vertices, positions in world scale
inline Object* build(const Model& m) {
    auto* o = new Object();
    o->vertices.reserve(m.vertCount);
    o->triangles.reserve(m.triCount);
    for (int i = 0; i < m.vertCount; ++i) {
        const int16_t* v = m.verts[i];
        Object::Vertex vert;
        vert.position = {int32_t(v[0]) * S, int32_t(v[1]) * S, int32_t(v[2]) * S};
        vert.uv = {int32_t(v[3]), int32_t(v[4])};
        vert.normal = {int32_t(v[5]), int32_t(v[6]), int32_t(v[7])};
        o->addVertex(vert);
    }
    for (int i = 0; i < m.triCount; ++i)
        o->addTriangle(m.tris[i][0], m.tris[i][1], m.tris[i][2], &material);
    o->calculateBoundingBox();
    return o;
}

inline void load(int next) {
    scene->getObjects()[slot] = &placeholder;   // never name freed memory
    delete loaded;
    index = next;
    const Model& m = models[index];
    texture.width = 128;
    texture.height = 128;
    texture.data = const_cast<uint16_t*>(m.texels);   // read in place, in flash
    loaded = build(m);
    loaded->setPosition(0, 0, 0);
    scene->getObjects()[slot] = loaded;
    std::snprintf(caption, sizeof caption, "%s  %d TRIANGLES", m.name, m.triCount);
    Esp32Jet::caption = caption;
    std::printf("viewer: %s, %d verts, %d tris\n", m.name, m.vertCount, m.triCount);
}

inline void init(Scene& target) {
    scene = &target;
    scene->setBackcolor(0x632C);                // picojet's 40% grey
    scene->setClearBuffer(true);
    camera.setFOV(int32_t(70), int32_t(Display::RENDER_WIDTH));	// as picojet
    camera.nearPlane = 128;
    camera.farPlane = 4096 * S;
    scene->setCamera(&camera);
    // lit for textured models: the ambient carries the exposure, the sun shapes
    sun = new DirectionalLight(Vector3{45, 35, 0}, Color{255, 250, 240}, 255);
    ambient = new AmbientLight(Color{205, 208, 215});
    scene->setDirectionalLight(sun);
    scene->setAmbientLight(ambient);
    material.shadingMode = ShadingMode::GOURAUD;
    material.specular = 48;                     // headroom: the sun brightens
    material.diffuseMap = &texture;
    placeholder.enabled = false;
    scene->addObject(&placeholder);
    slot = int(scene->getObjects().size()) - 1;
    load(0);
}

inline void update(float dt) {
    if (dt > 0.1f)
        dt = 0.1f;
    spinX = std::fmod(spinX + dt * 43.0f, 360.0f);
    spinY = std::fmod(spinY + dt * 61.0f, 360.0f);
    loaded->setRotation(int32_t(spinX), int32_t(spinY), 0);
    shown += dt;
    if (shown >= 6.0f) {
        shown = 0;
        load((index + 1) % MODEL_COUNT);
    }
    angle += dt * 0.35f;
    const float radius = 760.0f * S;
    camera.setPosition(int32_t(std::cos(angle) * radius), int32_t(210 * S), int32_t(std::sin(angle) * radius));
    camera.lookAt(Vector3{0, 0, 0});
}
}  // namespace Viewer
