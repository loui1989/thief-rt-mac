#ifndef __RENDER_RT_H
#define __RENDER_RT_H

// Ray-traced rendering boundary (RTGL1 path tracer, macOS/Metal).
//
// The engine still draws through lgd3d exactly as for the OpenGL renderer.
// When the ray tracer is active the platform renderer additionally receives:
//
//  * the main view's camera, once per rendered scene;
//  * the complete world representation (every cell's polygons, in world
//    space), whenever a mission's geometry changes, plus the current
//    texture for each world texture id every frame;
//  * the static/animated/dynamic object lights, every frame;
//  * object, AI and effect polygons as the engine draws them, with world-space
//    positions recovered from r3d's camera transform.
//
// Everything else lgd3d draws (the HUD, inventory models, menus) is composited
// over the path-traced image as before.  All functions are no-ops unless
// RenderRTActive() returns nonzero.

#include <stdint.h>
#include <render_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

enum eRenderRTWorldFlags {
  kRenderRTWorldSky = 1,          // sky surface: show the sky through it
  kRenderRTWorldWater = 2,        // medium boundary (water surface)
  kRenderRTWorldFullbright = 4,   // RENDER_DOESNT_LIGHT: self-lit
  kRenderRTWorldTransparent = 8   // texture has transparent texels
};

int RenderRTActive(void);

// Main view. Dark world space: z is up. fwd/left/up are the camera axes,
// zoom is r3d's (1.0 = 90 degrees horizontal), width/height the 3D canvas.
void RenderRTSceneBegin(const float pos[3], const float fwd[3],
                        const float left[3], const float up[3], float zoom,
                        int width, int height);
void RenderRTSceneEnd(void);

// While nonzero (the main view is being drawn), lgd3d hands polygons to
// RenderRTDrawWorld instead of rasterizing them.
int RenderRTCaptureDraws(void);
// Nonzero while vertices lgd3d builds belong in the ray-traced scene: the
// main view, outside suppressed regions. Sampled per vertex, because lgd3d
// batches polygons and submits them later.
extern int g_RenderRTCaptureVertices;
// The main view starts suppressed: world surfaces, sky and water come from
// the world representation. The portal renderer lifts it while drawing
// objects. Returns the previous setting, for restoring.
int RenderRTSuppressCapture(int suppress);
// xyz: count world-space positions matching the vertices. Texture, blend and
// alpha-test state are those last set through RenderBackend*.
void RenderRTDrawWorld(int primitive, const sRenderBackendVertex *vertices,
                       const float *xyz, int count);

// World representation. Returns nonzero if geometry for this serial must be
// (re)submitted between RenderRTWorldBegin and RenderRTWorldEnd.
int RenderRTWorldWanted(uint32_t serial);
void RenderRTWorldBegin(uint32_t serial);
void RenderRTWorldPolygon(int textureId, int flags, int count,
                          const float *xyz, const float *uv);
void RenderRTWorldEnd(void);
// Every frame: the backend texture (RenderBackendCreateTexture handle)
// currently showing for a world texture id (animated textures change).
void RenderRTWorldTexture(int textureId, void *texture);

// Lights, every frame between SceneBegin and SceneEnd. rgb is Dark's light
// brightness (0..~255 per channel), radius 0 means unlimited. For a spot
// light dir is set and inner/outer are cosines; inner < 0 for point lights.
void RenderRTLight(uint32_t id, const float pos[3], const float rgb[3],
                   float radius, const float dir[3], float inner, float outer);

#ifdef __cplusplus
}
#endif

#endif
