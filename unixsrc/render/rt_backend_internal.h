#pragma once

// The RTGL1 (path traced, Metal) presentation backend. opengl.cpp owns the
// SDL window, input and the RenderBackend entry points; when the ray tracer
// is active it creates a Metal window instead of an OpenGL context and
// forwards every drawing operation here.

#include <stdint.h>
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

} // namespace darkrt
