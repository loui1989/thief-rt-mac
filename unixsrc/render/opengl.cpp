#define GL_GLEXT_PROTOTYPES
#pragma pack(push, 8)
#include <SDL2/SDL.h>
#ifdef DARK_RT
#include <SDL2/SDL_metal.h>
#endif
#ifdef DARK_OPENGL_ES
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
// macOS ships a 4.1 core profile. Every function used here is a core entry
// point exported directly by OpenGL.framework.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <cmath>
#include <thread>
#include <string>
#include <platform_services.h>
#pragma pack(pop)

#include <kb.h>
#include <mouse.h>
#include <render_backend.h>

#include "app_icon_data.h"
#include "runtime.h"
#ifdef DARK_RT
#include "rt_backend_internal.h"
#endif
#include "rt_font.h"

extern "C" void DarkInputKey(uchar scan, int down);
extern "C" void DarkInputMouse(short x, short y, uchar type, uchar buttons, char wheel);
extern "C" void DarkInputMouseRelative(int x, int y, uchar buttons);

namespace { void load_finish(); void load_progress(const char *stage); void set_load_thread(); }
static void present_legacy_frame(const void *pixels, int width, int height,
   int rowBytes, int depth, const uint8_t *palette);

namespace {
struct Texture {
   GLuint name{};
   int width{}, height{};
   int wrap{-1}, smooth{-1};
};
SDL_Window *g_window;
SDL_GLContext g_context;
// The RTGL1 path tracer presents instead of OpenGL (rt_backend_internal.h).
bool g_rt;
SDL_Cursor *g_invisibleCursor;
int g_width, g_height, g_clientWidth, g_clientHeight;
bool g_scene, g_scale = true, g_fit = true, g_preserve, g_quit;
bool g_resizePending;
bool g_fog, g_alphaTest;
bool g_depthCompare, g_depthWrite;
int g_blendMode = kRenderBackendBlendOpaque;
uint32_t g_fogColor;
void *g_textures[2];
GLuint g_overlayTexture;
int g_overlayWidth, g_overlayHeight;
int g_overlayStorage;
std::vector<uint32_t> g_overlay;
FILE *g_trace;
uchar g_buttons;
unsigned g_sceneGeneration, g_presentedGeneration, g_hardwareFrameCount;
Uint64 g_lastPresentTicks;
bool g_captureComplete;
bool g_relativeMouse, g_relativeActive, g_focused;
bool g_keysDown[SDL_NUM_SCANCODES];   // keys the game was told are down
bool g_wslPointerWorkaround;
bool g_pointerWarpWarningPrinted;
int g_swapInterval = -1;
bool g_swapIntervalApplied;
unsigned g_unsupportedSwapIntervals;
unsigned g_applicationFrameDepth;
bool g_presentPending;
const void *g_deferredPixels;
// the canvas last given to the screen (the loading bar is drawn over it)
const void *g_lastPixels;
const uint8_t *g_lastPalette;
int g_lastWidth, g_lastHeight, g_lastRowBytes, g_lastDepth;
const uint8_t *g_deferredPalette;
int g_deferredWidth, g_deferredHeight, g_deferredRowBytes, g_deferredDepth;
bool g_samplerWrap[2];
bool g_samplerSmooth[2] = {true, true};
GLuint g_sceneProgram, g_sceneBuffer, g_sceneVertexArray;
GLint g_viewportUniform, g_useTexture1Uniform, g_fogEnabledUniform;
GLint g_minLightUniform = -1;
// The least light a lit-map surface gets (gl_settings.txt: min_light, 0 to 0.5):
// the corners no lamp reaches show a hint of their texture, not black. 0 is
// the original look.
float g_minLight = 0.06f;
extern "C" int g_PlayerShadowAlert;   // (src/render/objlight.c: the guards seeing the player's shadow)
bool g_glFps = false;   // the OpenGL panel's frame counter
void load_gl_settings()
{
   const char *home = getenv("HOME");
   std::string path = home && *home ?
#if defined(__APPLE__)
      std::string(home) + "/Library/Application Support/Dark Engine Restored/gl_settings.txt"
#else
      std::string(home) + "/.config/dark-engine-restored/gl_settings.txt"
#endif
      : std::string("gl_settings.txt");
   if (FILE *f = fopen(path.c_str(), "r")) {
      char key[64];
      float value;
      while (fscanf(f, "%63s %f", key, &value) == 2)
         if (!strcmp(key, "min_light"))
            g_minLight = std::clamp(value, 0.0f, 0.5f);
         else if (!strcmp(key, "shadow_alert"))
            g_PlayerShadowAlert = value != 0.0f ? 1 : 0;
         else if (!strcmp(key, "fps"))
            g_glFps = value != 0.0f;
      fclose(f);
   } else if (FILE *w = fopen(path.c_str(), "w")) {
      // (written once with the default, to be edited)
      fprintf(w, "min_light %.2f\n", g_minLight);
      fclose(w);
   }
   if (const char *env = getenv("THIEF_GL_MIN_LIGHT"))
      g_minLight = std::clamp(float(atof(env)), 0.0f, 0.5f);
   fprintf(stderr, "OpenGL: unlit surfaces lifted to %.0f%% of full light (gl_settings.txt min_light)\n",
           g_minLight * 100.0f);
}
GLint g_fogColorUniform, g_alphaTestUniform, g_transparentBlackUniform;

void window_drawable_size(int &w, int &h);
void viewport(int &x, int &y, int &w, int &h);

// Window coordinates are in points; on HiDPI displays (macOS Retina, scaled
// Wayland) the GL drawable has more pixels than the window has points.
void drawable_scale(float &sx, float &sy)
{
   sx = sy = 1.0f;
   if (!g_window || g_clientWidth <= 0 || g_clientHeight <= 0) return;
   int dw = 0, dh = 0;
   window_drawable_size(dw, dh);
   if (dw > 0 && dh > 0) {
      sx = static_cast<float>(dw) / g_clientWidth;
      sy = static_cast<float>(dh) / g_clientHeight;
   }
}

void window_drawable_size(int &w, int &h)
{
   w = g_clientWidth; h = g_clientHeight;
   if (!g_window) return;
#ifdef DARK_RT
   if (g_rt) { darkrt::ApplyOutputSize(); SDL_Metal_GetDrawableSize(g_window, &w, &h); return; }
#endif
   SDL_GL_GetDrawableSize(g_window, &w, &h);
}

#ifdef DARK_RT
// viewport() in drawable pixels, for the ray tracer
darkrt::Rect rt_viewport_rect()
{
   int x, y, w, h; viewport(x, y, w, h);
   int dw, dh; window_drawable_size(dw, dh);
   const float sx = g_clientWidth > 0 ? float(dw) / g_clientWidth : 1.0f;
   const float sy = g_clientHeight > 0 ? float(dh) / g_clientHeight : 1.0f;
   return darkrt::Rect{x * sx, y * sy, w * sx, h * sy};
}
#endif

void set_gl_viewport(int x, int y, int w, int h)
{
   float sx, sy; drawable_scale(sx, sy);
   glViewport(static_cast<GLint>(x * sx + 0.5f), static_cast<GLint>(y * sy + 0.5f),
      static_cast<GLsizei>(w * sx + 0.5f), static_cast<GLsizei>(h * sy + 0.5f));
}

void set_window_icon()
{
   std::array<Uint8, dark_app_icon::kWidth * dark_app_icon::kHeight * 4> pixels;
   for (size_t i = 0; i < dark_app_icon::kWidth * dark_app_icon::kHeight; ++i) {
      const Uint8 *color = dark_app_icon::kPalette[dark_app_icon::kPixels[i]];
      pixels[i * 4] = color[0];
      pixels[i * 4 + 1] = color[1];
      pixels[i * 4 + 2] = color[2];
      pixels[i * 4 + 3] = color[3];
   }

   SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),
      dark_app_icon::kWidth, dark_app_icon::kHeight, 32,
      dark_app_icon::kWidth * 4, SDL_PIXELFORMAT_RGBA32);
   if (icon) {
      SDL_SetWindowIcon(g_window, icon);
      SDL_FreeSurface(icon);
   }
}

#ifdef DARK_OPENGL_ES
const char kShaderPreamble[] =
   "#version 300 es\n"
   "precision highp float;\n";
#else
const char kShaderPreamble[] = "#version 330 core\n";
#endif

const char kSceneVertexShader[] =
   "layout(location=0) in vec4 position;\n"
   "layout(location=1) in vec4 color;\n"
   "layout(location=2) in float fog;\n"
   "layout(location=3) in vec2 texcoord0;\n"
   "layout(location=4) in vec2 texcoord1;\n"
   "uniform vec2 viewportSize;\n"
   "out vec4 linearColor;\n"
   "out float linearFog;\n"
   "out float linearWeight;\n"
   "out vec2 uv0;\n"
   "out vec2 uv1;\n"
   "void main(){\n"
   " float q=1.0/max(position.w,0.000001);\n"
   " gl_Position=vec4((position.x/viewportSize.x*2.0-1.0)*q,"
      "(1.0-position.y/viewportSize.y*2.0)*q,"
      "(position.z*2.0-1.0)*q,q);\n"
   " linearColor=color*q;linearFog=fog*q;linearWeight=q;\n"
   " uv0=texcoord0;uv1=texcoord1;gl_PointSize=1.0;\n"
   "}\n";

const char kSceneFragmentShader[] =
   "in vec4 linearColor;\n"
   "in float linearFog;\n"
   "in float linearWeight;\n"
   "in vec2 uv0;\n"
   "in vec2 uv1;\n"
   "uniform sampler2D texture0;\n"
   "uniform sampler2D texture1;\n"
   "uniform float useTexture1;\n"
   "uniform float minLight;\n"
   "uniform float fogEnabled;\n"
   "uniform vec3 fogColor;\n"
   "uniform float alphaTest;\n"
   "uniform float transparentBlack;\n"
   "out vec4 fragmentColor;\n"
   "void main(){\n"
   " float weight=max(linearWeight,0.000001);\n"
   " vec4 texel=texture(texture0,uv0);\n"
   " if(transparentBlack>0.5&&all(equal(texel.rgb,vec3(0.0))))texel.a=0.0;\n"
   " vec4 color=(linearColor/weight)*texel;\n"
   " if(useTexture1>0.5){vec4 light=texture(texture1,uv1);"
      "light.rgb+=minLight*(vec3(1.0)-light.rgb);color*=light;}\n"
   " if(alphaTest>0.5&&texel.a<0.5)discard;\n"
   " if(fogEnabled>0.5)color.rgb=mix(fogColor,color.rgb,"
      "clamp(linearFog/weight,0.0,1.0));\n"
   " fragmentColor=color;\n"
   "}\n";

GLuint compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   const GLchar *parts[] = {kShaderPreamble, source};
   glShaderSource(shader, 2, parts, nullptr);
   glCompileShader(shader);
   GLint compiled = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   if (compiled == GL_TRUE)
      return shader;
   GLint length = 0;
   glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
   std::vector<GLchar> log(static_cast<size_t>(std::max(length, 1)));
   glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr,
      log.data());
   fprintf(stderr, "OpenGL shader compilation failed: %s\n", log.data());
   glDeleteShader(shader);
   return 0;
}

bool create_scene_pipeline()
{
   while (glGetError() != GL_NO_ERROR) {}
   GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, kSceneVertexShader);
   GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, kSceneFragmentShader);
   if (!vertexShader || !fragmentShader) {
      if (vertexShader) glDeleteShader(vertexShader);
      if (fragmentShader) glDeleteShader(fragmentShader);
      return false;
   }
   g_sceneProgram = glCreateProgram();
   glAttachShader(g_sceneProgram, vertexShader);
   glAttachShader(g_sceneProgram, fragmentShader);
   glLinkProgram(g_sceneProgram);
   glDeleteShader(vertexShader);
   glDeleteShader(fragmentShader);
   GLint linked = GL_FALSE;
   glGetProgramiv(g_sceneProgram, GL_LINK_STATUS, &linked);
   if (linked != GL_TRUE) {
      GLint length = 0;
      glGetProgramiv(g_sceneProgram, GL_INFO_LOG_LENGTH, &length);
      std::vector<GLchar> log(static_cast<size_t>(std::max(length, 1)));
      glGetProgramInfoLog(g_sceneProgram, static_cast<GLsizei>(log.size()),
         nullptr, log.data());
      fprintf(stderr, "OpenGL shader link failed: %s\n", log.data());
      glDeleteProgram(g_sceneProgram);
      g_sceneProgram = 0;
      return false;
   }

   glUseProgram(g_sceneProgram);
   glUniform1i(glGetUniformLocation(g_sceneProgram, "texture0"), 0);
   glUniform1i(glGetUniformLocation(g_sceneProgram, "texture1"), 1);
   g_viewportUniform = glGetUniformLocation(g_sceneProgram, "viewportSize");
   g_useTexture1Uniform = glGetUniformLocation(g_sceneProgram, "useTexture1");
   g_minLightUniform = glGetUniformLocation(g_sceneProgram, "minLight");
   load_gl_settings();
   g_fogEnabledUniform = glGetUniformLocation(g_sceneProgram, "fogEnabled");
   g_fogColorUniform = glGetUniformLocation(g_sceneProgram, "fogColor");
   g_alphaTestUniform = glGetUniformLocation(g_sceneProgram, "alphaTest");
   g_transparentBlackUniform = glGetUniformLocation(g_sceneProgram,
      "transparentBlack");

   glGenVertexArrays(1, &g_sceneVertexArray);
   glBindVertexArray(g_sceneVertexArray);
   glGenBuffers(1, &g_sceneBuffer);
   glBindBuffer(GL_ARRAY_BUFFER, g_sceneBuffer);
   const GLsizei stride = sizeof(sRenderBackendVertex);
   const size_t offsets[] = {
      offsetof(sRenderBackendVertex, x), offsetof(sRenderBackendVertex, r),
      offsetof(sRenderBackendVertex, fog), offsetof(sRenderBackendVertex, u0),
      offsetof(sRenderBackendVertex, u1)};
   const GLint sizes[] = {4, 4, 1, 2, 2};
   for (GLuint i = 0; i < 5; ++i) {
      glEnableVertexAttribArray(i);
      glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, stride,
         reinterpret_cast<const void *>(offsets[i]));
   }
   return g_sceneProgram && g_sceneBuffer && g_sceneVertexArray &&
      glGetError() == GL_NO_ERROR;
}

void destroy_scene_pipeline()
{
   if (g_sceneBuffer) glDeleteBuffers(1, &g_sceneBuffer);
   if (g_sceneVertexArray) glDeleteVertexArrays(1, &g_sceneVertexArray);
   if (g_sceneProgram) glDeleteProgram(g_sceneProgram);
   g_sceneBuffer = g_sceneVertexArray = g_sceneProgram = 0;
}

void viewport(int &x, int &y, int &w, int &h)
{
   x = y = 0; w = g_clientWidth; h = g_clientHeight;
   if (!g_scale) {
      w = std::min(w, g_width);
      h = std::min(h, g_height);
      return;
   }
   // Fit gameplay owns the settled client-sized canvas and fills the output.
   // Fixed gameplay and 2D panels retain their logical aspect ratio.
   if (!(g_fit && g_scene) && g_width > 0 && g_height > 0 &&
       w * g_height != h * g_width) {
      if (w * g_height > h * g_width) { int nw = h * g_width / g_height; x = (w - nw) / 2; w = nw; }
      else { int nh = w * g_height / g_width; y = (h - nh) / 2; h = nh; }
   }
}

uchar key_scan(SDL_Scancode key)
{
   if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F10)
      return static_cast<uchar>(0x3b + key - SDL_SCANCODE_F1);
   switch (key) {
   case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
   case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06;
   case SDL_SCANCODE_6: return 0x07; case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
   case SDL_SCANCODE_9: return 0x0a; case SDL_SCANCODE_0: return 0x0b; case SDL_SCANCODE_MINUS: return 0x0c;
   case SDL_SCANCODE_EQUALS: return 0x0d; case SDL_SCANCODE_BACKSPACE: return 0x0e; case SDL_SCANCODE_TAB: return 0x0f;
   case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
   case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
   case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
   case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1a; case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
   case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1c;
   case SDL_SCANCODE_LCTRL: return KBC_LCTRL; case SDL_SCANCODE_RCTRL: return KBC_RCTRL;
   case SDL_SCANCODE_A: return 0x1e; case SDL_SCANCODE_S: return 0x1f; case SDL_SCANCODE_D: return 0x20;
   case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
   case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
   case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
   case SDL_SCANCODE_LSHIFT: return KBC_LSHIFT; case SDL_SCANCODE_BACKSLASH: return 0x2b; case SDL_SCANCODE_Z: return 0x2c;
   case SDL_SCANCODE_X: return 0x2d; case SDL_SCANCODE_C: return 0x2e; case SDL_SCANCODE_V: return 0x2f;
   case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31; case SDL_SCANCODE_M: return 0x32;
   case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35;
   case SDL_SCANCODE_RSHIFT: return KBC_RSHIFT; case SDL_SCANCODE_LALT: return KBC_LALT; case SDL_SCANCODE_RALT: return KBC_RALT;
   case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return KBC_CAPS;
   case SDL_SCANCODE_NUMLOCKCLEAR: return KBC_NUM; case SDL_SCANCODE_SCROLLLOCK: return KBC_SCROLL;
   case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
   case SDL_SCANCODE_HOME: return 0xc7; case SDL_SCANCODE_UP: return 0xc8; case SDL_SCANCODE_PAGEUP: return 0xc9;
   case SDL_SCANCODE_LEFT: return 0xcb; case SDL_SCANCODE_RIGHT: return 0xcd; case SDL_SCANCODE_END: return 0xcf;
   case SDL_SCANCODE_DOWN: return 0xd0; case SDL_SCANCODE_PAGEDOWN: return 0xd1; case SDL_SCANCODE_INSERT: return 0xd2;
   case SDL_SCANCODE_DELETE: return 0xd3; default: return KBC_NONE;
   }
}

void logical_point(int &x, int &y)
{
   int vx, vy, vw, vh; viewport(vx, vy, vw, vh);
   x = vw ? (x - vx) * g_width / vw : 0;
   y = vh ? (y - vy) * g_height / vh : 0;
   x = std::clamp(x, 0, std::max(g_width - 1, 0));
   y = std::clamp(y, 0, std::max(g_height - 1, 0));
}

void set_swap_interval(int interval)
{
   if (g_rt) { g_swapInterval = interval; return; }
   if (!g_context || g_swapInterval == interval)
      return;
   // Remember the request even when the compositor cannot provide it.  Some
   // WSLg/X11 configurations reject vsync, and retrying once per rendered
   // frame only floods stderr without changing the result.
   g_swapInterval = interval;
   const unsigned unsupportedBit = interval == 0 ? 1u : interval == 1 ? 2u : 4u;
   if (g_unsupportedSwapIntervals & unsupportedBit) {
      g_swapIntervalApplied = false;
      return;
   }
   if (SDL_GL_SetSwapInterval(interval) != 0) {
      g_unsupportedSwapIntervals |= unsupportedBit;
      g_swapIntervalApplied = false;
      fprintf(stderr, "SDL swap interval %d unavailable: %s\n", interval,
         SDL_GetError());
      return;
   }
   g_swapIntervalApplied = true;
}

void update_relative_mouse()
{
   const bool active = g_relativeMouse && g_focused;
   g_relativeActive = SDL_GetRelativeMouseMode() == SDL_TRUE;
   const bool changed = active != g_relativeActive;
   if (changed) {
      if (SDL_SetRelativeMouseMode(active ? SDL_TRUE : SDL_FALSE) != 0) {
         SDL_SetWindowMouseGrab(g_window, SDL_FALSE);
         fprintf(stderr, "SDL relative pointer lock failed: %s\n", SDL_GetError());
         return;
      }
   }
   g_relativeActive = SDL_GetRelativeMouseMode() == SDL_TRUE;
   if (g_relativeActive != active) {
      fprintf(stderr, "SDL relative pointer lock did not enter the requested state\n");
      return;
   }

   // Relative mode supplies the motion deltas, but WSLg/XWayland can leave
   // its host cursor visible and able to cross the client edge unless the
   // mouse grab and confinement rectangle are also asserted explicitly.
   // Reassert these even when relative mode itself stayed active: focus and
   // compositor transitions can independently drop the grab or cursor state.
   if (SDL_GetWindowMouseGrab(g_window) != (active ? SDL_TRUE : SDL_FALSE))
      SDL_SetWindowMouseGrab(g_window, active ? SDL_TRUE : SDL_FALSE);
   // SDL relative mode owns confinement on native Wayland/X11. Capture and
   // an explicit confinement rectangle are only needed for WSLg's XWayland
   // bridge; applying them on native compositors can suppress relative motion
   // after a menu-to-game transition even though the pointer remains locked.
   if (g_wslPointerWorkaround) {
      SDL_CaptureMouse(active ? SDL_TRUE : SDL_FALSE);
      if (active) {
         SDL_Rect client = {0, 0, std::max(g_clientWidth, 1),
                            std::max(g_clientHeight, 1)};
         const SDL_Rect *current = SDL_GetWindowMouseRect(g_window);
         if (!current || current->x != client.x || current->y != client.y ||
             current->w != client.w || current->h != client.h)
            SDL_SetWindowMouseRect(g_window, &client);
      } else if (SDL_GetWindowMouseRect(g_window)) {
         SDL_SetWindowMouseRect(g_window, nullptr);
      }
   }
   if (g_invisibleCursor && SDL_GetCursor() != g_invisibleCursor)
      SDL_SetCursor(g_invisibleCursor);
   if (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE)
      SDL_ShowCursor(SDL_DISABLE);

   RenderBackendTrace("mouse-lock requested=%d active=%d focused=%d grabbed=%d",
      g_relativeMouse, g_relativeActive, g_focused,
      SDL_GetWindowMouseGrab(g_window) == SDL_TRUE);
   // Discard any delta accumulated while the compositor was changing pointer
   // mode. Gameplay consumes motion accumulated after the lock transition.
   if (changed) {
      int ignoredX, ignoredY;
      SDL_GetRelativeMouseState(&ignoredX, &ignoredY);
   }
}

// ---------------------------------------------------------------------------
// The OpenGL renderer's settings panel (F10 or Cmd+, in game, as the ray
// tracer's): what applies without the ray tracer. Kept in gl_settings.txt.
extern "C" void SetSimTimePassing(int passing);
extern "C" int IsSimTimePassing(void);
void bind_texture(int level, Texture *texture);
bool draw_vertices(GLenum primitive, const sRenderBackendVertex *vertices, int count, bool second,
                   int width, int height, bool fog, uint32_t fogColor, bool alphaTest,
                   bool transparentBlack);
namespace glpanel {
bool g_open = false;
int g_row = 0;
enum { kShadow, kMinLight, kFps, kRows };
Texture *g_font = nullptr, *g_white = nullptr;

std::string settings_path()
{
   const char *home = getenv("HOME");
   return home && *home ?
#if defined(__APPLE__)
      std::string(home) + "/Library/Application Support/Dark Engine Restored/gl_settings.txt"
#else
      std::string(home) + "/.config/dark-engine-restored/gl_settings.txt"
#endif
      : std::string("gl_settings.txt");
}

void save()
{
   if (FILE *w = fopen(settings_path().c_str(), "w")) {
      fprintf(w, "min_light %.2f\nshadow_alert %d\nfps %d\n", g_minLight, g_PlayerShadowAlert, g_glFps ? 1 : 0);
      fclose(w);
   }
}

const char *label(int row)
{
   switch (row) {
   case kShadow: return "Guards notice your shadow";
   case kMinLight: return "Unlit corners lifted to";
   default: return "FPS counter";
   }
}

std::string value(int row)
{
   char b[64];
   switch (row) {
   case kShadow: return g_PlayerShadowAlert ? "On" : "Off (as the game)";
   case kMinLight:
      if (g_minLight <= 0.0f) return "Off (black, as the game)";
      snprintf(b, sizeof(b), "%.0f%% of full light", g_minLight * 100.0f);
      return b;
   default: return g_glFps ? "On" : "Off";
   }
}

void change(int row, int dir)
{
   switch (row) {
   case kShadow: g_PlayerShadowAlert = !g_PlayerShadowAlert; break;
   case kMinLight: g_minLight = std::clamp(std::round((g_minLight + 0.02f * dir) * 50.0f) / 50.0f, 0.0f, 0.5f); break;
   default: g_glFps = !g_glFps; break;
   }
   save();
}

void pause(bool open)
{
   static bool s_paused = false;
   static int s_was = 1;
   if (open && !s_paused && g_scene) {
      s_was = IsSimTimePassing();
      SetSimTimePassing(0);
      s_paused = true;
   } else if (!open && s_paused) {
      SetSimTimePassing(s_was);
      s_paused = false;
   }
}

// true: the key was the panel's
bool key(int scancode, int mods, bool down, bool repeat)
{
   const bool toggle = scancode == SDL_SCANCODE_F10 || (scancode == SDL_SCANCODE_COMMA && (mods & KMOD_GUI));
   if (toggle) {
      if (down && !repeat) {
         g_open = !g_open;
         pause(g_open);
      }
      return true;
   }
   if (!g_open || !down)
      return false;
   switch (scancode) {
   case SDL_SCANCODE_ESCAPE:
      g_open = false;
      pause(false);
      break;
   case SDL_SCANCODE_UP: g_row = (g_row + kRows - 1) % kRows; break;
   case SDL_SCANCODE_DOWN: g_row = (g_row + 1) % kRows; break;
   case SDL_SCANCODE_LEFT: change(g_row, -1); break;
   case SDL_SCANCODE_RIGHT:
   case SDL_SCANCODE_RETURN:
   case SDL_SCANCODE_SPACE: change(g_row, 1); break;
   default: break;
   }
   return true;
}

void make_textures()
{
   if (g_font)
      return;
   std::vector<uint8_t> rgba(size_t(rtfont::kWidth) * rtfont::kHeight * 4);
   for (int i = 0; i < rtfont::kWidth * rtfont::kHeight; ++i) {
      const uint8_t b = rtfont::kAlpha4[i / 2];
      const uint8_t a4 = (i & 1) ? (b >> 4) : (b & 15);
      rgba[size_t(i) * 4 + 0] = rgba[size_t(i) * 4 + 1] = rgba[size_t(i) * 4 + 2] = 255;
      rgba[size_t(i) * 4 + 3] = uint8_t(a4 * 17);
   }
   const uint8_t white[4] = {255, 255, 255, 255};
   auto make = [](int w, int h, const void *px) {
      Texture *t = new Texture;
      glGenTextures(1, &t->name);
      t->width = w, t->height = h;
      glBindTexture(GL_TEXTURE_2D, t->name);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      t->wrap = 0, t->smooth = 1;
      return t;
   };
   g_font = make(rtfont::kWidth, rtfont::kHeight, rgba.data());
   g_white = make(1, 1, white);
}

struct Batch {
   std::vector<sRenderBackendVertex> v;
   void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float c[4])
   {
      const sRenderBackendVertex q[4] = {
         {x0, y0, 0, 1, c[0], c[1], c[2], c[3], 1, u0, v0, 0, 0},
         {x1, y0, 0, 1, c[0], c[1], c[2], c[3], 1, u1, v0, 0, 0},
         {x1, y1, 0, 1, c[0], c[1], c[2], c[3], 1, u1, v1, 0, 0},
         {x0, y1, 0, 1, c[0], c[1], c[2], c[3], 1, u0, v1, 0, 0}};
      for (int i : {0, 1, 2, 0, 2, 3})
         v.push_back(q[i]);
   }
};

// text at (x, y) (its top left), cells s pixels of the font's
float text(Batch &b, float x, float y, float s, const char *str, const float c[4])
{
   for (; *str; ++str) {
      int ch = (unsigned char)*str;
      if (ch < 32 || ch > 127)
         ch = '?';
      const int i = ch - 32, cx = i % rtfont::kCols, cy = i / rtfont::kCols;
      const float u0 = float(cx * rtfont::kCellW) / rtfont::kWidth, v0 = float(cy * rtfont::kCellH) / rtfont::kHeight;
      const float u1 = float((cx + 1) * rtfont::kCellW) / rtfont::kWidth,
                  v1 = float((cy + 1) * rtfont::kCellH) / rtfont::kHeight;
      if (ch != ' ')
         b.quad(x, y, x + rtfont::kCellW * s, y + rtfont::kCellH * s, u0, v0, u1, v1, c);
      x += rtfont::kCellW * s;
   }
   return x;
}

void draw()
{
   static Uint64 s_last = 0;
   static float s_ms = 16.0f;
   const Uint64 now = SDL_GetPerformanceCounter();
   if (s_last)
      s_ms += (float(double(now - s_last) * 1000.0 / double(SDL_GetPerformanceFrequency())) - s_ms) * 0.1f;
   s_last = now;
   if (!g_open && !g_glFps)
      return;
   make_textures();
   int dw, dh;
   window_drawable_size(dw, dh);
   if (dw <= 0 || dh <= 0)
      return;
   glViewport(0, 0, dw, dh);
   glDisable(GL_DEPTH_TEST);
   glDepthMask(GL_FALSE);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   const float s = std::max(1.0f, float(dh) / 1100.0f);
   const float white[4] = {1, 1, 1, 1}, dim[4] = {0.75f, 0.75f, 0.75f, 1}, pick[4] = {1.0f, 0.85f, 0.45f, 1},
               shade[4] = {0, 0, 0, 0.72f};
   Batch bg, fg;
   if (g_glFps) {
      char b[64];
      snprintf(b, sizeof(b), "%3.0f fps  %5.1f ms", 1000.0f / std::max(s_ms, 0.1f), s_ms);
      bg.quad(8 * s, 8 * s, 8 * s + (strlen(b) + 1) * rtfont::kCellW * s, 8 * s + rtfont::kCellH * s * 1.2f, 0, 0, 1, 1, shade);
      text(fg, 14 * s, 10 * s, s, b, white);
   }
   if (g_open) {
      const float lineH = rtfont::kCellH * s * 1.3f, w = 64 * rtfont::kCellW * s;
      const float h = lineH * (kRows + 4);
      const float x0 = (dw - w) * 0.5f, y0 = (dh - h) * 0.5f;
      bg.quad(x0, y0, x0 + w, y0 + h, 0, 0, 1, 1, shade);
      float y = y0 + lineH * 0.5f;
      text(fg, x0 + 20 * s, y, s, "Settings (OpenGL)", white);
      y += lineH * 1.5f;
      for (int r = 0; r < kRows; ++r, y += lineH) {
         const float *c = r == g_row ? pick : dim;
         text(fg, x0 + 20 * s, y, s, r == g_row ? ">" : " ", c);
         text(fg, x0 + 40 * s, y, s, label(r), c);
         text(fg, x0 + w * 0.5f, y, s, value(r).c_str(), c);
      }
      text(fg, x0 + 20 * s, y + lineH * 0.5f, s, "Up/Down: choose   Left/Right: change   F10: close", dim);
   }
   const bool wrap = g_samplerWrap[0], smooth = g_samplerSmooth[0];
   g_samplerWrap[0] = false, g_samplerSmooth[0] = true;
   if (!bg.v.empty()) {
      bind_texture(0, g_white);
      draw_vertices(GL_TRIANGLES, bg.v.data(), int(bg.v.size()), false, dw, dh, false, 0, false, false);
   }
   if (!fg.v.empty()) {
      bind_texture(0, g_font);
      draw_vertices(GL_TRIANGLES, fg.v.data(), int(fg.v.size()), false, dw, dh, false, 0, false, false);
   }
   g_samplerWrap[0] = wrap, g_samplerSmooth[0] = smooth;
}
} // namespace glpanel

void swap_presented_frame()
{
#ifdef DARK_RT
   if (g_rt) {
      if (!g_presentPending) return;
      int dw, dh; window_drawable_size(dw, dh);
      darkrt::Present(dw, dh, g_swapInterval == 1);
      g_presentPending = false;
      return;
   }
#endif
   if (!g_context || !g_presentPending)
      return;
   // Automated capture only needs the completed back buffer.  Some headless
   // compositors (notably WSLg during CI-style runs) can block indefinitely
   // in the window-system swap before the editor reaches its first hardware
   // frame.  Keep ordinary interactive presentation unchanged and make the
   // no-swap behavior an explicit test-only opt-in.
   glpanel::draw();
   const char *headlessCapture = getenv("DARK_RENDER_CAPTURE_HEADLESS");
   if (headlessCapture && atoi(headlessCapture) != 0)
      glFlush();
   else
      SDL_GL_SwapWindow(g_window);

   // A working swap interval already paces gameplay. The old unconditional
   // post-swap delay occasionally added an extra millisecond (or a full
   // refresh on some compositors), producing visibly uneven camera motion.
   if (g_swapInterval != 1 || !g_swapIntervalApplied) {
      const Uint64 now = SDL_GetTicks64();
      if (g_lastPresentTicks && now - g_lastPresentTicks < 16)
         SDL_Delay(static_cast<Uint32>(16 - (now - g_lastPresentTicks)));
      g_lastPresentTicks = SDL_GetTicks64();
   } else {
      g_lastPresentTicks = 0;
   }
   g_presentPending = false;
}

void bind_texture(int level, Texture *texture)
{
   glActiveTexture(GL_TEXTURE0 + level);
   if (texture) {
      glBindTexture(GL_TEXTURE_2D, texture->name);
      if (texture->wrap != g_samplerWrap[level]) {
         texture->wrap = g_samplerWrap[level];
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
            texture->wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
            texture->wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
      }
      if (texture->smooth != g_samplerSmooth[level]) {
         texture->smooth = g_samplerSmooth[level];
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            texture->smooth ? GL_LINEAR : GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            texture->smooth ? GL_LINEAR : GL_NEAREST);
      }
   }
   else glBindTexture(GL_TEXTURE_2D, 0);
}

void apply_depth_state()
{
   // Dark uses compare-off/write-on while laying down terrain: that means
   // "always pass and write depth", not "disable the depth buffer".  OpenGL
   // suppresses depth writes when GL_DEPTH_TEST is disabled, regardless of
   // glDepthMask, so keep the test enabled whenever either operation is used.
   if (g_depthCompare || g_depthWrite)
      glEnable(GL_DEPTH_TEST);
   else
      glDisable(GL_DEPTH_TEST);
   glDepthFunc(g_depthCompare ? GL_LEQUAL : GL_ALWAYS);
   glDepthMask(g_depthWrite ? GL_TRUE : GL_FALSE);
}

void apply_blend_state()
{
   if (g_blendMode == kRenderBackendBlendOpaque) {
      glDisable(GL_BLEND);
   } else {
      glEnable(GL_BLEND);
      if (g_blendMode == kRenderBackendBlendMultiply)
         glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
      else if (g_blendMode == kRenderBackendBlendAdd)
         glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
      else
         glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA);
   }
}

const void *tight_texture_pixels(int width, int height, const void *pixels,
   int rowBytes, std::vector<uint8_t> &storage)
{
   const int tightRowBytes = width * 4;
   if (rowBytes == tightRowBytes)
      return pixels;
   storage.resize(static_cast<size_t>(tightRowBytes) * height);
   for (int y = 0; y < height; ++y)
      memcpy(storage.data() + static_cast<size_t>(y) * tightRowBytes,
         static_cast<const uint8_t *>(pixels) + static_cast<size_t>(y) * rowBytes,
         tightRowBytes);
   return storage.data();
}

bool draw_vertices(GLenum primitive, const sRenderBackendVertex *vertices,
   int count, bool second, int width, int height, bool fog, uint32_t fogColor,
   bool alphaTest, bool transparentBlack = false)
{
   if (!g_sceneProgram || !vertices || count <= 0 || width <= 0 || height <= 0)
      return false;
   glUseProgram(g_sceneProgram);
   glUniform2f(g_viewportUniform, static_cast<float>(width),
      static_cast<float>(height));
   glUniform1f(g_useTexture1Uniform, second ? 1.0f : 0.0f);
   glUniform1f(g_minLightUniform, g_minLight);
   glUniform1f(g_fogEnabledUniform, fog ? 1.0f : 0.0f);
   glUniform3f(g_fogColorUniform, ((fogColor >> 16) & 255) / 255.0f,
      ((fogColor >> 8) & 255) / 255.0f, (fogColor & 255) / 255.0f);
   glUniform1f(g_alphaTestUniform, alphaTest ? 1.0f : 0.0f);
   glUniform1f(g_transparentBlackUniform, transparentBlack ? 1.0f : 0.0f);
   glBindVertexArray(g_sceneVertexArray);
   glBindBuffer(GL_ARRAY_BUFFER, g_sceneBuffer);
   glBufferData(GL_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(static_cast<size_t>(count) * sizeof(*vertices)),
      vertices, GL_STREAM_DRAW);
   glDrawArrays(primitive, 0, count);
   return true;
}

void clear_depth()
{
   glDepthMask(GL_TRUE);
   glClear(GL_DEPTH_BUFFER_BIT);
   glDepthMask(g_depthWrite ? GL_TRUE : GL_FALSE);
}

void write_u16(FILE *file, uint16_t value)
{
   unsigned char bytes[] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8)};
   fwrite(bytes, sizeof(bytes), 1, file);
}

void write_u32(FILE *file, uint32_t value)
{
   unsigned char bytes[] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
      static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
   fwrite(bytes, sizeof(bytes), 1, file);
}

bool capture_frame(const char *path)
{
   int captureWidth = g_clientWidth, captureHeight = g_clientHeight;
   SDL_GL_GetDrawableSize(g_window, &captureWidth, &captureHeight);
#define g_clientWidth captureWidth
#define g_clientHeight captureHeight
   const int rowBytes = (g_clientWidth * 3 + 3) & ~3;
   std::vector<unsigned char> rgba(static_cast<size_t>(g_clientWidth) * g_clientHeight * 4);
   std::vector<unsigned char> row(rowBytes);
   FILE *file = fopen(path, "wb");
   if (!file) return false;
   glReadPixels(0, 0, g_clientWidth, g_clientHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
   write_u16(file, 0x4d42); write_u32(file, 54 + rowBytes * g_clientHeight);
   write_u16(file, 0); write_u16(file, 0); write_u32(file, 54); write_u32(file, 40);
   write_u32(file, g_clientWidth); write_u32(file, g_clientHeight); write_u16(file, 1); write_u16(file, 24);
   write_u32(file, 0); write_u32(file, rowBytes * g_clientHeight);
   write_u32(file, 0); write_u32(file, 0); write_u32(file, 0); write_u32(file, 0);
   for (int y = 0; y < g_clientHeight; ++y) {
      const unsigned char *source = rgba.data() + static_cast<size_t>(y) * g_clientWidth * 4;
      for (int x = 0; x < g_clientWidth; ++x) {
         row[x*3] = source[x*4+2]; row[x*3+1] = source[x*4+1]; row[x*3+2] = source[x*4];
      }
      fwrite(row.data(), row.size(), 1, file);
   }
   fclose(file);
   return true;
#undef g_clientWidth
#undef g_clientHeight
}
}

bool DarkRenderOpen(int width, int height, const char *title)
{
   g_PlatformLoadProgressHook = load_progress;   // (the loading bar, below)
   set_load_thread();
   if (g_context || g_rt) {
      // A logical mode change can replace the display device and its backing
      // pixel vector during an application frame. Do not retain a pointer to
      // the preceding mode's canvas.
      g_deferredPixels = nullptr;
      g_lastPixels = nullptr;
      g_width = width; g_height = height;
      DarkRenderSetTitle(title);
      SDL_GetWindowSize(g_window, &g_clientWidth, &g_clientHeight);
      update_relative_mouse();
      return true;
   }
   // WSLg exposes OpenGL through Mesa's D3D12 Gallium driver.  Its automatic
   // probe can try Zink first and fall back to llvmpipe despite /dev/dxg.
   if (getenv("WSL2_GUI_APPS_ENABLED")) {
      SDL_setenv("GALLIUM_DRIVER", "d3d12", 0);
      SDL_setenv("EGL_LOG_LEVEL", "fatal", 0);
   }
   // Prefer the compositor-native backend when Wayland is available. SDL 2
   // otherwise commonly chooses XWayland, which gives absolute coordinates in
   // a different desktop space under WSLg and some scaled desktops.
   if (getenv("WAYLAND_DISPLAY") && !getenv("SDL_VIDEODRIVER"))
      SDL_setenv("SDL_VIDEODRIVER", "wayland", 0);
   // WSLg's XWayland relative-pointer implementation can continue to draw
   // and move the host pointer even after accepting SDL relative mode. Force
   // SDL's warp implementation there, and explicitly forbid the relative
   // cursor on every backend.
   SDL_SetHint("SDL_MOUSE_RELATIVE_CURSOR_VISIBLE", "0");
   SDL_SetHint("SDL_MOUSE_RELATIVE_SCALING", "0");
   g_wslPointerWorkaround = getenv("WSL_DISTRO_NAME") || getenv("WSL_INTEROP");
   if (g_wslPointerWorkaround) {
      SDL_SetHint("SDL_MOUSE_RELATIVE_MODE_WARP", "1");
      // X11 cursor deltas passing through WSLg are substantially smaller
      // than raw device counts. Preserve the normal in-game sensitivity
      // range by compensating at SDL's relative-input boundary. An explicit
      // SDL hint supplied by the user still wins.
      if (!SDL_GetHint("SDL_MOUSE_RELATIVE_SPEED_SCALE"))
         SDL_SetHint("SDL_MOUSE_RELATIVE_SPEED_SCALE", "2.0");
   }
   if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
      fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
      return false;
   }
#ifdef DARK_OPENGL_ES
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
#endif
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef DARK_RT
   if (darkrt::Wanted()) {
      g_window = SDL_CreateWindow(title ? title : "Dark Engine",
         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
         SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
      if (g_window && darkrt::Open(g_window)) {
         g_rt = true;
         set_window_icon();
      } else if (g_window) {
         SDL_DestroyWindow(g_window);
         g_window = nullptr;
      }
   }
   if (g_rt) {
      const Uint8 blank[8] = {};
      g_invisibleCursor = SDL_CreateCursor(blank, blank, 8, 8, 0, 0);
      if (g_invisibleCursor) SDL_SetCursor(g_invisibleCursor);
      SDL_ShowCursor(SDL_DISABLE);
      fprintf(stderr, "macOS video backend: SDL/%s, RTGL1 path tracer\n",
         SDL_GetCurrentVideoDriver());
      g_width = g_clientWidth = width; g_height = g_clientHeight = height;
      SDL_GetWindowSize(g_window, &g_clientWidth, &g_clientHeight);
      g_quit = false; g_resizePending = false;
      g_focused = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
      update_relative_mouse();
      return true;
   }
#endif
   g_window = SDL_CreateWindow(title ? title : "Dark Engine",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
   if (g_window) set_window_icon();
   if (g_window) g_context = SDL_GL_CreateContext(g_window);
   if (!g_window || !g_context) {
      fprintf(stderr, "OpenGL: no %s: %s\n", g_window ? "context" : "window", SDL_GetError());
      DarkRenderClose(); return false;
   }
   SDL_GL_MakeCurrent(g_window, g_context);
   // Loading panels force a present for small progress-dial updates.  Do not
   // serialize hundreds of those updates to WSLg's compositor refresh.  The
   // hardware scene enables vsync when gameplay begins.
   set_swap_interval(0);
   const Uint8 blankCursor[8] = {};
   g_invisibleCursor = SDL_CreateCursor(blankCursor, blankCursor, 8, 8, 0, 0);
   if (g_invisibleCursor)
      SDL_SetCursor(g_invisibleCursor);
   SDL_ShowCursor(SDL_DISABLE);
#ifdef __APPLE__
   fprintf(stderr, "macOS video backend: SDL/%s, %s\n", SDL_GetCurrentVideoDriver(),
      reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
#else
   fprintf(stderr, "Linux video backend: SDL/%s\n", SDL_GetCurrentVideoDriver());
#endif
   g_width = g_clientWidth = width; g_height = g_clientHeight = height;
   g_quit = false; g_resizePending = false;
   g_focused = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
   update_relative_mouse();
   if (!create_scene_pipeline()) { DarkRenderClose(); return false; }
   glGenTextures(1, &g_overlayTexture);
   glDisable(GL_CULL_FACE);
   const char *tracePath = getenv("DARK_RENDER_TRACE");
   if (!g_trace && tracePath && *tracePath) g_trace = fopen(tracePath, "w");
   return true;
}

void DarkRenderClose(void)
{
   if (g_context) {
      SDL_GL_MakeCurrent(g_window, g_context);
      if (g_overlayTexture) glDeleteTextures(1, &g_overlayTexture);
      destroy_scene_pipeline();
   }
   if (g_context) SDL_GL_DeleteContext(g_context);
#ifdef DARK_RT
   if (g_rt) darkrt::Close();
   g_rt = false;
#endif
   if (g_window) SDL_DestroyWindow(g_window);
   if (g_invisibleCursor) {
      SDL_SetCursor(SDL_GetDefaultCursor());
      SDL_FreeCursor(g_invisibleCursor);
   }
   g_window = nullptr; g_context = nullptr; g_overlayTexture = 0;
   g_invisibleCursor = nullptr;
   g_overlayWidth = g_overlayHeight = g_overlayStorage = 0;
   g_relativeActive = false;
   g_wslPointerWorkaround = false;
   g_pointerWarpWarningPrinted = false;
   g_swapInterval = -1;
   g_swapIntervalApplied = false;
   g_unsupportedSwapIntervals = 0;
   g_applicationFrameDepth = 0;
   g_presentPending = false;
   g_resizePending = false;
   g_deferredPixels = nullptr;
   g_lastPixels = nullptr;
   g_PlatformLoadProgressHook = nullptr;
   load_finish();
   SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
   if (g_trace) { fclose(g_trace); g_trace = nullptr; }
}

bool DarkRenderSetFullscreen(bool enabled)
{
   if (!g_window) return false;
   const bool current = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
   if (current != enabled) {
      const Uint32 flags = enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
      if (SDL_SetWindowFullscreen(g_window, flags) != 0) {
         fprintf(stderr, "SDL fullscreen change failed: %s\n", SDL_GetError());
         return false;
      }
   }
   SDL_GetWindowSize(g_window, &g_clientWidth, &g_clientHeight);
   RenderBackendTrace("sdl-window requested-fullscreen=%d actual-fullscreen=%d client=%dx%d",
      enabled, !!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN),
      g_clientWidth, g_clientHeight);
   return true;
}

bool DarkRenderPumpEvents(void)
{
   if (!g_window) return false;
   SDL_Event event;
   bool haveMotion = false;
   int motionX = 0, motionY = 0;
   int relativeX = 0, relativeY = 0;
   const auto flushMotion = [&]() {
      if (!haveMotion)
         return;
      logical_point(motionX, motionY);
      DarkInputMouse(static_cast<short>(motionX), static_cast<short>(motionY),
         MOUSE_MOTION, g_buttons, 0);
      haveMotion = false;
      motionX = motionY = 0;
   };
   while (SDL_PollEvent(&event)) {
      if (event.type == SDL_MOUSEMOTION) {
         if (g_relativeActive) {
            relativeX += event.motion.xrel;
            relativeY += event.motion.yrel;
         } else {
            haveMotion = true;
            motionX = event.motion.x;
            motionY = event.motion.y;
         }
         continue;
      }
      flushMotion();
      if (event.type == SDL_QUIT) g_quit = true;
      else if (event.type == SDL_WINDOWEVENT) {
         if (event.window.event == SDL_WINDOWEVENT_CLOSE) g_quit = true;
         else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            g_clientWidth = event.window.data1; g_clientHeight = event.window.data2;
            g_resizePending = true;
         } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            g_focused = true; update_relative_mouse();
            fprintf(stderr, "SDL: window focus gained (%.1f s)\n", SDL_GetTicks() / 1000.0);
         } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            g_focused = false; update_relative_mouse();
            fprintf(stderr, "SDL: window focus lost (%.1f s)\n", SDL_GetTicks() / 1000.0);
         }
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
#if defined(DARK_RT)
         if (g_rt && darkrt::HandleKey(event.key.keysym.scancode, event.key.keysym.mod,
                                       event.type == SDL_KEYDOWN, event.key.repeat != 0))
            continue;
#endif
         if (!g_rt && glpanel::key(event.key.keysym.scancode, event.key.keysym.mod,
                                   event.type == SDL_KEYDOWN, event.key.repeat != 0))
            continue;
         uchar scan = key_scan(event.key.keysym.scancode);
         if (scan != KBC_NONE && !event.key.repeat) {
            DarkInputKey(scan, event.type == SDL_KEYDOWN);
            if (event.key.keysym.scancode < SDL_NUM_SCANCODES)
               g_keysDown[event.key.keysym.scancode] = event.type == SDL_KEYDOWN;
         }
      } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
         int logical = event.button.button == SDL_BUTTON_LEFT ? 0 :
            event.button.button == SDL_BUTTON_RIGHT ? 1 :
            event.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
         if (logical >= 0) {
            const bool down = event.type == SDL_MOUSEBUTTONDOWN;
            const uchar bit = static_cast<uchar>(1 << logical);
            if (down) g_buttons |= bit; else g_buttons &= ~bit;
            const uchar type = static_cast<uchar>(1 << (1 + 2 * logical + !down));
            int x = g_relativeActive ? g_width / 2 : event.button.x;
            int y = g_relativeActive ? g_height / 2 : event.button.y;
            if (!g_relativeActive) logical_point(x, y);
            DarkInputMouse(static_cast<short>(x), static_cast<short>(y), type, g_buttons, 0);
         }
      } else if (event.type == SDL_MOUSEWHEEL) {
         int x = g_width / 2, y = g_height / 2;
         if (!g_relativeActive) {
            SDL_GetMouseState(&x, &y); logical_point(x, y);
         }
         int direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
         int wheel = std::clamp(event.wheel.y * direction, -127, 127);
         if (wheel)
            DarkInputMouse(static_cast<short>(x), static_cast<short>(y),
               MOUSE_WHEEL, g_buttons, static_cast<char>(wheel));
      }
   }
   flushMotion();
   // A key the game still holds down that is up: its release never reached
   // the game (lost in a stall, or to the window losing focus) -- the
   // character walked on by itself and the keys seemed not to answer.
   if (const Uint8 *state = SDL_GetKeyboardState(nullptr))
      for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc)
         if (g_keysDown[sc] && !state[sc]) {
            g_keysDown[sc] = false;
            const uchar scan = key_scan(static_cast<SDL_Scancode>(sc));
            if (scan != KBC_NONE)
               DarkInputKey(scan, 0);
            fprintf(stderr, "SDL: key %s held by the game but up: released (%.1f s)\n",
                    SDL_GetScancodeName(static_cast<SDL_Scancode>(sc)), SDL_GetTicks() / 1000.0);
         }
   if (g_relativeActive && (relativeX || relativeY)) {
      DarkInputMouseRelative(relativeX, relativeY, g_buttons);
   }
   // Compositors may revoke relative mode while a menu or focus transition is
   // in progress. Reconcile the requested gameplay mode with SDL's actual
   // state after all pending window events have been handled.
   update_relative_mouse();
   if (g_relativeActive && g_wslPointerWorkaround) {
      int mouseX, mouseY, windowX, windowY;
      SDL_GetGlobalMouseState(&mouseX, &mouseY);
      SDL_GetWindowPosition(g_window, &windowX, &windowY);
      const int edge = 8;
      if (mouseX <= windowX + edge || mouseY <= windowY + edge ||
          mouseX >= windowX + g_clientWidth - edge ||
          mouseY >= windowY + g_clientHeight - edge) {
         if (SDL_WarpMouseGlobal(windowX + g_clientWidth / 2,
                                 windowY + g_clientHeight / 2) != 0 &&
             !g_pointerWarpWarningPrinted) {
            fprintf(stderr, "SDL WSL pointer recenter unavailable: %s\n",
               SDL_GetError());
            g_pointerWarpWarningPrinted = true;
         }
      }
   }
   return g_quit;
}

bool DarkRenderTakeResize(int *width, int *height)
{
   if (!g_resizePending)
      return false;
   if (width)
      *width = g_clientWidth;
   if (height)
      *height = g_clientHeight;
   g_resizePending = false;
   return true;
}

bool DarkRenderPumpMovieEvents(void)
{
   if (!g_window)
      return true;
   SDL_Event event;
   bool stop = false;
   while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
         g_quit = true;
         stop = true;
      } else if (event.type == SDL_KEYDOWN ||
                 event.type == SDL_MOUSEBUTTONDOWN ||
                 event.type == SDL_CONTROLLERBUTTONDOWN) {
         stop = true;
      } else if (event.type == SDL_WINDOWEVENT) {
         if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
            g_quit = true;
            stop = true;
         } else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            g_clientWidth = event.window.data1;
            g_clientHeight = event.window.data2;
         } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            g_focused = true;
         } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            g_focused = false;
         }
      }
   }
   return stop;
}

void DarkRenderBeginApplicationFrame(void)
{
   // Cursor save-under drawing can request several legacy FlushRect calls
   // while input is being pumped. Keep those updates in the back buffer and
   // expose only the completed engine frame.
   if (g_applicationFrameDepth == 0 && g_presentPending)
      swap_presented_frame();
   ++g_applicationFrameDepth;
}

void DarkRenderEndApplicationFrame(void)
{
   if (g_applicationFrameDepth > 0)
      --g_applicationFrameDepth;
   if (g_applicationFrameDepth == 0 && g_deferredPixels) {
      const void *pixels = g_deferredPixels;
      const uint8_t *palette = g_deferredPalette;
      const int width = g_deferredWidth;
      const int height = g_deferredHeight;
      const int rowBytes = g_deferredRowBytes;
      const int depth = g_deferredDepth;
      g_deferredPixels = nullptr;
      present_legacy_frame(pixels, width, height, rowBytes, depth, palette);
   }
   if (g_applicationFrameDepth == 0)
      swap_presented_frame();
}

void DarkRenderSetTitle(const char *title) { if (g_window) SDL_SetWindowTitle(g_window, title ? title : "Dark Engine"); }
void DarkRenderShowCursor(bool) { SDL_ShowCursor(SDL_DISABLE); }

void DarkRenderSetMousePosition(int x, int y)
{
   if (!g_window || g_width <= 0 || g_height <= 0 || g_relativeActive) return;
   int vx, vy, vw, vh; viewport(vx, vy, vw, vh);
   const int targetX = vx + x * vw / g_width;
   const int targetY = vy + y * vh / g_height;
   SDL_WarpMouseInWindow(g_window, targetX, targetY);
}

void DarkRenderSetRelativeMouse(bool enabled)
{
   g_relativeMouse = enabled;
   update_relative_mouse();
}

static void present_legacy_frame(const void *pixels, int width, int height,
   int rowBytes, int depth, const uint8_t *palette)
{
   if (pixels) {
      g_lastPixels = pixels; g_lastPalette = palette; g_lastWidth = width;
      g_lastHeight = height; g_lastRowBytes = rowBytes; g_lastDepth = depth;
   }
#ifdef DARK_RT
   if (g_rt) {
      if (!pixels) return;
      darkrt::SetCanvas(pixels, width, height, rowBytes, depth, palette, g_scene,
         rt_viewport_rect());
      if (g_scene && g_presentedGeneration != g_sceneGeneration) {
         g_presentedGeneration = g_sceneGeneration; ++g_hardwareFrameCount;
      }
      g_presentPending = true;
      return;
   }
#endif
   if (!g_context || !pixels) return; SDL_GL_MakeCurrent(g_window, g_context);
   int vx, vy, vw, vh; viewport(vx, vy, vw, vh);
   if (!g_scene) {
      // Logical mode changes may alter the aspect-fitted viewport. Clear the
      // whole output so the previously presented menu cannot remain visible
      // in newly exposed bars around the rebuilt canvas.
      set_gl_viewport(0, 0, g_clientWidth, g_clientHeight);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
   }
   set_gl_viewport(vx, vy, vw, vh);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_overlayTexture);
   glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
   glActiveTexture(GL_TEXTURE0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (depth == 16) {
      // The legacy canvas is already tightly packed RGB565. Upload it in its
      // native format and let the fragment shader apply Dark's index-zero
      // gameplay transparency key. Expanding a native-resolution Android
      // canvas to RGBA on the CPU consumed most of an entire core.
      glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, rowBytes / 2);
      if (width != g_overlayWidth || height != g_overlayHeight ||
          g_overlayStorage != 16) {
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
         g_overlayWidth = width; g_overlayHeight = height;
         g_overlayStorage = 16;
      } else {
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
      }
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   } else {
      g_overlay.resize(static_cast<size_t>(width) * height);
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
         uint8_t r, g, b; uint32_t raw;
         if (depth == 8) { raw = static_cast<const uint8_t *>(pixels)[y * rowBytes + x]; r = palette[raw*3]; g = palette[raw*3+1]; b = palette[raw*3+2]; }
         else { raw = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(pixels) + y * rowBytes)[x];
            r = ((raw >> 10) & 31) * 255 / 31; g = ((raw >> 5) & 31) * 255 / 31; b = (raw & 31) * 255 / 31; }
         uint8_t a = g_scene && raw == 0 ? 0 : 255; g_overlay[static_cast<size_t>(y)*width+x] = r | (g<<8) | (b<<16) | (a<<24);
      }
      if (width != g_overlayWidth || height != g_overlayHeight ||
          g_overlayStorage != 32) {
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, g_overlay.data());
         g_overlayWidth = width; g_overlayHeight = height;
         g_overlayStorage = 32;
      } else {
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
            GL_RGBA, GL_UNSIGNED_BYTE, g_overlay.data());
      }
   }
   glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
   glEnable(GL_BLEND);
   glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
      GL_ONE_MINUS_SRC_ALPHA);
   const float w = static_cast<float>(width), h = static_cast<float>(height);
   const sRenderBackendVertex quad[] = {
      {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
      {w, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0},
      {w, h, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
      {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
      {w, h, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
      {0, h, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0}};
   draw_vertices(GL_TRIANGLES, quad, 6, false, width, height, false, 0,
      false, g_scene && depth == 16);
   if (g_scene && g_presentedGeneration != g_sceneGeneration) {
      g_presentedGeneration = g_sceneGeneration; ++g_hardwareFrameCount;
      const char *path = getenv("DARK_RENDER_CAPTURE");
      const char *frameText = getenv("DARK_RENDER_CAPTURE_FRAME");
      int frame = frameText ? std::max(atoi(frameText), 1) : 1;
      if (!g_captureComplete && path && *path && g_hardwareFrameCount >= static_cast<unsigned>(frame))
         g_captureComplete = capture_frame(path);
   }
   g_presentPending = true;
}

// ---- the loading bar -------------------------------------------------------
// While a mission or a saved game loads, the engine runs for seconds without
// presenting (its screen updates wait for the end of the application frame
// the load runs in): the window looked frozen and macOS called it not
// responding. PlatformLoadProgress, called through the load, pumps the
// window's events and, a few times a second, puts the last screen up again
// with a bar along its foot. The bar moves with the time the last load took
// (remembered between runs), easing toward the end, so it never stops.
namespace {
using LoadClock = std::chrono::steady_clock;
LoadClock::time_point g_loadStart, g_loadLast, g_loadDrawn;
bool g_loadActive = false;
std::thread::id g_loadThread;   // the window's (set when it opens)
double g_loadExpected = 0;   // seconds; 0 not read yet
std::vector<uint8_t> g_loadCanvas;

std::string load_time_path()
{
   const char *home = getenv("HOME");
   if (!home || !*home)
      return "load_time.txt";
#if defined(__APPLE__)
   return std::string(home) + "/Library/Application Support/Dark Engine Restored/load_time.txt";
#else
   return std::string(home) + "/.config/dark-engine-restored/load_time.txt";
#endif
}

void load_finish()
{
   if (!g_loadActive)
      return;
   g_loadActive = false;
   const double took = std::chrono::duration<double>(g_loadLast - g_loadStart).count();
   if (took < 1.0)
      return;
   // a running average: one slow first load (caches cold) does not stick
   g_loadExpected = g_loadExpected > 0 ? 0.5 * g_loadExpected + 0.5 * took : took;
   if (FILE *f = fopen(load_time_path().c_str(), "w")) {
      fprintf(f, "%.2f\n", g_loadExpected);
      fclose(f);
   }
}

uint8_t nearest_index(const uint8_t *pal, int r, int g, int b)
{
   int best = 0, bestD = 1 << 30;
   for (int i = 1; i < 256; ++i) {
      const int dr = pal[i * 3] - r, dg = pal[i * 3 + 1] - g, db = pal[i * 3 + 2] - b;
      const int d = dr * dr + dg * dg + db * db;
      if (d < bestD) { bestD = d; best = i; }
   }
   return uint8_t(best);
}

void draw_load_bar(float fraction)
{
   const bool deferred = g_deferredPixels != nullptr;
   const void *src = deferred ? g_deferredPixels : g_lastPixels;
   if (!src)
      return;
   const int w = deferred ? g_deferredWidth : g_lastWidth;
   const int h = deferred ? g_deferredHeight : g_lastHeight;
   const int rowBytes = deferred ? g_deferredRowBytes : g_lastRowBytes;
   const int depth = deferred ? g_deferredDepth : g_lastDepth;
   const uint8_t *pal = deferred ? g_deferredPalette : g_lastPalette;
   if (w <= 0 || h <= 0 || rowBytes <= 0 || (depth != 8 && depth != 16) || (depth == 8 && !pal))
      return;
   g_loadCanvas.assign(static_cast<const uint8_t *>(src),
                       static_cast<const uint8_t *>(src) + size_t(rowBytes) * h);
   // along the foot of the screen: a dark track, a warm fill, a lighter edge
   static const int kRgb[3][3] = {{24, 22, 18}, {196, 160, 96}, {120, 104, 76}};
   uint16_t c16[3];
   uint8_t c8[3] = {};
   for (int k = 0; k < 3; ++k) {
      c16[k] = uint16_t(((kRgb[k][0] >> 3) << 11) | ((kRgb[k][1] >> 2) << 5) | (kRgb[k][2] >> 3));
      if (depth == 8)
         c8[k] = nearest_index(pal, kRgb[k][0], kRgb[k][1], kRgb[k][2]);
   }
   const int x0 = w / 8, x1 = w - w / 8, bh = std::max(4, h / 90);
   const int y0 = h - h / 14 - bh, y1 = y0 + bh;
   const int fill = x0 + int(float(x1 - x0) * std::clamp(fraction, 0.0f, 1.0f));
   for (int y = y0 - 1; y <= y1; ++y) {
      if (y < 0 || y >= h)
         continue;
      uint8_t *row = g_loadCanvas.data() + size_t(y) * rowBytes;
      for (int x = x0 - 1; x <= x1; ++x) {
         if (x < 0 || x >= w)
            continue;
         const bool edge = y == y0 - 1 || y == y1 || x == x0 - 1 || x == x1;
         const int kind = edge ? 2 : (x < fill ? 1 : 0);
         if (depth == 8)
            row[x] = c8[kind];
         else
            reinterpret_cast<uint16_t *>(row)[x] = c16[kind];
      }
   }
   const bool scene = g_scene;
   g_scene = false;   // (the canvas alone: no scene behind it is traced yet)
   present_legacy_frame(g_loadCanvas.data(), w, h, rowBytes, depth, pal);
   g_presentPending = true;
   swap_presented_frame();
   g_scene = scene;
   // (the bar's copy is not the engine's canvas: not kept as the last one)
   g_lastPixels = src;
}

void set_load_thread() { g_loadThread = std::this_thread::get_id(); }

void load_progress(const char *stage)
{
   // (images are read on other threads too: the window is the main thread's)
   if (std::this_thread::get_id() != g_loadThread)
      return;
   const auto now = LoadClock::now();
   // A load is begun by the engine's load messages ("+..." stages); the rest
   // (world cells, images read, the path tracer's textures) only carry one
   // on -- the same reads happen in play too.
   const bool begins = stage && stage[0] == '+';
   if (g_loadActive && now - g_loadLast > std::chrono::seconds(4))
      load_finish();
   if (!g_loadActive) {
      if (!begins)
         return;
      g_loadActive = true;
      g_loadStart = now;
      g_loadDrawn = now;
      if (g_loadExpected <= 0) {
         g_loadExpected = 12.0;
         if (FILE *f = fopen(load_time_path().c_str(), "r")) {
            double v = 0;
            if (fscanf(f, "%lf", &v) == 1 && v > 0.5 && v < 600)
               g_loadExpected = v;
            fclose(f);
         }
      }
   }
   g_loadLast = now;
   if (now - g_loadDrawn < std::chrono::milliseconds(66))
      return;
   g_loadDrawn = now;
   SDL_PumpEvents();   // the window answers (no spinning beach ball)
   const double t = std::chrono::duration<double>(now - g_loadStart).count();
   if (t < 0.4)
      return;   // a quick one: no bar flashed up
#ifdef DARK_RT
   if (g_rt && darkrt::FrameInProgress())
      return;   // (the path tracer's own frame is being built: events only)
#endif
   // ~85% at the time the last load took, then on toward the end
   const double fraction = 1.0 - std::exp(-t / std::max(0.5, g_loadExpected / 1.9));
   draw_load_bar(float(fraction));
}
} // namespace

void DarkRenderPresent(const void *pixels, int width, int height, int rowBytes,
                       int depth, const uint8_t *palette)
{
   if (g_applicationFrameDepth > 0) {
      // Cursor save-under and the end-of-frame page flip can both flush the
      // same stable display canvas. Retain only its final state; uploading
      // every intermediate request doubled native-resolution Android work.
      g_deferredPixels = pixels;
      g_deferredPalette = palette;
      g_deferredWidth = width;
      g_deferredHeight = height;
      g_deferredRowBytes = rowBytes;
      g_deferredDepth = depth;
      return;
   }
   present_legacy_frame(pixels, width, height, rowBytes, depth, palette);
   swap_presented_frame();
}

bool DarkRenderPresentMovieFrame(const void *rgba, int width, int height,
                                 int rowBytes)
{
#ifdef DARK_RT
   if (g_rt) {
      if (!rgba || width <= 0 || height <= 0 || rowBytes < width * 4)
         return false;
      g_scene = false;
      int dw, dh; window_drawable_size(dw, dh);
      // letterbox the movie at its own aspect
      float vw = float(dw), vh = float(dh);
      const float aspect = float(width) / float(height);
      if (vw / vh > aspect) vw = vh * aspect; else vh = vw / aspect;
      const darkrt::Rect vp{(dw - vw) * 0.5f, (dh - vh) * 0.5f, vw, vh};
      return darkrt::MovieFrame(rgba, width, height, rowBytes, vp, dw, dh);
   }
#endif
   if (!g_context || !rgba || width <= 0 || height <= 0 ||
       rowBytes < width * 4)
      return false;
   SDL_GL_MakeCurrent(g_window, g_context);
   set_swap_interval(1);
   g_scene = false;

   int viewportWidth = g_clientWidth;
   int viewportHeight = g_clientHeight;
   const double sourceAspect = (double)width / height;
   const double targetAspect = viewportHeight > 0
      ? (double)viewportWidth / viewportHeight : sourceAspect;
   int viewportX = 0;
   int viewportY = 0;
   if (targetAspect > sourceAspect) {
      viewportWidth = (int)(viewportHeight * sourceAspect + 0.5);
      viewportX = (g_clientWidth - viewportWidth) / 2;
   } else {
      viewportHeight = (int)(viewportWidth / sourceAspect + 0.5);
      viewportY = (g_clientHeight - viewportHeight) / 2;
   }

   set_gl_viewport(0, 0, g_clientWidth, g_clientHeight);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   set_gl_viewport(viewportX, viewportY, viewportWidth, viewportHeight);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, g_overlayTexture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   const void *pixels = rgba;
   if (rowBytes != width * 4) {
      g_overlay.resize((size_t)width * height);
      for (int y = 0; y < height; ++y)
         memcpy(g_overlay.data() + (size_t)y * width,
                (const uint8_t *)rgba + (size_t)y * rowBytes,
                (size_t)width * 4);
      pixels = g_overlay.data();
   }
   if (width != g_overlayWidth || height != g_overlayHeight ||
       g_overlayStorage != 32) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      g_overlayWidth = width;
      g_overlayHeight = height;
      g_overlayStorage = 32;
   } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                      GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   }

   glDisable(GL_DEPTH_TEST);
   glDepthMask(GL_FALSE);
   glDisable(GL_BLEND);
   const float w = (float)width;
   const float h = (float)height;
   const sRenderBackendVertex quad[] = {
      {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
      {w, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0},
      {w, h, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
      {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
      {w, h, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
      {0, h, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0}
   };
   draw_vertices(GL_TRIANGLES, quad, 6, false, width, height, false, 0,
      false);
   SDL_GL_SwapWindow(g_window);
   return true;
}

extern "C" {
int RenderBackendAvailable(void) { return g_context != nullptr || g_rt; }
void RenderBackendSetScaleToWindow(int enabled) { g_scale = enabled != 0; }
void RenderBackendSetFitToViewport(int enabled) { g_fit = enabled != 0; }
void RenderBackendSetInteractiveResize(int) {}
void RenderBackendSetPreserveCanvas(int enabled) { g_preserve = enabled != 0; }
int RenderBackendPreserveCanvas(void) { return g_preserve; }
void RenderBackendSetWindowedClientSizeHint(int, int) {}
int RenderBackendGetSettledClientSize(int *width, int *height)
{
   if (!width || !height || !(g_context || g_rt) ||
       g_clientWidth <= 0 || g_clientHeight <= 0)
      return 0;
   *width = g_clientWidth;
   *height = g_clientHeight;
   return 1;
}
int RenderBackendClientToLogicalPoint(int *x, int *y) { if (!x || !y || !(g_context || g_rt)) return 0; logical_point(*x, *y); return 1; }
int RenderBackendLogicalToClientPoint(int *x, int *y) { int vx,vy,vw,vh; if (!x || !y || !(g_context || g_rt)) return 0; viewport(vx,vy,vw,vh); *x=vx+*x*vw/g_width; *y=vy+*y*vh/g_height; return 1; }
int RenderBackendBeginFrame(void)
{
#ifdef DARK_RT
   if (g_rt) { set_swap_interval(1); g_scene=true; ++g_sceneGeneration; darkrt::BeginSceneFrame(); return 1; }
#endif
   if (!g_context) return 0; SDL_GL_MakeCurrent(g_window, g_context);
   set_swap_interval(1);
   int x,y,w,h; viewport(x,y,w,h); set_gl_viewport(x,y,w,h); glClearColor(0,0,0,1);
   glDepthMask(GL_TRUE); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
   apply_depth_state(); apply_blend_state();
   g_scene=true;++g_sceneGeneration;return 1;
}
void RenderBackendEndFrame(void) { if (!g_rt) glFlush(); }
void RenderBackendDeactivateScene(void) { g_scene=false;set_swap_interval(0); }
void RenderBackendClearDepth(void)
{
   if (g_context) clear_depth();
}
void RenderBackendSetDepth(int compare, int write) { g_depthCompare=compare!=0;g_depthWrite=write!=0;if(!g_rt)apply_depth_state(); }
void RenderBackendSetBlend(int mode)
{
   g_blendMode=mode;
#ifdef DARK_RT
   if (g_rt) { darkrt::SetBlend(mode); return; }
#endif
   apply_blend_state();
}
void RenderBackendSetAlphaTest(int enabled)
{
   g_alphaTest=enabled;
#ifdef DARK_RT
   if (g_rt) darkrt::SetAlphaTest(enabled);
#endif
}
void RenderBackendSetSampler(int level, int wrap, int smooth)
{
   if (level < 0 || level >= 2) return;
   if (g_rt) return;
   g_samplerWrap[level]=wrap!=0;g_samplerSmooth[level]=smooth!=0;
   Texture *texture=static_cast<Texture*>(g_textures[level]);if(texture)bind_texture(level,texture);
}
void RenderBackendSetFog(int enabled, uint32_t rgb) { g_fog=enabled;g_fogColor=rgb; }
void *RenderBackendCreateTexture(int width,int height,const void *rgba,int rowBytes)
{
#ifdef DARK_RT
   if (g_rt) return darkrt::CreateTexture(width,height,rgba,rowBytes);
#endif
   if(width<=0||height<=0||!rgba||rowBytes<width*4)return nullptr;
   std::vector<uint8_t> storage;
   const void *pixels=tight_texture_pixels(width,height,rgba,rowBytes,storage);
   Texture *t=new Texture; glGenTextures(1,&t->name); t->width=width;t->height=height; glBindTexture(GL_TEXTURE_2D,t->name);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);return t;
}
int RenderBackendUpdateTexture(void *handle,int width,int height,const void *rgba,int rowBytes)
{
#ifdef DARK_RT
   if (g_rt) return darkrt::UpdateTexture(handle,width,height,rgba,rowBytes);
#endif
   Texture *texture = static_cast<Texture *>(handle);
   if (!g_context || !texture || width <= 0 || height <= 0 || !rgba ||
       rowBytes < width * 4)
      return 0;

   std::vector<uint8_t> storage;
   const void *pixels = tight_texture_pixels(width, height, rgba, rowBytes,
      storage);

   // Lightmap atlases are updated while the legacy renderer is logically on
   // texture stage one, but OpenGL's active unit is independent of that
   // state.  Do the upload on a known unit and restore all state that affects
   // client-memory texture transfers.  In particular, never claim that an
   // animated-light reload succeeded when the GLES driver rejected it.
   GLint previousActive = GL_TEXTURE0;
   GLint previousBinding = 0;
   GLint previousAlignment = 4;
   GLint previousRowLength = 0;
   glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
   glActiveTexture(GL_TEXTURE0);
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
   glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
   glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previousRowLength);

   glBindTexture(GL_TEXTURE_2D, texture->name);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
   while (glGetError() != GL_NO_ERROR) {}

   if (texture->width == width && texture->height == height)
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
         GL_UNSIGNED_BYTE, pixels);
   else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
         GL_UNSIGNED_BYTE, pixels);

   GLenum error = glGetError();
   if (error != GL_NO_ERROR && texture->width == width &&
       texture->height == height) {
      // Some GLES drivers can lose mutable texture storage across a context
      // transition.  Reallocate the same texture object so existing Dark
      // texture IDs and bindings remain valid.
      while (glGetError() != GL_NO_ERROR) {}
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
         GL_UNSIGNED_BYTE, pixels);
      error = glGetError();
   }

   glPixelStorei(GL_UNPACK_ROW_LENGTH, previousRowLength);
   glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
   glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
   glActiveTexture(static_cast<GLenum>(previousActive));

   if (error != GL_NO_ERROR)
      return 0;
   texture->width = width;
   texture->height = height;
   return 1;
}
void RenderBackendDestroyTexture(void *handle) {
#ifdef DARK_RT
   if (g_rt) { darkrt::DestroyTexture(handle); return; }
#endif
   Texture*t=static_cast<Texture*>(handle);if(!t)return;for(void*&bound:g_textures)if(bound==t)bound=nullptr;glDeleteTextures(1,&t->name);delete t; }
void RenderBackendBindTexture(int level,void *texture) {
#ifdef DARK_RT
   if (g_rt) { if(level>=0&&level<2) g_textures[level]=texture; darkrt::BindTexture(level,texture); return; }
#endif
   if(level>=0&&level<2){g_textures[level]=texture;bind_texture(level,static_cast<Texture*>(texture));} }
int RenderBackendDraw(int primitive,const sRenderBackendVertex *vertices,int count,int second)
{
#ifdef DARK_RT
   if(g_rt) return darkrt::Draw(primitive,vertices,count,g_width,g_height,rt_viewport_rect());
#endif
   if(!g_context||!vertices||count<=0)return 0; bind_texture(0,static_cast<Texture*>(g_textures[0]));
   bind_texture(1,second?static_cast<Texture*>(g_textures[1]):nullptr);
   glActiveTexture(GL_TEXTURE0);
   GLenum p=primitive==kRenderBackendLines?GL_LINES:primitive==kRenderBackendPoints?GL_POINTS:GL_TRIANGLES;
   return draw_vertices(p,vertices,count,second!=0,g_width,g_height,g_fog,
      g_fogColor,g_alphaTest);
}
void RenderBackendTraceReset(void) { if(g_trace){fclose(g_trace);g_trace=nullptr;} const char*p=getenv("DARK_RENDER_TRACE");if(p&&*p)g_trace=fopen(p,"w"); }
void RenderBackendTrace(const char *format,...) { if(!g_trace)return;va_list args;va_start(args,format);vfprintf(g_trace,format,args);va_end(args);fputc('\n',g_trace);fflush(g_trace); }
int RenderBackendCaptureComplete(void) { return g_captureComplete; }
}
