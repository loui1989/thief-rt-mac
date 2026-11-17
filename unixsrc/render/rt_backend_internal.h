#pragma once

// The RTGL1 (path traced, Metal) presentation backend. opengl.cpp owns the
// SDL window, input and the RenderBackend entry points; when the ray tracer
// is active it creates a Metal window instead of an OpenGL context and
// forwards every drawing operation here.

#include <stdint.h>
#include <string>
#include <vector>
#include <render_backend.h>

struct SDL_Window;

namespace darkrt {

// The ray tracer was asked for (THIEF_RT=1) and its library exists.
bool Wanted();
// After SDL_CreateWindow(SDL_WINDOW_METAL). False: use OpenGL instead.
bool Open(SDL_Window *window);
void Close();
bool Active();
// Sets the Metal layer's pixel size for the output resolution setting
// (before the drawable size is read).
void ApplyOutputSize();

void *CreateTexture(int width, int height, const void *rgba, int rowBytes);
int UpdateTexture(void *texture, int width, int height, const void *rgba,
                  int rowBytes);
void DestroyTexture(void *texture);
void BindTexture(int level, void *texture);
void SetBlend(int mode);
void SetAlphaTest(int enabled);
void SetFog(int enabled, uint32_t rgb);

// A rect in drawable pixels; (x, y) is the top-left corner.
struct Rect { float x, y, w, h; };

// lgd3d's screen-space draws. During the main view they have no place in the
// path-traced scene and are dropped; afterwards (HUD models, overlays) they
// are composited over it in canvas coordinates.
int Draw(int primitive, const sRenderBackendVertex *vertices, int count,
         int canvasWidth, int canvasHeight, const Rect &viewport);
void BeginSceneFrame();
void DeactivateScene();

// The legacy 2D canvas for this frame, drawn over the scene in viewport.
void SetCanvas(const void *pixels, int width, int height, int rowBytes,
               int depth, const uint8_t *palette, bool scene,
               const Rect &viewport);
// Draws the frame. drawable: the whole window in pixels.
void Present(int drawableWidth, int drawableHeight, bool vsync);
// A frame of the path tracer's is being built (no other may be presented).
bool FrameInProgress();
// The settings panel (Cmd+, or F10). True if the key was used and must not
// reach the game. scancode/mods: SDL's.
bool HandleKey(int scancode, int mods, bool down, bool repeat);
bool MovieFrame(const void *rgba, int width, int height, int rowBytes,
                const Rect &viewport, int drawableWidth, int drawableHeight);

// The OpenGL renderer's bump maps: the material maps the path tracer makes
// (rt/mat_dev) for a texture of the game's, found or made the same way, off
// the main thread. GlMaterialSource: the file of the texture made next
// (RenderRTTextureSource), taken (illum: its glow map's strength).
// GlMaterialStart: nullptr when it gets none. GlMaterialPoll: 0 not yet, 1 its
// normal map (rgba, Y up), -1 none.
std::string GlMaterialSource(float *illum);
void *GlMaterialStart(const std::string &source, const void *rgba, int w, int h, int rowBytes);
int GlMaterialPoll(void *handle, std::vector<uint8_t> &normal, int &w, int &h);
void GlMaterialFree(void *handle);

// The OpenGL renderer's lights of the effects the path tracer lights (fx_lights:
// fire elementals, fireballs, crystals, talismans, flash bombs and mines going
// off...): the nearest first, at most max. rgb: its colour times its strength
// now (the path tracer's units, the game's light's brightness); pos in feet.
// GlEffectsReset: a new world (objects' numbers used again).
struct GlEffectLight {
   int id;
   uint32_t obj;
   float pos[3], rgb[3];
   float size, distance;
   bool moving;
   bool lava;   // (the OpenGL renderer's own: one over the lava)
};
int GlEffectLights(const float cam[3], GlEffectLight *out, int max);
void GlEffectsReset();

} // namespace darkrt
