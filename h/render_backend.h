#ifndef __RENDER_BACKEND_H
#define __RENDER_BACKEND_H

#include <stdint.h>

// Platform-neutral rendering boundary used by the legacy lgd3d facade and
// higher-level engine code.  A platform renderer owns the window/surface and
// implements these operations; the current Windows provider is D3D11.  An
// OpenGL provider can implement the same contract without exposing GL types
// to the engine.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sRenderBackendVertex {
  float x, y, z, rhw;
  float r, g, b, a;
  float fog;
  float u0, v0;
  float u1, v1;
  // the point's place in the world (feet), for the OpenGL renderer's
  // real-time lights; wvalid 1 when known
  float wx, wy, wz, wvalid;
} sRenderBackendVertex;

// A light the OpenGL renderer lights the world with in real time (a lamp
// that flickers or is switched, a torch carried, a fire arrow), its own
// light taken out of the lightmaps -- as NewDark's hardware renderer does.
// In feet. kind 0: a level's light, lit as its lightmap was (lux =
// (0.5 + cos/2) * bright / dist, out to radius, fading past inner_radius);
// kind 1: a moving light, as the game lights the lightmaps with one
// (cos * bright / dist * (1 - dist^2 / radius^2)). color: each channel 0..1,
// its brightness now as a share of bright. spot_inner <= -1: not a spot.
typedef struct sRenderBackendLight {
  float pos[3];
  float color[3];
  float bright;
  float radius, inner_radius;
  float dir[3];
  float spot_inner, spot_outer;
  int kind;
  int id;        // the same light keeps its id (its shadow map kept)
  int moving;    // its shadow drawn anew each frame
  uint32_t owner; // its own object (a torch, a lamp): casts it no shadow
  int flicker;    // a flame's: it flickers and sways a little (its shadows move)
  float size;     // how big (feet): its shadows' softness; 0 a lamp's or a flame's
} sRenderBackendLight;

enum eRenderBackendPrimitive {
  kRenderBackendTriangles = 0,
  kRenderBackendLines = 1,
  kRenderBackendPoints = 2
};

enum eRenderBackendBlend {
  kRenderBackendBlendOpaque = 0,
  kRenderBackendBlendAlpha = 1,
  kRenderBackendBlendMultiply = 2,
  kRenderBackendBlendAdd = 3
};

int RenderBackendAvailable(void);
void RenderBackendSetScaleToWindow(int enabled);
void RenderBackendSetFitToViewport(int enabled);
void RenderBackendSetInteractiveResize(int resizing);
void RenderBackendSetPreserveCanvas(int enabled);
int RenderBackendPreserveCanvas(void);
void RenderBackendSetWindowedClientSizeHint(int width, int height);
// Returns the drawable client size in physical pixels. While an interactive
// resize is in progress the size is intentionally withheld so callers can
// rebuild expensive engine-owned canvases once, when the drag finishes.
int RenderBackendGetSettledClientSize(int *width, int *height);
int RenderBackendClientToLogicalPoint(int *x, int *y);
int RenderBackendLogicalToClientPoint(int *x, int *y);
int RenderBackendBeginFrame(void);
void RenderBackendEndFrame(void);
void RenderBackendDeactivateScene(void);
void RenderBackendClearDepth(void);
void RenderBackendSetDepth(int compareEnabled, int writeEnabled);
void RenderBackendSetBlend(int blendMode);
void RenderBackendSetAlphaTest(int enabled);
void RenderBackendSetSampler(int level, int wrap, int smooth);
void RenderBackendSetFog(int enabled, uint32_t rgb);

void *RenderBackendCreateTexture(int width, int height, const void *rgba,
                                 int rowBytes);
int RenderBackendUpdateTexture(void *texture, int width, int height,
                               const void *rgba, int rowBytes);
void RenderBackendDestroyTexture(void *texture);
void RenderBackendBindTexture(int level, void *texture);

int RenderBackendDraw(int primitive, const sRenderBackendVertex *vertices,
                      int vertexCount, int useSecondTexture);

// The OpenGL renderer's real-time lights: how many at most (0: off, the
// setting, or no OpenGL 4 or the path tracer running). While nonzero lgd3d
// gives vertices their world positions (g_RenderGLWorldPositions).
int RenderBackendDynamicLightsWanted(void);
extern int g_RenderGLWorldPositions;
// The level's solid surfaces, for the lights' shadows: triangles (xyz, feet),
// in runs by cell: cell c's are cellFirst[c] .. cellFirst[c + 1] - 1, inside
// the sphere cellSphere[c * 4 ..] (centre, radius). Once a mission.
void RenderBackendSetWorldShadowMesh(const float *xyz, int triangles,
                                     const int *cellFirst, const float *cellSphere,
                                     int cells);
// The level's lava, for its glow on the walls round it: one light over each
// square of it (13 feet, 4 m, a side), place (feet) and its area there
// (square feet) -- x, y, z, area each. Once a mission, after the mesh.
void RenderBackendSetLavaLights(const float *xyza, int count);
// This frame's real-time lights (before the view is drawn), and where the
// camera is (feet).
void RenderBackendSetDynamicLights(const sRenderBackendLight *lights, int count,
                                   const float camera[3]);
// The view drawn: the models it drew (their world triangles, solid ones)
// cast the lights' shadows from the next frame.
void RenderBackendDynamicViewEnd(void);
// The models that cast the lights' shadows, drawn for them alone (nothing
// rasterized) and kept while they stay where they are: between
// RenderBackendCaptureObjects(1) and (0), each between ObjectBegin and End.
// pose: its place and facing (x y z, three angles). ObjectCached: already
// kept, at this pose.
void RenderBackendCaptureObjects(int on);
void RenderBackendObjectBegin(uint32_t obj, const float pose[6]);
void RenderBackendObjectEnd(void);
int RenderBackendObjectCached(uint32_t obj, const float pose[6]);

// Optional diagnostics for automated render checks. Set DARK_RENDER_TRACE to
// opt in to the plain-text trace; normal game and viewer runs create no log.
void RenderBackendTraceReset(void);
void RenderBackendTrace(const char *format, ...);
int RenderBackendCaptureComplete(void);

#ifdef __cplusplus
}
#endif

#endif
