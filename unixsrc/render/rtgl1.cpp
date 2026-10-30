// The RTGL1 path tracer as a Dark Engine renderer (macOS, Metal).
//
// RTGL1 -- the Metal build from the Doom 64 RT / Raze RT Mac ports -- is
// loaded at run time from <rt folder>/bin/libRTGL1.dylib. The rt folder is
// $THIEF_RT_DIR, else "rt" next to the app bundle, else "rt" in the working
// directory.
//
// What is drawn, per frame:
//   * the world representation: every cell polygon of the mission, with its
//     texture, handed over by the portal renderer (render_rt.h);
//   * objects, AI, effects: the triangles lgd3d draws for the main view, with
//     world positions recovered from r3d's camera transform;
//   * the object-light table as sphere and spot lights;
//   * lgd3d's 2D draws after the main view, then the legacy 2D canvas (HUD,
//     menus, text), composited over the path-traced image.
//
// Settings: Cmd+, (or F10) in game opens the ray tracing settings panel;
// changes apply at once and are saved to
// ~/Library/Application Support/Dark Engine Restored/rt_settings.txt.
// Environment variables, when set, override the saved values at start:
//   THIEF_RT=1            use the ray tracer (run.sh sets it)
//   THIEF_RT_UPSCALE      0 off (render scale), 1 quality, 2 balanced,
//                         3 performance (default), 4 ultra performance,
//                         5 custom render scale  (MetalFX)
//   THIEF_RT_SCALE        render scale for modes 0 and 5 (0.25 .. 1)
//   THIEF_RT_LIGHT        light brightness multiplier (default 1)
//   THIEF_RT_SKY          sky brightness (default 1)
//   THIEF_RT_FPS=1        frame rate counter
//   THIEF_RT_FLIP=1       mirror the camera's right vector (diagnostic)
//   THIEF_RT_DEBUG=1      RTGL1 info messages, per-second statistics

#define RG_USE_SURFACE_METAL
#pragma pack(push, 8)
#include <RTGL1/RTGL1.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
// SDK headers end with "#pragma pack()", which restores the engine's packing
#pragma pack(8)

#include <render_rt.h>
#include "rt_backend_internal.h"
#include "rt_font.h"

extern "C" int g_RenderRTCaptureVertices = 0;
extern "C" int g_RenderRTEmissiveSprite = 0;
extern "C" int g_RenderRTShadowOnly = 0;

namespace {

// Dark units are feet.
constexpr float kMetersPerUnit = 0.3048f;

RgInterface rt{};
RgLibraryHandle g_lib = nullptr;
bool g_active = false;
SDL_MetalView g_metalView = nullptr;
std::string g_rtDir;

struct Settings {
   int upscale = 3;
   float light = 1.0f;
   float sky = 1.0f;
   float glow = 1.0f;         // light fixtures
   float spriteGlow = 0.35f;  // glow and particle sprites
   int water = 0;             // 0 translucent (as the game), 1 RTGL1 water, 2 opaque
   int gi = 0;                // bounce light: 0 two bounces at half rate (as Blood RT),
                              // 1 one bounce, 2 two bounces, 3 one bounce at half rate
   bool volumetric = false;   // haze lit by every light
   int dynres = 0;            // dynamic resolution's target fps, 0 off
   int maxLights = 64;        // lights given to the path tracer, nearest first; 0 all
   int flicker = 0;           // flickering lights: 0 smoothed, 1 as the game, 2 nearly steady
   bool smoothModels = false; // models shaded smooth (their normals blended across edges)
   float ambient = 1.0f;      // the level's ambient light, as a faint glow of every surface
   bool lightCull = true;     // skip lights the level's lighting has on no cell near or in view
   float minRoughness = 0.7f; // no surface glossier than this (1: no glossy highlights)
   int debugView = 0;         // 0 off, 1 shadow visibility, 2 direct diffuse, 3 specular, 4 bounce
   float waterAlpha = 0.55f;
   float scale = 0.5f;      // render scale for upscale modes 0 and 5
   bool fps = false;
   bool vsync = true;
   float lightSize = 0.25f;
   bool flip = false;
   bool debug = false;
} g_cfg;

float env_float(const char *name, float fallback)
{
   const char *v = getenv(name);
   return v && *v ? (float)atof(v) : fallback;
}

const Settings kDefaults;

std::string settings_path()
{
   const char *home = getenv("HOME");
   if (!home || !*home)
      return "rt_settings.txt";
#if defined(__APPLE__)
   return std::string(home) + "/Library/Application Support/Dark Engine Restored/rt_settings.txt";
#else
   return std::string(home) + "/.config/dark-engine-restored/rt_settings.txt";
#endif
}

void load_settings()
{
   FILE *f = fopen(settings_path().c_str(), "r");
   if (!f)
      return;
   char key[64];
   float value;
   while (fscanf(f, " %63[a-z_] = %f", key, &value) == 2) {
      const std::string k = key;
      if (k == "upscale") g_cfg.upscale = std::clamp(int(value), 0, 5);
      else if (k == "scale") g_cfg.scale = std::clamp(value, 0.25f, 1.0f);
      else if (k == "fps") g_cfg.fps = value != 0;
      else if (k == "vsync") g_cfg.vsync = value != 0;
      else if (k == "light") g_cfg.light = std::clamp(value, 0.05f, 20.0f);
      else if (k == "sky") g_cfg.sky = std::clamp(value, 0.0f, 20.0f);
      else if (k == "light_size") g_cfg.lightSize = std::clamp(value, 0.02f, 2.0f);
      else if (k == "glow") g_cfg.glow = std::clamp(value, 0.0f, 10.0f);
      else if (k == "water") g_cfg.water = std::clamp(int(value), 0, 2);
      else if (k == "sprite_glow") g_cfg.spriteGlow = std::clamp(value, 0.0f, 10.0f);
      else if (k == "gi") g_cfg.gi = std::clamp(int(value), 0, 3);
      else if (k == "volumetric") g_cfg.volumetric = value != 0;
      else if (k == "dynres") g_cfg.dynres = std::clamp(int(value), 0, 240);
      else if (k == "max_lights") g_cfg.maxLights = std::clamp(int(value), 0, 4096);
      else if (k == "flicker") g_cfg.flicker = std::clamp(int(value), 0, 2);
      else if (k == "smooth_models") g_cfg.smoothModels = value != 0;
      else if (k == "ambient") g_cfg.ambient = std::clamp(value, 0.0f, 20.0f);
      else if (k == "light_cull") g_cfg.lightCull = value != 0;
      else if (k == "min_roughness") g_cfg.minRoughness = std::clamp(value, 0.0f, 1.0f);
   }
   fclose(f);
}

void save_settings()
{
   const std::string path = settings_path();
   // the folder normally exists already (the game path is remembered there)
   const std::string dir = path.substr(0, path.rfind('/'));
   mkdir(dir.c_str(), 0755);
   FILE *f = fopen(path.c_str(), "w");
   if (!f)
      return;
   fprintf(f, "upscale = %d\nscale = %.2f\nfps = %d\nvsync = %d\nlight = %.3f\nsky = %.3f\n"
              "light_size = %.3f\nglow = %.3f\nwater = %d\nsprite_glow = %.3f\ngi = %d\n"
              "volumetric = %d\ndynres = %d\nmax_lights = %d\nmin_roughness = %.2f\nlight_cull = %d\nflicker = %d\nambient = %.3f\nsmooth_models = %d\n",
           g_cfg.upscale, g_cfg.scale, int(g_cfg.fps), int(g_cfg.vsync), g_cfg.light, g_cfg.sky,
           g_cfg.lightSize, g_cfg.glow, g_cfg.water, g_cfg.spriteGlow, g_cfg.gi,
           int(g_cfg.volumetric), g_cfg.dynres, g_cfg.maxLights, g_cfg.minRoughness,
           int(g_cfg.lightCull), g_cfg.flicker, g_cfg.ambient, int(g_cfg.smoothModels));
   fclose(f);
}

// the settings panel and frame counter
struct Ui {
   bool open = false;
   int row = 0;
   bool fontProvided = false;
   double hintUntil = -1;     // the "Cmd+, settings" hint, after the first scene
   double windowStart = 0, lastPresent = 0;
   int windowFrames = 0;
   double fps = 0, frameMs = 0, worstMs = 0, windowWorst = 0;
   // where a frame's time goes, averaged over the same window: the game's own
   // work, waiting in rgStartFrame for the GPU, submitting in rgDrawFrame
   double waitSum = 0, drawSum = 0, waitMs = 0, drawMs = 0;
   std::string status;        // a line shown in the panel for a while
   double statusUntil = 0;
} g_ui;

// ---- textures ----------------------------------------------------------

struct RtTexture {
   uint32_t serial = 0;
   char name[24] = {};
   int width = 0, height = 0;
   std::vector<uint8_t> rgba;
   bool dirty = true;      // pixels changed since RTGL1 got them
   bool provided = false;  // RTGL1 has a texture of this name
   bool hasAlpha = false;  // any texel not fully opaque
   bool cutout = false;    // any texel see-through (alpha < 50%)
   bool white = false;     // the 1x1 white "no texture" texture
};

uint32_t g_textureSerial = 0;
std::deque<RtTexture> g_textures;   // never freed: world groups refer to them
std::vector<RtTexture *> g_freeTextures;
RtTexture *g_bound[2] = {};
int g_blend = kRenderBackendBlendOpaque;
bool g_alphaTest = false;

void copy_pixels(RtTexture &t, int width, int height, const void *rgba, int rowBytes)
{
   t.width = width;
   t.height = height;
   t.rgba.resize(size_t(width) * height * 4);
   bool alpha = false, cutout = false;
   for (int y = 0; y < height; ++y) {
      const uint8_t *src = static_cast<const uint8_t *>(rgba) + size_t(y) * rowBytes;
      uint8_t *dst = t.rgba.data() + size_t(y) * width * 4;
      memcpy(dst, src, size_t(width) * 4);
      if (!cutout)
         for (int x = 0; x < width; ++x) {
            const uint8_t a = dst[x * 4 + 3];
            if (a != 255) alpha = true;
            if (a < 128) { cutout = true; break; }
         }
   }
   t.hasAlpha = alpha;
   t.cutout = cutout;
   t.white = width == 1 && height == 1 && t.rgba[0] == 255 && t.rgba[1] == 255 &&
             t.rgba[2] == 255 && t.rgba[3] == 255;
   t.dirty = true;
}

// The RTGL1 name for a texture, uploading its pixels first if they changed.
const char *texture_name(RtTexture *t)
{
   if (!t || t->white || t->rgba.empty())
      return nullptr;
   if (t->dirty) {
      if (t->provided)
         rt.rgMarkOriginalTextureAsDeleted(t->name);
      auto details = RgOriginalTextureDetailsEXT{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
         .pNext = nullptr,
         .flags = 0,
         .format = RG_FORMAT_R8G8B8A8_SRGB,
      };
      auto info = RgOriginalTextureInfo{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
         .pNext = &details,
         .pTextureName = t->name,
         .pPixels = t->rgba.data(),
         .size = {uint32_t(t->width), uint32_t(t->height)},
         .filter = RG_SAMPLER_FILTER_AUTO,
         .addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
         .addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
      };
      t->provided = rt.rgProvideOriginalTexture(&info) == RG_RESULT_SUCCESS;
      t->dirty = false;
   }
   return t->provided ? t->name : nullptr;
}

// A texture RTGL1 should see in full every frame (canvas, movie).
struct FrameTexture {
   const char *name;
   bool provided = false;
   bool dirty = true;
   std::vector<uint8_t> rgba, scratch;
   int width = 0, height = 0;
   void upload()
   {
      dirty = false;
      if (provided)
         rt.rgMarkOriginalTextureAsDeleted(name);
      auto details = RgOriginalTextureDetailsEXT{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
         .pNext = nullptr, .flags = 0, .format = RG_FORMAT_R8G8B8A8_SRGB,
      };
      auto info = RgOriginalTextureInfo{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
         .pNext = &details,
         .pTextureName = name,
         .pPixels = rgba.data(),
         .size = {uint32_t(width), uint32_t(height)},
         .filter = RG_SAMPLER_FILTER_NEAREST,
         .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
         .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
      };
      provided = rt.rgProvideOriginalTexture(&info) == RG_RESULT_SUCCESS;
   }
};
FrameTexture g_canvasTex{"dark.canvas"};
FrameTexture g_movieTex{"dark.movie"};

// ---- frame state ------------------------------------------------------------

bool g_frameStarted = false;
bool g_inScene = false;
bool g_suppress = true;
bool g_cameraUploaded = false;
bool g_vsync = true;
// Test switch (panel): the scene given to the path tracer stops changing --
// models and lights stay as they were -- while the camera still moves. What
// still changes as you walk is then the path tracer's own doing.
bool g_freeze = false;
// Test switch (panel): faces turned away from the camera, which the game culls,
// are dropped instead of kept for shadows.
bool g_dropBackFaces = false;
// Test switch (panel): how the path tracer reuses light samples between
// pixels and frames. 0 normal, 1 no spatial reuse, 2 no reuse at all,
// 3 normal with 8 shadow rays and 4 light samples per pixel.
int g_reuseTest = 0;
struct PendingLight {
   uint32_t id;
   float pos[3], rgb[3], dir[3];
   float radius, inner, outer, peak, score = 0;
   bool reaches = true;   // shines on a cell near the camera or in view
};
std::vector<PendingLight> g_pendingLights;
uint32_t g_lightsTotal = 0, g_frameLightsShown = 0, g_lightsReaching = 0;
std::vector<PendingLight> g_lastLights;   // last frame's, for the inspector
std::vector<PendingLight> g_lastOffered;  // all the game gave last frame, before culling
std::vector<PendingLight> g_dimPending, g_lastDim;   // too dim to trace (off)
std::vector<PendingLight> g_frozenLights;  // while g_freeze
bool g_underwater = false;
int g_drawableW = 1, g_drawableH = 1;
uint32_t g_framePrims = 0, g_frameDropped = 0, g_frameLights = 0;
// this frame's light positions (meters), for making light fixtures glow
std::vector<RgFloat3D> g_lightPositions;
// the level's ambient light (0..1), and the glow it gives surfaces
float g_levelAmbient = 0.0f;
float ambient_emissive()
{
   // calibrated by eye against the OpenGL renderer's dark corners
   return g_levelAmbient * 0.6f * g_cfg.ambient;
}

struct Camera {
   float pos[3], fwd[3], left[3], up[3];
   float zoom;
   int width, height;
} g_camera;

struct Canvas {
   bool valid = false;
   bool scene = false;
   darkrt::Rect viewport{};
   int width = 0, height = 0;
} g_canvas;

// screen-space draws after the main view, composited before the canvas
struct Overlay {
   std::vector<RgPrimitiveVertex> verts;
   RtTexture *texture;
   int blend;
   int canvasW, canvasH;
   darkrt::Rect viewport;
};
std::vector<Overlay> g_overlays;

std::unordered_map<uint32_t, uint32_t> g_primitiveCounters;

double now_seconds()
{
   using namespace std::chrono;
   return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void print(const char *message, RgMessageSeverityFlags, void *)
{
   fprintf(stderr, "RTGL1: %s\n", message);
}

uint32_t pack_color(float r, float g, float b, float a)
{
   auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
   // RgColor4DPacked32: R in the low byte
   return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

void render_size(int &w, int &h);
void flush_lights();
bool metalfx_denoiser_available()
{
   return rt.rgUtilGetSupportedFeatures &&
          (rt.rgUtilGetSupportedFeatures() & RG_FEATURE_METALFX_DENOISER) != 0;
}

void start_frame()
{
   if (g_frameStarted || !g_active)
      return;
   auto resolution = RgStartFrameRenderResolutionParams{
      .sType = RG_STRUCTURE_TYPE_START_FRAME_RENDER_RESOLUTION_PARAMS,
      .pNext = nullptr,
   };
   switch (g_cfg.upscale) {
   case 1: resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY; break;
   case 2: resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED; break;
   case 3: resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE; break;
   case 4: resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE; break;
   default: break;
   }
   // 0: no upscaler, traced at rt_scale of the window (1 = native) and
   //    stretched; 1..4: MetalFX quality .. ultra performance; 5: MetalFX from
   //    rt_scale of the window.
   // MetalFX's temporal scaler upscales at most 3x per axis
   const float scale = std::clamp(g_cfg.scale, g_cfg.upscale == 5 ? 0.34f : 0.25f, 1.0f);
   const RgExtent2D custom{uint32_t(std::max(1, int(g_drawableW * scale))),
                           uint32_t(std::max(1, int(g_drawableH * scale)))};
   resolution.frameGeneration = RG_FRAME_GENERATION_MODE_OFF;
   resolution.denoiser = RG_DENOISER_DEFAULT;
   const bool metalfx = g_cfg.upscale >= 1 && g_cfg.upscale <= 5;
   // (MetalFX's denoiser: less stable than RTGL1's, and not cheaper)
   const bool mfxDenoiser = false;
   // Dynamic resolution (the Metal backend reads RTGL1_DYNRES_* every frame):
   // the size asked for here is the largest; each frame's follows the GPU time.
   // Not with the MetalFX denoiser, which needs a fixed size.
   const bool dyn = g_cfg.dynres > 0 && !mfxDenoiser;
   {
      static int s_fps = -1;
      const int fps = dyn ? std::clamp(g_cfg.dynres, 20, 240) : 0;
      if (fps != s_fps) {
         s_fps = fps;
         if (fps) {
            char v[16];
            snprintf(v, sizeof(v), "%d", fps);
            setenv("RTGL1_DYNRES_FPS", v, 1);
            setenv("RTGL1_DYNRES_MIN", "0.25", 1);
         } else {
            unsetenv("RTGL1_DYNRES_FPS");
         }
      }
   }
   int maxW, maxH;
   render_size(maxW, maxH);
   if (metalfx) {
      // on Metal the AMD FSR2 slot is the MetalFX temporal scaler
      resolution.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
      resolution.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
      if (g_cfg.upscale == 5) {
         resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
         resolution.customRenderSize = custom;
      }
      if (mfxDenoiser) {
         resolution.denoiser = RG_DENOISER_METALFX;
         resolution.rayReconstruction = 1;
      }
      if (dyn) {
         resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
         resolution.customRenderSize = {uint32_t(maxW), uint32_t(maxH)};
      }
   } else {
      resolution.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_LINEAR;
      resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
      resolution.customRenderSize = custom;
      resolution.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_NONE;
   }
   auto info = RgStartFrameInfo{
      .sType = RG_STRUCTURE_TYPE_START_FRAME_INFO,
      .pNext = &resolution,
      .pMapName = nullptr,
      .ignoreExternalGeometry = true,
      .vsync = g_vsync ? 1u : 0u,
      .hdr = false,
      .allowMapAutoExport = false,
      .lightmapScreenCoverage = 0.0f,
      .lightstyleValuesCount = 0,
      .pLightstyleValues8 = nullptr,
      .pResultStaticSceneStatus = nullptr,
      .staticSceneAnimationTime = 0,
   };
   const double t0 = now_seconds();
   RgResult r = rt.rgStartFrame(&info);
   g_ui.waitSum += now_seconds() - t0;
   g_frameStarted = r == RG_RESULT_SUCCESS;
   if (!g_frameStarted) {
      static int warned = 0;
      if (warned++ < 5)
         fprintf(stderr, "RT: rgStartFrame failed: %s\n",
                 rt.rgUtilGetResultDescription ? rt.rgUtilGetResultDescription(r) : "?");
   }
   g_cameraUploaded = false;
   g_frameLightsShown = g_frameLights;
   g_framePrims = g_frameDropped = g_frameLights = 0;
   g_lightPositions.clear();
   g_pendingLights.clear();
   g_primitiveCounters.clear();
}

RgFloat3D to_rt(const float *p)
{
   return {{p[0] * kMetersPerUnit, p[1] * kMetersPerUnit, p[2] * kMetersPerUnit}};
}

RgTransform identity_transform()
{
   return RgTransform{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
}

bool upload(uint64_t id, const RgMeshPrimitiveInfo &prim, bool firstPerson = false)
{
   auto mesh = RgMeshInfo{
      .sType = RG_STRUCTURE_TYPE_MESH_INFO,
      .pNext = nullptr,
      .flags = firstPerson ? RgMeshInfoFlags(RG_MESH_FIRST_PERSON) : 0u,
      .uniqueObjectID = id,
      .pMeshName = nullptr,
      .transform = identity_transform(),
      .isExportable = false,
      .animationTime = 0.0f,
      .localLightsIntensity = 1.0f,
   };
   if (rt.rgUploadMeshPrimitive(&mesh, &prim) == RG_RESULT_SUCCESS) {
      ++g_framePrims;
      return true;
   }
   ++g_frameDropped;
   return false;
}

// ---- captured objects -------------------------------------------------------

// A triangle list lgd3d drew for the main view, in RTGL1 terms.
struct CapturedPrim {
   std::vector<RgPrimitiveVertex> verts;
   std::string texture;   // RTGL1 name, "" for untextured
   RgMeshPrimitiveFlags flags = 0;
   float emissive = 0.0f;
   // The primitive's colour. RTGL1's path tracer ignores vertex colours, so
   // an untextured part (a model's flat-coloured material) is split by colour
   // and each piece gets its colour here -- otherwise it is white.
   uint32_t color = 0xffffffffu;
   // for the inspector
   const RtTexture *tex = nullptr;
   uint32_t texSerial = 0;
   int blend = 0;
   bool alphaTest = false, sprite = false;
   float avgColor[4] = {};  // the game's vertex colours, before they were replaced
};

// What an object looked like the last time the engine drew it. Objects the
// portal renderer does not draw (out of view) would otherwise vanish from the
// traced scene, taking their shadows -- and the light a lamp's shade blocks --
// with them, so lighting changed with the view direction. An object that has
// not moved since it was last drawn (lamps, furniture, closed doors, idle AI)
// is kept while out of view, for as long as the game says it still exists
// where it was, and it is near enough to the camera to matter.
struct CachedObject {
   std::vector<CapturedPrim> prims;
   uint64_t hash = 0;
   uint32_t frame = 0;
   int unchanged = 0;
   bool posed = false;
   float pose[6] = {};
   float center[3] = {};   // meters
   bool whole = false;     // captured unclipped by the view
   std::vector<RgNormalPacked32> normals;   // of prims' vertices, computed for normalsHash
   uint64_t normalsHash = 0;
};

// cached objects farther than this from the camera are not drawn out of view
constexpr float kKeepDistance = 60.0f;

std::unordered_map<uint32_t, CachedObject> g_objects;
uint32_t g_frameNumber = 1;
struct DroppedObject {
   uint32_t obj, frame;
   const char *why;
};
std::deque<DroppedObject> g_droppedObjects;   // the last few, for the inspector
void note_dropped(uint32_t obj, const char *why)
{
   g_droppedObjects.push_back({obj, g_frameNumber, why});
   while (g_droppedObjects.size() > 12)
      g_droppedObjects.pop_front();
}
uint32_t g_currentObject = 0;
bool g_currentWhole = false;
bool g_currentNoShadow = false;
std::vector<CapturedPrim> g_currentCapture;
bool g_emissiveSpriteNow = false;

uint64_t hash_prims(const std::vector<CapturedPrim> &prims)
{
   uint64_t h = 1469598103934665603ull;
   auto mix = [&h](const void *data, size_t n) {
      const uint8_t *b = static_cast<const uint8_t *>(data);
      for (size_t i = 0; i < n; ++i)
         h = (h ^ b[i]) * 1099511628211ull;
   };
   for (const CapturedPrim &p : prims) {
      for (const RgPrimitiveVertex &v : p.verts) {
         // quantized: lighting jitter in the vertex colours is not a change
         int32_t q[3] = {int32_t(v.position[0] * 256), int32_t(v.position[1] * 256),
                         int32_t(v.position[2] * 256)};
         mix(q, sizeof(q));
      }
      mix(p.texture.data(), p.texture.size());
   }
   return h;
}

// Vertex normals for captured triangles. The game hands over only positions;
// a zero normal decodes in RTGL1 as straight down, and the shader then bends
// that toward each pixel's view direction to keep it plausible -- so models
// were lit by the angle you looked at them from, changing as you walked up to
// them. Smooth across edges up to 60 degrees (round lamp shades, posts), keep
// sharper creases (box corners, the two faces of a thin part) hard.
void compute_normals(CapturedPrim *prims, size_t count)
{
   struct Tri { float n[3]; };
   std::vector<Tri> tris;
   std::unordered_map<uint64_t, std::vector<uint32_t>> at;   // position -> corners
   auto key = [](const float *p) {
      auto q = [](float x) { return uint64_t(uint32_t(int32_t(std::lround(x * 1024.0f))) & 0x1fffffu); };
      return q(p[0]) | (q(p[1]) << 21) | (q(p[2]) << 42);
   };
   std::vector<std::pair<uint32_t, uint32_t>> corners;   // (prim, vertex)
   for (size_t pi = 0; pi < count; ++pi) {
      std::vector<RgPrimitiveVertex> &v = prims[pi].verts;
      for (size_t t = 0; t + 2 < v.size(); t += 3) {
         const float *a = v[t].position, *b = v[t + 1].position, *c = v[t + 2].position;
         const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
         const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
         Tri tri{{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]}};
         tris.push_back(tri);
         for (int k = 0; k < 3; ++k) {
            at[key(v[t + k].position)].push_back(uint32_t(corners.size()));
            corners.emplace_back(uint32_t(pi), uint32_t(t + k));
         }
      }
   }
   auto unit = [](const float *n, float *out) {
      const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (int k = 0; k < 3; ++k)
         out[k] = l > 1e-12f ? n[k] / l : (k == 2 ? 1.0f : 0.0f);
      return l > 1e-12f;
   };
   std::vector<float> unitN(tris.size() * 3);
   for (size_t i = 0; i < tris.size(); ++i)
      unit(tris[i].n, &unitN[i * 3]);
   const float kCos = 0.5f;
   for (size_t ci = 0; ci < corners.size(); ++ci) {
      const size_t ti = ci / 3;
      const float *mine = &unitN[ti * 3];
      RgPrimitiveVertex &dst = prims[corners[ci].first].verts[corners[ci].second];
      float sum[3] = {0, 0, 0};
      for (uint32_t other : at[key(dst.position)]) {
         const size_t oi = other / 3;
         const float *on = &unitN[oi * 3];
         if (oi != ti && mine[0] * on[0] + mine[1] * on[1] + mine[2] * on[2] < kCos)
            continue;
         for (int k = 0; k < 3; ++k)
            sum[k] += tris[oi].n[k];   // area weighted
      }
      float n[3];
      if (!unit(sum, n))
         memcpy(n, mine, sizeof(n));
      dst.normalPacked = rt.rgUtilPackNormal ? rt.rgUtilPackNormal(n[0], n[1], n[2]) : 0;
   }
}

void upload_captured(uint64_t id, const CapturedPrim &p)
{
   // Flat: each face shaded by its own direction. RTGL1 bends a blended
   // normal toward the viewer where it disagrees with the face (to keep
   // reflections out of the surface), so smooth shading changes with the
   // angle a model is seen from -- light that came and went as you moved.
   const RgMeshPrimitiveFlags shading =
      g_cfg.smoothModels ? 0u : RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS);
   auto prim = RgMeshPrimitiveInfo{
      .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
      .pNext = nullptr,
      .flags = p.flags | shading,
      .primitiveIndexInMesh = 0,
      .pVertices = p.verts.data(),
      .vertexCount = uint32_t(p.verts.size()),
      .pIndices = nullptr,
      .indexCount = 0,
      .pTextureName = p.texture.empty() ? nullptr : p.texture.c_str(),
      .textureFrame = 0,
      .color = p.color,
      .emissive = (p.flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_SHADOW_ONLY)) || p.sprite
                     ? p.emissive
                     : std::max(p.emissive, ambient_emissive()),
      .classicLight = 1.0f,
      .emissiveGi = 0.0f,
   };
   upload(id, prim);
}

uint32_t g_cachedDrawn = 0;

void upload_objects()
{
   g_cachedDrawn = 0;
   if (g_freeze) {
      // the test switch: the models exactly as they were when it was set
      for (const auto &e : g_objects)
         for (size_t i = 0; i < e.second.prims.size(); ++i)
            upload_captured(0x500000000ull + (uint64_t(e.first) << 12) + i, e.second.prims[i]);
      return;
   }
   for (auto it = g_objects.begin(); it != g_objects.end();) {
      const uint32_t obj = it->first;
      CachedObject &c = it->second;
      bool draw = c.frame == g_frameNumber;
      if (!draw) {
         float pose[6];
         const RgFloat3D cam = to_rt(g_camera.pos);
         const float dx = c.center[0] - cam.data[0], dy = c.center[1] - cam.data[1],
                     dz = c.center[2] - cam.data[2];
         if (dx * dx + dy * dy + dz * dz > kKeepDistance * kKeepDistance) {
            ++it;   // remembered, but too far away to shadow anything seen
            continue;
         }
         const bool keep = c.posed && RenderRTGameObjectPose(obj, pose) &&
                           std::fabs(pose[0] - c.pose[0]) < 0.01f &&
                           std::fabs(pose[1] - c.pose[1]) < 0.01f &&
                           std::fabs(pose[2] - c.pose[2]) < 0.01f &&
                           pose[3] == c.pose[3] && pose[4] == c.pose[4] && pose[5] == c.pose[5];
         if (!keep) {
            note_dropped(obj, c.posed ? "moved since it was last drawn" : "no position known");
            it = g_objects.erase(it);
            continue;
         }
         draw = true;
         ++g_cachedDrawn;
      }
      for (size_t i = 0; i < c.prims.size(); ++i)
         upload_captured(0x500000000ull + (uint64_t(obj) << 12) + i, c.prims[i]);
      ++it;
   }
}

// ---- world representation -------------------------------------------------

struct WorldGroup {
   int textureId;
   int flags;
   std::vector<RgPrimitiveVertex> verts;
   std::vector<uint32_t> indices;
};
uint32_t g_worldSerial = 0;
bool g_worldValid = false;
std::vector<WorldGroup> g_world;
std::unordered_map<uint64_t, size_t> g_worldIndex;
std::unordered_map<int, RtTexture *> g_worldTextures;

void upload_world()
{
   if (!g_worldValid)
      return;
   static uint32_t s_reported = 0;
   size_t tris = 0, cutTris = 0;
   for (size_t i = 0; i < g_world.size(); ++i) {
      WorldGroup &g = g_world[i];
      if (g.verts.empty() || (g.flags & kRenderRTWorldSky))
         continue;
      auto found = g_worldTextures.find(g.textureId);
      RtTexture *tex = found == g_worldTextures.end() ? nullptr : found->second;
      RgMeshPrimitiveFlags flags = 0;
      // Alpha-tested geometry runs a shader at every hit any ray finds on it:
      // only where the game cuts holes (a colour-keyed texture that has
      // see-through texels: grates, railings, foliage).
      if ((g.flags & kRenderRTWorldTransparent) && tex && tex->cutout)
         flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
      float alpha = 1.0f;
      if (g.flags & kRenderRTWorldWater) {
         flags &= ~RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_ALPHA_TESTED);
         if (g_cfg.water == 1) {
            flags |= RG_MESH_PRIMITIVE_WATER;    // RTGL1's reflective/refractive water
         } else if (g_cfg.water == 0) {
            // Thief's water texture through its medium's palette (portal.c),
            // which carries the water's colour and translucency
            flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
            if (!tex || !tex->hasAlpha)
               alpha = g_cfg.waterAlpha;
         }
      }
      auto prim = RgMeshPrimitiveInfo{
         .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
         .pNext = nullptr,
         .flags = flags,
         .primitiveIndexInMesh = 0,
         .pVertices = g.verts.data(),
         .vertexCount = uint32_t(g.verts.size()),
         .pIndices = g.indices.data(),
         .indexCount = uint32_t(g.indices.size()),
         .pTextureName = texture_name(tex),
         .textureFrame = 0,
         .color = pack_color(1, 1, 1, alpha),
         .emissive = (g.flags & kRenderRTWorldFullbright) ? 0.25f
                     : (flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_SKY)) ? 0.0f
                                                                                      : ambient_emissive(),
         .classicLight = 1.0f,
         .emissiveGi = 0.0f,
      };
      upload(0x100000000ull + i, prim);
      tris += g.indices.size() / 3;
      if (flags & RG_MESH_PRIMITIVE_ALPHA_TESTED)
         cutTris += g.indices.size() / 3;
   }
   if (s_reported != g_worldSerial) {
      s_reported = g_worldSerial;
      fprintf(stderr, "RT: world %zu triangles in %zu groups, %zu of them alpha-tested\n", tris,
              g_world.size(), cutTris);
   }
}

} // namespace

// ============================================================================
// darkrt: called by opengl.cpp

namespace darkrt {

bool Wanted()
{
   const char *v = getenv("THIEF_RT");
   if (!v || !*v || *v == '0')
      return false;

   // the rt folder
   std::vector<std::string> candidates;
   if (const char *dir = getenv("THIEF_RT_DIR"))
      candidates.push_back(dir);
#if defined(__APPLE__)
   char exe[PATH_MAX];
   uint32_t size = sizeof(exe);
   if (_NSGetExecutablePath(exe, &size) == 0) {
      std::string e(exe);
      e = e.substr(0, e.rfind('/'));                    // .../Contents/MacOS
      candidates.push_back(e + "/../Resources/rt");
      candidates.push_back(e + "/../../../rt");          // next to the .app
   }
#endif
   candidates.push_back("rt");
   for (const std::string &c : candidates) {
      struct stat st;
      if (stat((c + "/bin/libRTGL1.dylib").c_str(), &st) == 0) {
         char resolved[PATH_MAX];
         g_rtDir = realpath(c.c_str(), resolved) ? resolved : c;
         return true;
      }
   }
   fprintf(stderr, "RT: THIEF_RT is set but no rt/bin/libRTGL1.dylib was found; using OpenGL\n");
   return false;
}

bool Open(SDL_Window *window)
{
   load_settings();
   // the Metal backend's GPU timeline, every 2 seconds, in the log
   if (env_float("THIEF_RT_PROFILE", 0) != 0)
      setenv("RTGL1_METAL_PROFILE", "1", 1);
   g_cfg.upscale = std::clamp(int(env_float("THIEF_RT_UPSCALE", float(g_cfg.upscale))), 0, 5);
   g_cfg.scale = std::clamp(env_float("THIEF_RT_SCALE", g_cfg.scale), 0.25f, 1.0f);
   g_cfg.light = env_float("THIEF_RT_LIGHT", g_cfg.light);
   g_cfg.sky = env_float("THIEF_RT_SKY", g_cfg.sky);
   g_cfg.flip = env_float("THIEF_RT_FLIP", 0) != 0;
   g_cfg.debug = env_float("THIEF_RT_DEBUG", 0) != 0;
   g_cfg.glow = env_float("THIEF_RT_GLOW", g_cfg.glow);
   g_cfg.lightSize = env_float("THIEF_RT_LIGHT_SIZE", g_cfg.lightSize);
   g_cfg.fps = env_float("THIEF_RT_FPS", g_cfg.fps ? 1.0f : 0.0f) != 0;
   g_ui.hintUntil = -1;

   g_metalView = SDL_Metal_CreateView(window);
   if (!g_metalView) {
      fprintf(stderr, "RT: no Metal view: %s\n", SDL_GetError());
      return false;
   }
   auto metal = RgMetalSurfaceCreateInfo{
      .pLayer = static_cast<const CAMetalLayer *>(SDL_Metal_GetLayer(g_metalView)),
   };
   auto info = RgInstanceCreateInfo{
      .sType = RG_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = nullptr,
      .version = RG_RTGL_VERSION_API,
      .sizeOfRgInterface = sizeof(RgInterface),
      .pAppName = "Thief RT",
      .pAppGUID = "3c9a6f02-8d1b-4b6e-a5f4-7e2d1c0b9a58",
      .pWin32SurfaceInfo = nullptr,
      .pMetalSurfaceCreateInfo = &metal,
      .pWaylandSurfaceCreateInfo = nullptr,
      .pXcbSurfaceCreateInfo = nullptr,
      .pXlibSurfaceCreateInfo = nullptr,
      .pOverrideFolderPath = g_rtDir.c_str(),
      .pfnPrint = print,
      .pUserPrintData = nullptr,
      .allowedMessages = g_cfg.debug
         ? RgMessageSeverityFlags(RG_MESSAGE_SEVERITY_INFO | RG_MESSAGE_SEVERITY_WARNING |
                                  RG_MESSAGE_SEVERITY_ERROR)
         : RgMessageSeverityFlags(RG_MESSAGE_SEVERITY_WARNING | RG_MESSAGE_SEVERITY_ERROR),
      .primaryRaysMaxAlbedoLayers = 1,
      .indirectIlluminationMaxAlbedoLayers = 1,
      .replacementsMaxVertexCount = 1024 * 1024,
      .dynamicMaxVertexCount = 4 * 1024 * 1024,
      .rayCullBackFacingTriangles = 0,
      .allowTexCoordLayer1 = false,
      .allowTexCoordLayer2 = false,
      .allowTexCoordLayer3 = false,
      .lightmapTexCoordLayerIndex = 0,
      .rasterizedMaxVertexCount = 1 << 20,
      .rasterizedMaxIndexCount = 1 << 21,
      .rasterizedVertexColorGamma = true,
      .rasterizedSkyCubemapSize = 256,
      .textureSamplerForceMinificationFilterLinear = true,
      .textureSamplerForceNormalMapFilterLinear = true,
      .pbrTextureSwizzling = RG_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC,
      .effectWipeIsUsed = false,
      .worldUp = {{0, 0, 1}},
      .worldForward = {{1, 0, 0}},
      .worldScale = 1.0f,
      .importedLightIntensityScaleDirectional = 1.0f,
      .importedLightIntensityScaleSphere = 1.0f,
      .importedLightIntensityScaleSpot = 1.0f,
   };
   fprintf(stderr, "RT: loading the path tracer from %s/bin\n", g_rtDir.c_str());
   RgResult r = rgLoadLibraryAndCreate(&info, false, nullptr, &rt, &g_lib);
   if (r != RG_RESULT_SUCCESS) {
      const char *err = r == RG_RESULT_CANT_FIND_DYNAMIC_LIBRARY ? dlerror() : nullptr;
      fprintf(stderr, "RT: RTGL1 failed to start (RgResult %d%s%s); using OpenGL\n", int(r),
              err ? ": " : "", err ? err : "");
      rt = RgInterface{};
      SDL_Metal_DestroyView(g_metalView);
      g_metalView = nullptr;
      return false;
   }
   g_active = true;
   fprintf(stderr, "RT: RTGL1 started (MetalFX mode %d)\n", g_cfg.upscale);
   return true;
}

void Close()
{
   if (g_active && rt.rgDestroyInstance)
      rt.rgDestroyInstance();
   rt = RgInterface{};
   g_active = false;
   if (g_metalView)
      SDL_Metal_DestroyView(g_metalView);
   g_metalView = nullptr;
   g_frameStarted = g_inScene = false;
   g_RenderRTCaptureVertices = 0;
   for (RtTexture &t : g_textures)
      t.provided = false, t.dirty = true;
   g_canvasTex.provided = g_movieTex.provided = false;
   g_ui.fontProvided = false;
}

bool Active() { return g_active; }

void *CreateTexture(int width, int height, const void *rgba, int rowBytes)
{
   if (width <= 0 || height <= 0 || !rgba || rowBytes < width * 4)
      return nullptr;
   RtTexture *t;
   if (!g_freeTextures.empty()) {
      // A freed handle is reused only for a new texture of its own; its old
      // RTGL1 name is retired so world groups never see different pixels
      // under a name they captured.
      t = g_freeTextures.back();
      g_freeTextures.pop_back();
   } else {
      g_textures.emplace_back();
      t = &g_textures.back();
   }
   t->serial = ++g_textureSerial;
   snprintf(t->name, sizeof(t->name), "dark.t%u", t->serial);
   t->provided = false;
   copy_pixels(*t, width, height, rgba, rowBytes);
   return t;
}

int UpdateTexture(void *handle, int width, int height, const void *rgba, int rowBytes)
{
   RtTexture *t = static_cast<RtTexture *>(handle);
   if (!t || width <= 0 || height <= 0 || !rgba || rowBytes < width * 4)
      return 0;
   copy_pixels(*t, width, height, rgba, rowBytes);
   return 1;
}

void DestroyTexture(void *handle)
{
   RtTexture *t = static_cast<RtTexture *>(handle);
   if (!t)
      return;
   for (RtTexture *&b : g_bound)
      if (b == t)
         b = nullptr;
   for (auto &w : g_worldTextures)
      if (w.second == t)
         w.second = nullptr;
   if (t->provided && g_active)
      rt.rgMarkOriginalTextureAsDeleted(t->name);
   t->provided = false;
   t->rgba.clear();
   t->rgba.shrink_to_fit();
   g_freeTextures.push_back(t);
}

void BindTexture(int level, void *texture)
{
   if (level >= 0 && level < 2)
      g_bound[level] = static_cast<RtTexture *>(texture);
}

void SetBlend(int mode) { g_blend = mode; }
void SetAlphaTest(int enabled) { g_alphaTest = enabled != 0; }
void SetFog(int, uint32_t) {}

int Draw(int primitive, const sRenderBackendVertex *v, int count, int canvasW, int canvasH,
         const Rect &viewport)
{
   if (!g_active || !v || count < 3 || primitive != kRenderBackendTriangles)
      return 1;
   if (g_inScene)
      return 1;   // the path-traced scene replaces lgd3d's screen-space view
   Overlay o;
   o.texture = g_bound[0];
   o.blend = g_blend;
   o.canvasW = canvasW;
   o.canvasH = canvasH;
   o.viewport = viewport;
   o.verts.resize(size_t(count));
   for (int i = 0; i < count; ++i) {
      RgPrimitiveVertex &d = o.verts[size_t(i)];
      d = {};
      d.position[0] = v[i].x;
      d.position[1] = v[i].y;
      d.position[2] = 0;
      d.texCoord[0] = v[i].u0;
      d.texCoord[1] = v[i].v0;
      d.color = pack_color(v[i].r, v[i].g, v[i].b, v[i].a);
   }
   g_overlays.push_back(std::move(o));
   return 1;
}

void BeginSceneFrame() {}
void DeactivateScene() {}

void SetCanvas(const void *pixels, int width, int height, int rowBytes, int depth,
               const uint8_t *palette, bool scene, const Rect &viewport)
{
   if (!g_active || !pixels || width <= 0 || height <= 0)
      return;
   FrameTexture &t = g_canvasTex;
   // converted into scratch, uploaded only when it differs from last frame's
   t.scratch.resize(size_t(width) * height * 4);
   for (int y = 0; y < height; ++y) {
      uint8_t *dst = t.scratch.data() + size_t(y) * width * 4;
      const uint8_t *row = static_cast<const uint8_t *>(pixels) + size_t(y) * rowBytes;
      for (int x = 0; x < width; ++x, dst += 4) {
         uint32_t raw;
         uint8_t r, g, b;
         if (depth == 8) {
            raw = row[x];
            r = palette ? palette[raw * 3] : 0;
            g = palette ? palette[raw * 3 + 1] : 0;
            b = palette ? palette[raw * 3 + 2] : 0;
         } else {
            raw = reinterpret_cast<const uint16_t *>(row)[x];
            // the canvas is RGB565 when presented at 16 bits
            r = uint8_t(((raw >> 11) & 31) * 255 / 31);
            g = uint8_t(((raw >> 5) & 63) * 255 / 63);
            b = uint8_t((raw & 31) * 255 / 31);
         }
         dst[0] = r; dst[1] = g; dst[2] = b;
         dst[3] = (scene && raw == 0) ? 0 : 255;
      }
   }
   if (t.width != width || t.height != height || t.scratch != t.rgba) {
      t.rgba.swap(t.scratch);
      t.width = width;
      t.height = height;
      t.dirty = true;
   }
   g_canvas.valid = true;
   g_canvas.scene = scene;
   g_canvas.viewport = viewport;
   g_canvas.width = width;
   g_canvas.height = height;
}

} // namespace darkrt

namespace {

// 2D, in canvas coordinates (0..w, 0..h, y down), drawn into viewport.
void upload_2d(uint64_t id, const RgPrimitiveVertex *verts, uint32_t count,
               const char *texture, bool blend, int canvasW, int canvasH,
               const darkrt::Rect &vp)
{
   // Orthographic projection of the canvas, column-major, into Metal/Vulkan
   // clip space, where y points down: canvas y = 0 is the top of the screen
   const float w = float(std::max(canvasW, 1)), h = float(std::max(canvasH, 1));
   const float proj[16] = {
      2.0f / w, 0, 0, 0,
      0, 2.0f / h, 0, 0,
      0, 0, 1, 0,
      -1, -1, 0, 1,
   };
   static const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
   const RgViewport viewport{vp.x, vp.y, vp.w, vp.h, 0.0f, 1.0f};
   auto swap = RgMeshPrimitiveSwapchainedEXT{
      .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
      .pNext = nullptr,
      .flags = 0,
      .pViewport = &viewport,
      .pView = view,
      .pProjection = proj,
      .pViewProjection = nullptr,
   };
   auto prim = RgMeshPrimitiveInfo{
      .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
      .pNext = &swap,
      .flags = blend ? RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_TRANSLUCENT) : 0u,
      .primitiveIndexInMesh = 0,
      .pVertices = verts,
      .vertexCount = count,
      .pIndices = nullptr,
      .indexCount = 0,
      .pTextureName = texture,
      .textureFrame = 0,
      .color = pack_color(1, 1, 1, 1),
      .emissive = 0,
      .classicLight = 1,
      .emissiveGi = 0,
   };
   upload(id, prim);
}

void quad(RgPrimitiveVertex *v, float x0, float y0, float x1, float y1, uint32_t color)
{
   const float p[6][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x1, y1, 1, 1},
                          {x0, y0, 0, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
   for (int i = 0; i < 6; ++i) {
      v[i] = {};
      v[i].position[0] = p[i][0];
      v[i].position[1] = p[i][1];
      v[i].texCoord[0] = p[i][2];
      v[i].texCoord[1] = p[i][3];
      v[i].color = color;
   }
}


// ---- settings panel, frame counter ------------------------------------------

constexpr const char *kFontName = "dark.font";

void provide_font()
{
   if (g_ui.fontProvided)
      return;
   std::vector<uint8_t> rgba(size_t(rtfont::kWidth) * rtfont::kHeight * 4);
   for (int i = 0; i < rtfont::kWidth * rtfont::kHeight; ++i) {
      const uint8_t nib = (rtfont::kAlpha4[i / 2] >> ((i & 1) * 4)) & 15;
      rgba[size_t(i) * 4 + 0] = rgba[size_t(i) * 4 + 1] = rgba[size_t(i) * 4 + 2] = 255;
      rgba[size_t(i) * 4 + 3] = uint8_t(nib * 17);
   }
   auto details = RgOriginalTextureDetailsEXT{
      .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
      .pNext = nullptr, .flags = 0, .format = RG_FORMAT_R8G8B8A8_UNORM,
   };
   auto info = RgOriginalTextureInfo{
      .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
      .pNext = &details,
      .pTextureName = kFontName,
      .pPixels = rgba.data(),
      .size = {uint32_t(rtfont::kWidth), uint32_t(rtfont::kHeight)},
      .filter = RG_SAMPLER_FILTER_LINEAR,
      .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
      .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
   };
   g_ui.fontProvided = rt.rgProvideOriginalTexture(&info) == RG_RESULT_SUCCESS;
}

// Text in window pixels; '\x7f' is an arrow. Returns the advance.
float text(std::vector<RgPrimitiveVertex> &out, float x, float y, float px, const char *s,
           uint32_t color)
{
   const float cw = px * rtfont::kCellW / rtfont::kCellH;
   const float x0 = x;
   for (; *s; ++s) {
      const int c = (unsigned char)*s;
      if (c > 32 && c <= 127) {
         const int i = c - 32;
         const float u0 = float((i % rtfont::kCols) * rtfont::kCellW) / rtfont::kWidth;
         const float v0 = float((i / rtfont::kCols) * rtfont::kCellH) / rtfont::kHeight;
         const float u1 = u0 + float(rtfont::kCellW) / rtfont::kWidth;
         const float v1 = v0 + float(rtfont::kCellH) / rtfont::kHeight;
         RgPrimitiveVertex q[6];
         quad(q, x, y, x + cw, y + px, color);
         const float uv[6][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v0}, {u1, v1}, {u0, v1}};
         for (int k = 0; k < 6; ++k) {
            q[k].texCoord[0] = uv[k][0];
            q[k].texCoord[1] = uv[k][1];
         }
         out.insert(out.end(), q, q + 6);
      }
      x += cw;
   }
   return x - x0;
}

float text_width(float px, const char *s)
{
   return px * rtfont::kCellW / rtfont::kCellH * float(strlen(s));
}

// The size the scene is traced at.
void render_size(int &w, int &h)
{
   float f;
   switch (g_cfg.upscale) {
   case 1: f = 1.0f / 1.5f; break;
   case 2: f = 1.0f / 1.7f; break;
   case 3: f = 0.5f; break;
   case 4: f = 1.0f / 3.0f; break;
   case 5: f = std::clamp(g_cfg.scale, 0.34f, 1.0f); break;
   default: f = std::clamp(g_cfg.scale, 0.25f, 1.0f); break;
   }
   w = std::max(1, int(g_drawableW * f));
   h = std::max(1, int(g_drawableH * f));
}

const char *kUpscaleNames[] = {"Off",
                               "MetalFX Quality",
                               "MetalFX Balanced",
                               "MetalFX Performance",
                               "MetalFX Ultra Performance",
                               "MetalFX Custom"};

enum UiRow {
   kRowUpscale, kRowScale, kRowDynres, kRowBounces, kRowLights, kRowLightCull, kRowFlicker, kRowVolumetric, kRowVsync,
   kRowFps, kRowLight, kRowAmbient, kRowSky, kRowLightSize, kRowRough, kRowSmooth, kRowGlow, kRowWater,
   kRowDebug, kRowFreeze, kRowBackFaces, kRowReuse, kRowInspect, kRowReset, kRowCount
};

const char *kRowLabels[kRowCount] = {
   "Upscaler", "Render resolution", "Dynamic resolution", "Bounce light", "Lights traced",
   "Lights behind walls", "Light flicker",
   "Volumetric light", "VSync", "FPS counter", "Light brightness", "Ambient light", "Sky brightness",
   "Light size (soft shadows)", "Surface roughness", "Model shading", "Lamp glow", "Water",
   "Debug view", "Freeze scene (test)", "Back faces (test)", "Light sampling (test)", "Inspect centre of view", "Reset to defaults",
};

// rows that do something when chosen rather than having a value
bool row_is_action(int row) { return row == kRowInspect || row == kRowReset; }

std::string row_value(int row)
{
   char buf[96];
   int w, h;
   switch (row) {
   case kRowUpscale: return kUpscaleNames[std::clamp(g_cfg.upscale, 0, 5)];
   case kRowScale:
      render_size(w, h);
      if (g_cfg.upscale >= 1 && g_cfg.upscale <= 4)
         snprintf(buf, sizeof(buf), "%dx%d (preset)", w, h);
      else
         snprintf(buf, sizeof(buf), "%d%%  %dx%d", int(std::lround(g_cfg.scale * 100)), w, h);
      return buf;
   case kRowDynres:
      if (g_cfg.dynres <= 0) return "Off";
      snprintf(buf, sizeof(buf), "aim for %d fps", g_cfg.dynres);
      return buf;
   case kRowBounces:
      return g_cfg.gi == 0 ? "2, half rate" : g_cfg.gi == 1 ? "1" : g_cfg.gi == 2 ? "2" : "1, half rate";
   case kRowLights:
      if (g_cfg.maxLights <= 0) return "All";
      snprintf(buf, sizeof(buf), "nearest %d", g_cfg.maxLights);
      return buf;
   case kRowLightCull: return g_cfg.lightCull ? "Skipped" : "Traced";
   case kRowFlicker: return g_cfg.flicker == 1 ? "As the game" : g_cfg.flicker == 2 ? "Nearly steady" : "Smoothed";
   case kRowVolumetric: return g_cfg.volumetric ? "On" : "Off";
   case kRowVsync: return g_cfg.vsync ? "On" : "Off";
   case kRowFps: return g_cfg.fps ? "On" : "Off";
   case kRowLight: snprintf(buf, sizeof(buf), "%.2fx", g_cfg.light); return buf;
   case kRowAmbient:
      if (g_cfg.ambient <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2fx the level's", g_cfg.ambient);
      return buf;
   case kRowSky: snprintf(buf, sizeof(buf), "%.2fx", g_cfg.sky); return buf;
   case kRowLightSize: snprintf(buf, sizeof(buf), "%.2f m", g_cfg.lightSize); return buf;
   case kRowSmooth: return g_cfg.smoothModels ? "Smooth" : "Flat (steady)";
   case kRowFreeze: return g_freeze ? "On (frozen)" : "Off";
   case kRowBackFaces: return g_dropBackFaces ? "Dropped" : "Shadows only";
   case kRowReuse: {
      static const char *kReuse[] = {"Normal", "No spatial reuse", "No reuse", "8 shadow rays"};
      return kReuse[std::clamp(g_reuseTest, 0, 3)];
   }
   case kRowDebug: {
      static const char *kViews[] = {"Off", "Shadows only", "Direct light only", "Glossy only",
                                     "Bounce light only"};
      return kViews[std::clamp(g_cfg.debugView, 0, 4)];
   }
   case kRowRough:
      snprintf(buf, sizeof(buf), g_cfg.minRoughness >= 0.999f ? "%.2f (matte)" : "at least %.2f",
               g_cfg.minRoughness);
      return buf;
   case kRowGlow:
      if (g_cfg.glow <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2f", g_cfg.glow);
      return buf;
   case kRowWater:
      return g_cfg.water == 1 ? "Reflective (RTGL1)" : g_cfg.water == 2 ? "Opaque" : "Translucent";
   default: return "";
   }
}

float step_mul(float v, int dir, float lo, float hi)
{
   // multiplicative steps that land on round numbers: 1, 1.25, 1.5, 2, 2.5, 3, 4 ...
   static const float steps[] = {0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.33f, 0.5f, 0.6f, 0.75f,
                                 0.85f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f,
                                 5.0f, 6.0f, 8.0f, 10.0f, 15.0f, 20.0f};
   const int n = int(sizeof(steps) / sizeof(steps[0]));
   int i = 0;
   while (i < n - 1 && steps[i] < v - 1e-3f)
      ++i;
   if (dir > 0 && steps[i] <= v + 1e-3f) ++i;
   if (dir < 0) --i;
   return std::clamp(steps[std::clamp(i, 0, n - 1)], lo, hi);
}

void inspect_view();

void change_row(int row, int dir)
{
   static const int kDynres[] = {0, 30, 40, 50, 60, 72, 90, 120};
   switch (row) {
   case kRowUpscale: g_cfg.upscale = (g_cfg.upscale + dir + 6) % 6; break;
   case kRowScale: {
      if (g_cfg.upscale >= 1 && g_cfg.upscale <= 4) {
         // leave the preset for a custom scale starting where the preset was
         int w, h;
         render_size(w, h);
         g_cfg.scale = float(w) / float(std::max(g_drawableW, 1));
         g_cfg.upscale = 5;
      }
      const float lo = g_cfg.upscale == 5 ? 0.35f : 0.25f;
      const float s = std::round(g_cfg.scale * 20.0f) / 20.0f + 0.05f * dir;
      g_cfg.scale = std::clamp(s, lo, 1.0f);
      break;
   }
   case kRowDynres: {
      const int n = int(sizeof(kDynres) / sizeof(kDynres[0]));
      int i = 0;
      while (i < n - 1 && kDynres[i] < g_cfg.dynres)
         ++i;
      g_cfg.dynres = kDynres[(i + dir + n) % n];
      break;
   }
   case kRowBounces: g_cfg.gi = (g_cfg.gi + dir + 4) % 4; break;
   case kRowLights: {
      static const int kSteps[] = {16, 32, 48, 64, 96, 128, 256, 0};
      const int n = int(sizeof(kSteps) / sizeof(kSteps[0]));
      int i = 0;
      while (i < n - 1 && kSteps[i] != g_cfg.maxLights)
         ++i;
      g_cfg.maxLights = kSteps[(i + dir + n) % n];
      break;
   }
   case kRowLightCull: g_cfg.lightCull = !g_cfg.lightCull; break;
   case kRowSmooth: g_cfg.smoothModels = !g_cfg.smoothModels; break;
   case kRowFlicker: g_cfg.flicker = (g_cfg.flicker + dir + 3) % 3; break;
   case kRowVolumetric: g_cfg.volumetric = !g_cfg.volumetric; break;
   case kRowVsync: g_cfg.vsync = !g_cfg.vsync; break;
   case kRowFps: g_cfg.fps = !g_cfg.fps; break;
   case kRowLight: g_cfg.light = step_mul(g_cfg.light, dir, 0.05f, 20.0f); break;
   case kRowAmbient: g_cfg.ambient = g_cfg.ambient <= 0.0f && dir > 0 ? 0.25f
                                    : (g_cfg.ambient <= 0.25f && dir < 0 ? 0.0f
                                                                         : step_mul(g_cfg.ambient, dir, 0.25f, 20.0f));
      break;
   case kRowSky: g_cfg.sky = g_cfg.sky <= 0.0f && dir > 0 ? 0.05f
                             : (g_cfg.sky <= 0.05f && dir < 0 ? 0.0f
                                                              : step_mul(g_cfg.sky, dir, 0.05f, 20.0f));
      break;
   case kRowLightSize:
      g_cfg.lightSize = std::clamp(std::round(g_cfg.lightSize * 20.0f) / 20.0f + 0.05f * dir,
                                   0.05f, 1.5f);
      break;
   case kRowDebug: g_cfg.debugView = (g_cfg.debugView + dir + 5) % 5; return;   // not saved
   case kRowBackFaces: g_dropBackFaces = !g_dropBackFaces; return;   // not saved
   case kRowReuse: g_reuseTest = (g_reuseTest + dir + 4) % 4; return;   // not saved
   case kRowFreeze:
      g_freeze = !g_freeze;
      if (g_freeze)
         g_frozenLights = g_lastLights;
      return;   // not saved
   case kRowRough:
      g_cfg.minRoughness = std::clamp(std::round(g_cfg.minRoughness * 20.0f) / 20.0f + 0.05f * dir,
                                      0.0f, 1.0f);
      break;
   case kRowGlow:
      g_cfg.glow = std::clamp(std::round(g_cfg.glow * 4.0f) / 4.0f + 0.25f * dir, 0.0f, 5.0f);
      break;
   case kRowWater: g_cfg.water = (g_cfg.water + dir + 3) % 3; break;
   case kRowInspect: inspect_view(); return;
   case kRowReset: {
      const bool debug = g_cfg.debug, flip = g_cfg.flip;
      g_cfg = kDefaults;
      g_cfg.debug = debug;
      g_cfg.flip = flip;
      break;
   }
   default: break;
   }
   save_settings();
}

void count_frame(bool scene)
{
   const double t = now_seconds();
   if (g_ui.lastPresent > 0) {
      const double ms = (t - g_ui.lastPresent) * 1000.0;
      g_ui.windowWorst = std::max(g_ui.windowWorst, ms);
   }
   g_ui.lastPresent = t;
   if (g_ui.windowStart <= 0)
      g_ui.windowStart = t;
   ++g_ui.windowFrames;
   if (t - g_ui.windowStart >= 0.5) {
      g_ui.fps = g_ui.windowFrames / (t - g_ui.windowStart);
      g_ui.frameMs = 1000.0 / std::max(g_ui.fps, 1e-3);
      g_ui.worstMs = g_ui.windowWorst;
      g_ui.waitMs = g_ui.waitSum * 1000.0 / g_ui.windowFrames;
      g_ui.drawMs = g_ui.drawSum * 1000.0 / g_ui.windowFrames;
      g_ui.waitSum = g_ui.drawSum = 0;
      g_ui.windowStart = t;
      g_ui.windowFrames = 0;
      g_ui.windowWorst = 0;
   }
   if (scene && g_ui.hintUntil < 0)
      g_ui.hintUntil = t + 8.0;
}

void draw_ui(bool scene)
{
   const bool hint = scene && g_ui.hintUntil > now_seconds();
   if (!g_cfg.fps && !g_ui.open && !hint)
      return;
   provide_font();
   if (!g_ui.fontProvided)
      return;
   const darkrt::Rect full{0, 0, float(g_drawableW), float(g_drawableH)};
   const float px = std::max(14.0f, std::round(g_drawableH / 42.0f));
   const float pad = std::round(px * 0.5f);
   const uint32_t white = pack_color(1, 1, 1, 1);
   const uint32_t dim = pack_color(0.75f, 0.75f, 0.72f, 1);
   const uint32_t gold = pack_color(1.0f, 0.82f, 0.45f, 1);
   std::vector<RgPrimitiveVertex> bg, fg;
   auto panel = [&](float x0, float y0, float x1, float y1, float a) {
      RgPrimitiveVertex q[6];
      quad(q, x0, y0, x1, y1, pack_color(0.02f, 0.02f, 0.03f, a));
      bg.insert(bg.end(), q, q + 6);
   };

   if (g_cfg.fps) {
      int w, h;
      render_size(w, h);
      char line[128], line2[128];
      if (g_cfg.dynres > 0)
         snprintf(line, sizeof(line), "%3.0f fps  %5.1f ms (worst %5.1f)  dynamic \x7f%dx%d",
                  g_ui.fps, g_ui.frameMs, g_ui.worstMs, g_drawableW, g_drawableH);
      else
         snprintf(line, sizeof(line), "%3.0f fps  %5.1f ms (worst %5.1f)  %dx%d\x7f%dx%d",
                  g_ui.fps, g_ui.frameMs, g_ui.worstMs, w, h, g_drawableW, g_drawableH);
      // waiting in rgStartFrame is the GPU still busy with earlier frames
      const double game = std::max(0.0, g_ui.frameMs - g_ui.waitMs - g_ui.drawMs);
      snprintf(line2, sizeof(line2), "game %4.1f ms   rt submit %4.1f ms   gpu wait %4.1f ms   lights %u/%u/%u",
               game, g_ui.drawMs, g_ui.waitMs, g_frameLightsShown, g_lightsReaching, g_lightsTotal);
      const float s = std::round(px * 0.8f);
      const float tw = std::max(text_width(s, line), text_width(s, line2));
      panel(0, 0, tw + 2 * pad, 2 * s + pad * 1.5f, 0.6f);
      text(fg, pad, pad * 0.5f, s, line, white);
      text(fg, pad, pad * 0.5f + s * 1.1f, s, line2, dim);
   }

   if (hint && !g_ui.open) {
      const char *msg = "Cmd+,  or  F10: ray tracing settings";
      const float s = std::round(px * 0.8f);
      const float tw = text_width(s, msg);
      const float x = (g_drawableW - tw) * 0.5f, y = g_drawableH - s * 3.0f;
      panel(x - pad, y - pad * 0.5f, x + tw + pad, y + s + pad * 0.5f, 0.55f);
      text(fg, x, y, s, msg, dim);
   }

   if (g_ui.open) {
      const float rowH = px * 1.5f;
      const float labelW = text_width(px, "Light size (soft shadows)") + px * 2.0f;
      const float valueW = text_width(px, "< MetalFX (not on this Mac) >") + px;
      const float w = labelW + valueW + 4 * pad + px * 2.0f;
      const float h = rowH * (kRowCount + 4.5f) + 2 * pad;
      const float x0 = std::round((g_drawableW - w) * 0.5f);
      const float y0 = std::round((g_drawableH - h) * 0.5f);
      panel(x0, y0, x0 + w, y0 + h, 0.82f);
      float y = y0 + pad + rowH * 0.25f;
      text(fg, x0 + 2 * pad, y, px, "Ray tracing settings", gold);
      y += rowH * 1.5f;
      for (int r = 0; r < kRowCount; ++r) {
         const bool sel = r == g_ui.row;
         if (sel)
            panel(x0 + pad, y - rowH * 0.18f, x0 + w - pad, y + px + rowH * 0.18f, 0.9f);
         const uint32_t c = sel ? white : dim;
         text(fg, x0 + 2 * pad, y, px, kRowLabels[r], c);
         std::string v = row_value(r);
         if (sel && !row_is_action(r))
            v = "< " + v + " >";
         text(fg, x0 + 2 * pad + labelW, y, px, v.c_str(), sel ? gold : c);
         y += rowH;
      }
      y += rowH * 0.5f;
      const float s = std::round(px * 0.75f);
      text(fg, x0 + 2 * pad, y, s, "Up/Down: choose   Left/Right: change   Esc: close", dim);
      if (!g_ui.status.empty() && now_seconds() < g_ui.statusUntil)
         text(fg, x0 + 2 * pad, y + rowH, s, g_ui.status.c_str(), gold);
   }

   upload_2d(0x2ffffff10ull, bg.data(), uint32_t(bg.size()), nullptr, true, g_drawableW,
             g_drawableH, full);
   if (!fg.empty())
      upload_2d(0x2ffffff11ull, fg.data(), uint32_t(fg.size()), kFontName, true, g_drawableW,
                g_drawableH, full);
}

void draw_frame(bool sceneDrawn)
{
   double current = now_seconds();
   // Thief's skies are night skies. RTGL1's default sky colour is a bright
   // grey that lit every outdoor surface like daylight. Linking this struct
   // replaces all of RTGL1's sky defaults, so every field is set.
   // RTGL1 uses its defaults only for a struct that is not linked, so the
   // linked ones repeat its defaults (DrawFrameInfo.h) and change only ours.
   auto refl = RgDrawFrameReflectRefractParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_REFLECT_REFRACT_PARAMS,
      .pNext = nullptr,
      .maxReflectRefractDepth = 2,
      .typeOfMediaAroundCamera = g_underwater ? RG_MEDIA_TYPE_WATER : RG_MEDIA_TYPE_VACUUM,
      .indexOfRefractionGlass = 1.52f,
      .indexOfRefractionWater = 1.33f,
      .thinMediaWidth = 0.1f,
      .waterWaveSpeed = 0.4f,
      .waterWaveNormalStrength = 0.6f,
      // Thief's water: a murky green-blue
      .waterColor = {{0.22f, 0.42f, 0.38f}},
      .acidColor = {{0.0f, 0.66f, 0.55f}},
      .acidDensity = 10.0f,
      .waterWaveTextureDerivativesMultiplier = 1.0f,
      .waterTextureAreaScale = 1.0f,
      .portalNormalTwirl = false,
      .stylizedWaterStrength = 0.0f,
      .stylizedWaterCaustic = 1.5f,
      .stylizedWaterReflMax = 0.75f,
      .stylizedWaterRoughness = 0.10f,
      .stylizedWaterGlow = 0.04f,
      .stylizedWaterVeinRef = 0.1f,
      .stylizedLiquidTint = {{{1 / 255.f, 1 / 255.f, 15 / 255.f}},
                             {{1 / 255.f, 5 / 255.f, 1 / 255.f}},
                             {{9 / 255.f, 3 / 255.f, 1 / 255.f}},
                             {{20 / 255.f, 1 / 255.f, 1 / 255.f}}},
      .stylizedLiquidCrest = {{{0.55f, 0.80f, 1.00f}},
                              {{50 / 255.f, 150 / 255.f, 50 / 255.f}},
                              {{120 / 255.f, 78 / 255.f, 38 / 255.f}},
                              {{1.00f, 0.45f, 0.40f}}},
      .stylizedLiquidRelief = {0.0f, 0.0f, 1.0f, 1.0f},
      .stylizedLiquidFlow = {0.0f, 0.0f, 0.0f, 1.0f},
      .stylizedLiquidRefl = {1.0f, 1.0f, 0.0f, 1.0f},
      .stylizedLiquidRough = {0.0f, 0.0f, 0.80f, 0.0f},
      .liquidNoSplit = 0.0f,
      .stylizedLiquidCaustics = {1.0f, 0.0f, 0.0f, 0.0f},
      .liquidFlowSpeed = 0.5f,
      .liquidFlowScale = 6.0f,
      .liquidFlowAspect = 3.0f,
      .liquidFlowDebug = 0.0f,
      .lavaEmisBoost = 6.0f,
      .lavaFlowStrength = 0.45f,
      .lavaFlowSpeed = 0.03f,
      .lavaFlowScale = 0.12f,
      .lavaFlowPixel = 0.25f,
      .lavaPulse = 0.10f,
      .lavaPulseSpeed = 0.35f,
      .lavaGiBoost = 1.0f,
      .lavaDebug = 0.0f,
      .lavaTint = {{1.0f, 0.55f, 0.30f}},
      .stylizedWaterDebug = 0.0f,
      .stylizedWaterReflMin = 0.1f,
      .waterCausticGain = 1.2f,
      .waterCausticScale = 0.8f,
      .waterCausticSpeed = 0.35f,
      .waterCausticDist = 6.0f,
      .waterCausticRise = 2.0f,
      .waterCausticSlant = 0.6f,
      .waterCausticWallBoost = 4.0f,
   };
   auto volumetric = RgDrawFrameVolumetricParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_PARAMS,
      .pNext = &refl,
      // RTGL1's default is on, with a bright ambient haze: costly, and it
      // greyed Thief's darkness. Optional, lit by the lamps when on.
      .enable = g_cfg.volumetric,
      .maxHistoryLength = 8.0f,
      .useSimpleDepthBased = false,
      .volumetricFar = 40.0f,
      .ambientColor = {{0.004f, 0.005f, 0.008f}},
      .scaterring = 0.3f,
      .assymetry = 0.6f,
      .useIlluminationVolume = false,
      .fallbackSourceColor = {{0, 0, 0}},
      .fallbackSourceDirection = {{0, 0, -1}},
      .lightMultiplier = 1.0f,
      .illuminateFromAllLights = true,
      .mediaColor = {{1.0f, 1.0f, 1.0f}},
      .mediaColorFar = {{1.0f, 1.0f, 1.0f}},
      .farScattering = 0.3f,
      .densityCurve = 1.0f,
      .occludeEmission = false,
      .ditherRadius = 2.0f,
      .ditherRadiusZ = 1.0f,
      .spatialBlur = 1.0f,
      .lightNearFade = 0.3f,
      .depthGate = 0.0f,
      .depthGateBias = 0.0f,
      .depthGateFeather = 1.0f,
      .depthGateTaps = 5,
   };
   int renderW = 0, renderH = 0;
   render_size(renderW, renderH);
   const float spatialRadius = std::clamp(30.0f * float(renderW) / 1152.0f, 30.0f, 80.0f);
   auto illum = RgDrawFrameIlluminationParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS,
      .pNext = &volumetric,
      .maxBounceShadows = 2,
      .indirectBounces = (g_cfg.gi == 1 || g_cfg.gi == 3) ? 1u : 2u,
      .indirectLegacyBounceWeight = true,
      .cellWorldSize = 1.0f,
      .directDiffuseSensitivityToChange = 0.5f,
      .indirectDiffuseSensitivityToChange = 0.2f,
      .specularSensitivityToChange = 0.5f,
      .polygonalLightSpotlightFactor = 2.0f,
      .lightUniqueIdIgnoreFirstPersonViewerShadows = nullptr,
      .enableRrTemporalPrefilter = false,
      .rrFireflyThreshold = 0.0f,
      .rrFireflyMinLum = 0.01f,
      .restirBlueNoise = true,
      .shadowSamples = g_reuseTest == 3 ? 8u : 1u,
      .debugRestirM = 0,
      // the panel's debug view: which part of the lighting is what
      .debugVisibility = g_cfg.debugView == 1 ? 1u : 0u,
      .debugShowFlags = g_cfg.debugView == 2 ? 32u : g_cfg.debugView == 3 ? 64u
                        : g_cfg.debugView == 4 ? 128u : 0u,
      .restirTemporalJitter = g_reuseTest == 2 ? 0.0f : 2.0f,
      .rrSpecularHitDistance = true,
      .directSamples = g_reuseTest == 3 ? 4u : 1u,
      // 0: bounce light traced for half of the pixels each frame
      .indirectSamples = (g_cfg.gi == 0 || g_cfg.gi == 3) ? 0u : 1u,
      // more candidate lights per pixel (no extra rays): Thief has many small lights
      .restirInitialSamples = 12,
      .restirSpatialSamples = (g_reuseTest == 1 || g_reuseTest == 2) ? 0u : 8u,
      // in pixels: 30 at ~1150 wide (Blood RT's), wider at higher resolutions so
      // neighbours are taken from as far across the surface as there
      .restirSpatialRadius = spatialRadius,
      .restirTemporalMCap = g_reuseTest == 2 ? 1u : 20u,
      .rrGuideMin = 0.01f,
      .rrGuideMode = 1,
      .restirIndirAntilag = 1,
      .rrPreExposure = true,
      .rrPreExposureDebug = false,
      // A light that goes out counts as a change for the denoiser, and the
      // bounce light keeps at most ~1 s of history (RTGL1's own is 256 frames,
      // and it held on to bright bounce on purpose): a doused torch's glow on
      // the walls goes with it instead of lingering for seconds.
      .svgfFpGrad = true,
      .svgfIndirMaxHist = 64.0f,
      .svgfIndirAntilag = true,
   };
   // No surface glossier than 0.7 (Blood RT's rt_min_roughness): a glossy
   // surface's reflection is one noisy sample per pixel, the crawling.
   auto textures = RgDrawFrameTexturesParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_TEXTURES_PARAMS,
      .pNext = &illum,
      .dynamicSamplerFilter = RG_SAMPLER_FILTER_LINEAR,
      .mipLodBiasOffset = 0.0f,
      .normalMapStrength = 1.0f,
      .emissionMapBoost = 100.0f,
      .emissionMaxScreenColor = 1.5f,
      .minRoughness = g_cfg.minRoughness,
      .heightMapDepth = 0.02f,
   };
   auto sky = RgDrawFrameSkyParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS,
      .pNext = &textures,
      .skyType = RG_SKY_TYPE_COLOR,
      .skyColorDefault = {{0.035f, 0.045f, 0.075f}},
      .skyColorMultiplier = g_cfg.sky,
      .skyColorSaturation = 1.0f,
      .skyViewerPosition = {{0, 0, 0}},
      .pSkyCubemapTextureName = nullptr,
      .skyCubemapRotationTransform = {},
      .sunRequireSky = 0.0f,
      .sunLeakDebug = 0.0f,
      .sunLeakDebugMul = 1.0f,
      .sunSkyProbeMaxDist = 10000.0f,
      .sunSplit = 0.0f,
   };
   auto info = RgDrawFrameInfo{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_INFO,
      .pNext = &sky,
      .rayLength = 2000.0f,
      .disableRayTracedGeometry = sceneDrawn ? 0u : 1u,
      .disableRasterization = false,
      .presentPrevFrame = false,
      .resetHistory = false,
      .currentTime = current,
   };
   if (!sceneDrawn) {
      // RTGL1 wants a camera every frame
      auto cam = RgCameraInfo{
         .sType = RG_STRUCTURE_TYPE_CAMERA_INFO,
         .pNext = nullptr,
         .flags = 0,
         .position = {{0, 0, 0}},
         .up = {{0, 0, 1}},
         .right = {{0, -1, 0}},
         .fovYRadians = 1.2f,
         .aspect = float(g_drawableW) / float(std::max(g_drawableH, 1)),
         .cameraNear = 0.05f,
         .cameraFar = 1000.0f,
         .pView = nullptr,
      };
      rt.rgUploadCamera(&cam);
   }
   const double t0 = now_seconds();
   RgResult r = rt.rgDrawFrame(&info);
   g_ui.drawSum += now_seconds() - t0;
   if (r != RG_RESULT_SUCCESS) {
      static int warned = 0;
      if (warned++ < 5)
         fprintf(stderr, "RT: rgDrawFrame failed: %s\n",
                 rt.rgUtilGetResultDescription ? rt.rgUtilGetResultDescription(r) : "?");
   }
   g_frameStarted = false;

   if (g_cfg.debug) {
      static double last = 0;
      if (current - last > 2.0) {
         last = current;
         fprintf(stderr, "RT: %u primitives, %u dropped, %u lights, scene %d, world %zu groups\n",
                 g_framePrims, g_frameDropped, g_frameLights, int(sceneDrawn), g_world.size());
      }
   }
}

} // namespace

namespace darkrt {

void Present(int drawableWidth, int drawableHeight, bool vsync)
{
   if (!g_active)
      return;
   g_vsync = vsync && g_cfg.vsync;
   g_drawableW = std::max(drawableWidth, 1);
   g_drawableH = std::max(drawableHeight, 1);
   start_frame();
   if (!g_frameStarted) {
      g_overlays.clear();
      return;
   }
   const bool scene = g_cameraUploaded;
   count_frame(scene);

   // lgd3d's 2D draws after the main view
   uint64_t id = 0x200000000ull;
   for (Overlay &o : g_overlays) {
      upload_2d(id++, o.verts.data(), uint32_t(o.verts.size()), texture_name(o.texture),
                o.blend != kRenderBackendBlendOpaque || (o.texture && o.texture->hasAlpha),
                o.canvasW, o.canvasH, o.viewport);
   }
   g_overlays.clear();

   if (g_canvas.valid) {
      const Rect &vp = g_canvas.viewport;
      // black around the canvas: the scene is traced across the whole window
      RgPrimitiveVertex bars[24];
      const uint32_t black = pack_color(0, 0, 0, 1);
      const Rect full{0, 0, float(g_drawableW), float(g_drawableH)};
      // in window pixels, projected with the window as the "canvas"
      quad(bars + 0, 0, 0, full.w, vp.y, black);
      quad(bars + 6, 0, vp.y + vp.h, full.w, full.h, black);
      quad(bars + 12, 0, vp.y, vp.x, vp.y + vp.h, black);
      quad(bars + 18, vp.x + vp.w, vp.y, full.w, vp.y + vp.h, black);
      upload_2d(0x2ffffff00ull, bars, 24, nullptr, false, g_drawableW, g_drawableH, full);

      if (g_canvasTex.dirty || !g_canvasTex.provided)
         g_canvasTex.upload();
      RgPrimitiveVertex q[6];
      quad(q, 0, 0, float(g_canvas.width), float(g_canvas.height), pack_color(1, 1, 1, 1));
      upload_2d(0x2ffffff01ull, q, 6, g_canvasTex.provided ? g_canvasTex.name : nullptr,
                g_canvas.scene, g_canvas.width, g_canvas.height, vp);
   }
   draw_ui(scene);
   draw_frame(scene);
   g_canvas.valid = false;
}

} // namespace darkrt

namespace {

// ---- inspector: what is at the centre of the view, written to logs/ --------

bool ray_triangle(const float o[3], const float d[3], const float *a, const float *b,
                  const float *c, float &t)
{
   const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
   const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
   const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                       d[0] * e2[1] - d[1] * e2[0]};
   const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
   if (std::fabs(det) < 1e-9f)
      return false;
   const float inv = 1.0f / det;
   const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
   const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
   if (u < 0 || u > 1)
      return false;
   const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                       s[0] * e1[1] - s[1] * e1[0]};
   const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
   if (v < 0 || u + v > 1)
      return false;
   t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
   return t > 0.01f;
}

std::string logs_dir()
{
   return g_rtDir.empty() ? std::string("logs") : g_rtDir + "/../logs";
}

// The texture as seen by the ray tracer, as a PPM and its alpha as a PGM.
void dump_texture(const RtTexture *t, const std::string &base)
{
   if (!t || t->rgba.empty())
      return;
   FILE *f = fopen((base + ".ppm").c_str(), "wb");
   FILE *a = fopen((base + "-alpha.pgm").c_str(), "wb");
   if (f) fprintf(f, "P6\n%d %d\n255\n", t->width, t->height);
   if (a) fprintf(a, "P5\n%d %d\n255\n", t->width, t->height);
   for (size_t i = 0; i < size_t(t->width) * t->height; ++i) {
      if (f) fwrite(&t->rgba[i * 4], 1, 3, f);
      if (a) fwrite(&t->rgba[i * 4 + 3], 1, 1, a);
   }
   if (f) fclose(f);
   if (a) fclose(a);
}

void inspect_view()
{
   const RgFloat3D o3 = to_rt(g_camera.pos);
   const float o[3] = {o3.data[0], o3.data[1], o3.data[2]};
   float d[3] = {g_camera.fwd[0], g_camera.fwd[1], g_camera.fwd[2]};
   const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
   for (float &x : d)
      x /= std::max(len, 1e-6f);

   float best = 1e30f;
   uint32_t bestObj = 0;
   size_t bestPrim = 0, bestGroup = SIZE_MAX;
   for (const auto &e : g_objects)
      for (size_t i = 0; i < e.second.prims.size(); ++i) {
         const auto &v = e.second.prims[i].verts;
         for (size_t k = 0; k + 2 < v.size(); k += 3) {
            float t;
            if (ray_triangle(o, d, v[k].position, v[k + 1].position, v[k + 2].position, t) &&
                t < best) {
               best = t, bestObj = e.first, bestPrim = i, bestGroup = SIZE_MAX;
            }
         }
      }
   for (size_t g = 0; g < g_world.size(); ++g) {
      const WorldGroup &w = g_world[g];
      for (size_t k = 0; k + 2 < w.indices.size(); k += 3) {
         float t;
         if (ray_triangle(o, d, w.verts[w.indices[k]].position, w.verts[w.indices[k + 1]].position,
                          w.verts[w.indices[k + 2]].position, t) &&
             t < best) {
            best = t, bestObj = 0, bestGroup = g;
         }
      }
   }

   static int n = 0;
   ++n;
   const std::string dir = logs_dir();
   mkdir(dir.c_str(), 0755);
   FILE *log = fopen((dir + "/rt-inspect.txt").c_str(), "a");
   auto out = [&](const char *fmt, ...) {
      char buf[512];
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(buf, sizeof(buf), fmt, ap);
      va_end(ap);
      fputs(buf, stderr);
      if (log)
         fputs(buf, log);
   };
   out("\n=== RT inspect #%d: camera (%.2f %.2f %.2f) m, looking (%.3f %.3f %.3f)\n", n, o[0], o[1],
       o[2], d[0], d[1], d[2]);
   char what[160];
   if (best >= 1e29f) {
      out("nothing hit\n");
      snprintf(what, sizeof(what), "Inspect: nothing at the centre");
   } else if (bestObj) {
      char name[128];
      RenderRTGameObjectName(bestObj, name, sizeof(name));
      const CachedObject &c = g_objects[bestObj];
      out("object %u \"%s\" at %.2f m: %zu primitives, whole %d, drawn frame %u (now %u)\n",
          bestObj, name, best, c.prims.size(), int(c.whole), c.frame, g_frameNumber);
      for (size_t i = 0; i < c.prims.size() && i < 64; ++i) {
         const CapturedPrim &p = c.prims[i];
         const RtTexture *t = (p.tex && p.tex->serial == p.texSerial) ? p.tex : nullptr;
         out("  %s[%zu] %zu verts, texture \"%s\" %dx%d%s%s, flags 0x%x, emissive %.2f, blend %d, "
             "alphatest %d, sprite %d, game colour %.2f %.2f %.2f a %.2f\n",
             i == bestPrim ? "*" : " ", i, p.verts.size(), p.texture.c_str(), t ? t->width : 0,
             t ? t->height : 0, t && t->white ? " white" : "", t && t->hasAlpha ? " alpha" : "",
             unsigned(p.flags), p.emissive, p.blend, int(p.alphaTest), int(p.sprite),
             p.avgColor[0], p.avgColor[1], p.avgColor[2], p.avgColor[3]);
         if (i == bestPrim && t) {
            char base[64];
            snprintf(base, sizeof(base), "/inspect-%d-%s", n, t->name);
            dump_texture(t, dir + base);
            out("    texture saved as logs%s.ppm\n", base);
         }
      }
      snprintf(what, sizeof(what), "Inspect: %s (object %u) logged", name, bestObj);
   } else {
      const WorldGroup &w = g_world[bestGroup];
      auto found = g_worldTextures.find(w.textureId);
      const RtTexture *t = found == g_worldTextures.end() ? nullptr : found->second;
      out("world surface at %.2f m: texture id %d, flags 0x%x, texture %s %dx%d%s\n", best,
          w.textureId, w.flags, t ? t->name : "-", t ? t->width : 0, t ? t->height : 0,
          t && t->hasAlpha ? " alpha" : "");
      if (t) {
         char base[64];
         snprintf(base, sizeof(base), "/inspect-%d-%s", n, t->name);
         dump_texture(t, dir + base);
         out("    texture saved as logs%s.ppm\n", base);
      }
      snprintf(what, sizeof(what), "Inspect: world texture %d logged", w.textureId);
   }
   auto list_models = [&](const float *at, float radius, const char *title) {
      out("%s:\n", title);
      for (const auto &e : g_objects) {
         const CachedObject &c = e.second;
         const float dx = c.center[0] - at[0], dy = c.center[1] - at[1], dz = c.center[2] - at[2];
         if (dx * dx + dy * dy + dz * dz > radius * radius)
            continue;
         size_t tris = 0, shadowTris = 0;
         unsigned allFlags = 0;
         for (const CapturedPrim &p : c.prims) {
            allFlags |= unsigned(p.flags);
            tris += p.verts.size() / 3;
            if (p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY)
               shadowTris += p.verts.size() / 3;
         }
         char name[128];
         RenderRTGameObjectName(e.first, name, sizeof(name));
         out("  object %u \"%s\": %zu triangles (%zu shadow-only) in %zu primitives, whole %d, "
             "drawn frame %u (now %u), posed %d, flags 0x%x\n",
             e.first, name, tris, shadowTris, c.prims.size(), int(c.whole), c.frame, g_frameNumber,
             int(c.posed), allFlags);
      }
   };
   if (best < 1e29f) {
      // what lights and shadows the point: the lights given to the path
      // tracer last frame near it, and the models near it
      const float hit[3] = {o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best};
      out("hit point (%.2f %.2f %.2f) m; lights traced last frame within 12 m (of %u in the mission):\n",
          hit[0], hit[1], hit[2], g_lightsTotal);
      for (const PendingLight &L : g_lastLights) {
         const RgFloat3D p = to_rt(L.pos);
         const float dx = p.data[0] - hit[0], dy = p.data[1] - hit[1], dz = p.data[2] - hit[2];
         const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
         if (dist > 12.0f)
            continue;
         out("  light %u at (%.2f %.2f %.2f), %.2f m away: rgb %.0f %.0f %.0f, radius %.1f ft, "
             "%s inner %.3f outer %.3f dir (%.2f %.2f %.2f)\n",
             L.id, p.data[0], p.data[1], p.data[2], dist, L.rgb[0], L.rgb[1], L.rgb[2], L.radius,
             L.inner >= 0 ? "spot" : "point", L.inner, L.outer, L.dir[0], L.dir[1], L.dir[2]);
      }
      // and the ones near it that were NOT traced, with why
      for (const PendingLight &L : g_lastOffered) {
         const RgFloat3D p = to_rt(L.pos);
         const float dx = p.data[0] - hit[0], dy = p.data[1] - hit[1], dz = p.data[2] - hit[2];
         const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
         if (dist > 12.0f)
            continue;
         bool traced = false;
         for (const PendingLight &T : g_lastLights)
            traced = traced || T.id == L.id;
         if (!traced)
            out("  NOT traced: light %u at (%.2f %.2f %.2f), %.2f m away: rgb %.0f %.0f %.0f, %s\n",
                L.id, p.data[0], p.data[1], p.data[2], dist, L.rgb[0], L.rgb[1], L.rgb[2],
                L.reaches ? "not among the nearest" : "behind walls (level's lists)");
      }
      for (const PendingLight &L : g_lastDim) {
         const RgFloat3D p = to_rt(L.pos);
         const float dx = p.data[0] - hit[0], dy = p.data[1] - hit[1], dz = p.data[2] - hit[2];
         const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
         if (dist <= 12.0f)
            out("  OFF: light %u at (%.2f %.2f %.2f), %.2f m away: brightness %.2f %.2f %.2f\n",
                L.id, p.data[0], p.data[1], p.data[2], dist, L.rgb[0], L.rgb[1], L.rgb[2]);
      }
      list_models(hit, 4.0f, "models within 4 m of the hit point");
   }
   {
      const RgFloat3D cam = to_rt(g_camera.pos);
      list_models(cam.data, 8.0f, "models within 8 m of the camera");
   }
   if (!g_droppedObjects.empty()) {
      out("models recently dropped (not drawn by the game and moved or gone):\n");
      for (const DroppedObject &d : g_droppedObjects) {
         char name[128];
         RenderRTGameObjectName(d.obj, name, sizeof(name));
         out("  object %u \"%s\" at frame %u (now %u): %s\n", d.obj, name, d.frame, g_frameNumber,
             d.why);
      }
   }
   if (log)
      fclose(log);
   g_ui.status = std::string(what) + " to logs/rt-inspect.txt";
   g_ui.statusUntil = now_seconds() + 6.0;
}

} // namespace

namespace darkrt {

bool HandleKey(int scancode, int mods, bool down, bool repeat)
{
   if (!g_active)
      return false;
   const bool toggle = scancode == SDL_SCANCODE_F10 ||
                       (scancode == SDL_SCANCODE_COMMA && (mods & KMOD_GUI));
   if (toggle) {
      if (down && !repeat) {
         g_ui.open = !g_ui.open;
         g_ui.hintUntil = 0;
      }
      return true;
   }
   if (!g_ui.open || !down)
      return false;   // key releases always reach the game: no stuck keys
   switch (scancode) {
   case SDL_SCANCODE_ESCAPE: if (!repeat) g_ui.open = false; break;
   case SDL_SCANCODE_UP: g_ui.row = (g_ui.row + kRowCount - 1) % kRowCount; break;
   case SDL_SCANCODE_DOWN: g_ui.row = (g_ui.row + 1) % kRowCount; break;
   case SDL_SCANCODE_LEFT: change_row(g_ui.row, -1); break;
   case SDL_SCANCODE_RIGHT:
   case SDL_SCANCODE_RETURN:
   case SDL_SCANCODE_SPACE: change_row(g_ui.row, 1); break;
   default: break;
   }
   return true;   // the game doesn't see keys pressed while the panel is open
}

bool MovieFrame(const void *rgba, int width, int height, int rowBytes, const Rect &viewport,
                int drawableWidth, int drawableHeight)
{
   if (!g_active || !rgba || width <= 0 || height <= 0)
      return false;
   g_drawableW = std::max(drawableWidth, 1);
   g_drawableH = std::max(drawableHeight, 1);
   start_frame();
   if (!g_frameStarted)
      return false;
   FrameTexture &t = g_movieTex;
   t.width = width;
   t.height = height;
   t.rgba.resize(size_t(width) * height * 4);
   for (int y = 0; y < height; ++y)
      memcpy(t.rgba.data() + size_t(y) * width * 4,
             static_cast<const uint8_t *>(rgba) + size_t(y) * rowBytes, size_t(width) * 4);
   t.upload();
   RgPrimitiveVertex q[6];
   quad(q, 0, 0, float(width), float(height), pack_color(1, 1, 1, 1));
   upload_2d(0x2fffffe00ull, q, 6, t.provided ? t.name : nullptr, false, width, height, viewport);
   g_overlays.clear();
   draw_frame(false);
   return true;
}

} // namespace darkrt

// ============================================================================
// render_rt.h: called by the engine

extern "C" {

int RenderRTActive(void) { return g_active ? 1 : 0; }

void RenderRTSceneBegin(const float pos[3], const float fwd[3], const float left[3],
                        const float up[3], float zoom, int width, int height)
{
   if (!g_active)
      return;
   start_frame();
   if (!g_frameStarted)
      return;
   memcpy(g_camera.pos, pos, sizeof(g_camera.pos));
   memcpy(g_camera.fwd, fwd, sizeof(g_camera.fwd));
   memcpy(g_camera.left, left, sizeof(g_camera.left));
   memcpy(g_camera.up, up, sizeof(g_camera.up));
   g_camera.zoom = zoom > 0 ? zoom : 1.0f;
   g_camera.width = std::max(width, 1);
   g_camera.height = std::max(height, 1);

   // r3d: zoom 1 is 90 degrees across the canvas width, square pixels. The
   // scene is traced over the whole window with the same vertical field of
   // view; outside the game's viewport it is covered by the black bars.
   const double tanHalfX = 1.0 / g_camera.zoom;
   const double tanHalfY = tanHalfX * double(g_camera.height) / double(g_camera.width);
   const float fovY = float(2.0 * std::atan(tanHalfY));
   const float s = g_cfg.flip ? 1.0f : -1.0f;
   auto cam = RgCameraInfo{
      .sType = RG_STRUCTURE_TYPE_CAMERA_INFO,
      .pNext = nullptr,
      .flags = 0,
      .position = to_rt(pos),
      .up = {{up[0], up[1], up[2]}},
      .right = {{s * left[0], s * left[1], s * left[2]}},
      .fovYRadians = fovY,
      .aspect = float(g_drawableW) / float(std::max(g_drawableH, 1)),
      .cameraNear = 0.03f,
      .cameraFar = 2000.0f,
      .pView = nullptr,
   };
   g_cameraUploaded = rt.rgUploadCamera(&cam) == RG_RESULT_SUCCESS;
   g_inScene = g_cameraUploaded;
   g_suppress = true;
   g_RenderRTCaptureVertices = 0;
}

void RenderRTSceneEnd(void)
{
   if (!g_active || !g_inScene)
      return;
   flush_lights();
   upload_world();
   upload_objects();
   ++g_frameNumber;
   g_inScene = false;
   g_RenderRTCaptureVertices = 0;
}

int RenderRTCaptureDraws(void) { return g_inScene ? 1 : 0; }

int RenderRTSuppressCapture(int suppress)
{
   const int previous = g_suppress ? 1 : 0;
   g_suppress = suppress != 0;
   g_RenderRTCaptureVertices = (g_inScene && !g_suppress) ? 1 : 0;
   return previous;
}

void RenderRTDrawWorld(int primitive, const sRenderBackendVertex *v, const float *xyz,
                       int count)
{
   if (g_RenderRTShadowOnly && g_dropBackFaces)
      return;
   if (!g_inScene || primitive != kRenderBackendTriangles || count < 3)
      return;
   if (g_blend == kRenderBackendBlendMultiply)
      return;   // a lightmap or shadow pass over something already submitted
   RtTexture *tex = g_bound[0];
   const char *name = texture_name(tex);
   const bool sprite = g_emissiveSpriteNow || g_RenderRTEmissiveSprite;
   CapturedPrim prim;
   prim.texture = name ? name : "";
   prim.verts.resize(static_cast<size_t>(count));
   float minAlpha = 1.0f;
   bool allWhite = true;
   for (int i = 0; i < count; ++i) {
      RgPrimitiveVertex &d = prim.verts[size_t(i)];
      d = {};
      RgFloat3D p = to_rt(xyz + i * 3);
      memcpy(d.position, p.data, sizeof(d.position));
      d.texCoord[0] = v[i].u0;
      d.texCoord[1] = v[i].v0;
      minAlpha = std::min(minAlpha, v[i].a);
      if (v[i].r < 0.98f || v[i].g < 0.98f || v[i].b < 0.98f)
         allWhite = false;
      // lit polygons carry the game's baked lighting in their colour; the
      // path tracer lights them itself, so only untextured ones keep it
      // (sprites keep theirs: it is the particle's tint over an alpha texture)
      d.color = (name && !sprite) ? pack_color(1, 1, 1, v[i].a)
                                  : pack_color(v[i].r, v[i].g, v[i].b, v[i].a);
   }
   if (g_RenderRTShadowOnly) {
      // a face turned away from the camera: it blocks light, but is not seen
      // (two-sided model parts are pairs of coincident opposite faces)
      prim.flags = RG_MESH_PRIMITIVE_SHADOW_ONLY |
                   ((tex && tex->cutout && g_alphaTest) ? RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_ALPHA_TESTED) : 0u);
   } else if (sprite) {
      // a glow or particle sprite, never casting shadows. Not emissive: RTGL1
      // adds a translucent primitive's emission without its texture's alpha,
      // so any glow at all turned the soft round particles into squares.
      prim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_NO_SHADOW |
                    RG_MESH_PRIMITIVE_NO_WATER_CAUSTICS;
      prim.emissive = 0.0f;
   } else if (g_blend == kRenderBackendBlendAdd) {
      prim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
      prim.emissive = 1.0f;
   } else if (g_blend == kRenderBackendBlendAlpha && minAlpha < 0.99f) {
      prim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
   } else if (tex && tex->cutout && (g_alphaTest || g_blend == kRenderBackendBlendAlpha)) {
      prim.flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
   }

   // a camera-facing bitmap object: seen, but it must not shadow anything
   if (g_currentObject && g_currentNoShadow && !g_RenderRTShadowOnly)
      prim.flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
   prim.tex = tex;
   prim.texSerial = tex ? tex->serial : 0;
   prim.blend = g_blend;
   prim.alphaTest = g_alphaTest;
   prim.sprite = sprite;
   for (int i = 0; i < count; ++i) {
      prim.avgColor[0] += v[i].r / count;
      prim.avgColor[1] += v[i].g / count;
      prim.avgColor[2] += v[i].b / count;
      prim.avgColor[3] += v[i].a / count;
   }

   // A light fixture's bulb, flame or glass sits at the light it carries and
   // is drawn self-lit (full white): make it glow, so lamps are seen to be lit.
   if (!sprite && !g_RenderRTShadowOnly && g_cfg.glow > 0.0f && allWhite && !g_lightPositions.empty() && name) {
      float c[3] = {0, 0, 0};
      for (const RgPrimitiveVertex &d : prim.verts)
         for (int k = 0; k < 3; ++k)
            c[k] += d.position[k];
      for (float &x : c)
         x /= float(prim.verts.size());
      float extent = 0;
      for (const RgPrimitiveVertex &d : prim.verts) {
         const float dx = d.position[0] - c[0], dy = d.position[1] - c[1], dz = d.position[2] - c[2];
         extent = std::max(extent, dx * dx + dy * dy + dz * dz);
      }
      // only small parts: never a wall or a whole statue near a torch
      if (extent < 0.6f * 0.6f)
         for (const RgFloat3D &l : g_lightPositions) {
            const float dx = l.data[0] - c[0], dy = l.data[1] - c[1], dz = l.data[2] - c[2];
            if (dx * dx + dy * dy + dz * dz < 0.5f * 0.5f) {
               prim.emissive = std::max(prim.emissive, g_cfg.glow);
               break;
            }
         }
   }

   auto emit = [&](CapturedPrim &&p) {
      if (g_currentObject) {
         g_currentCapture.push_back(std::move(p));
         return;
      }
      if (g_freeze)
         return;
      compute_normals(&p, 1);
      const uint32_t key = tex ? tex->serial : 0;
      const uint32_t n = g_primitiveCounters[key]++;
      upload_captured(0x300000000ull + (uint64_t(key) << 12) + n, p);
   };
   const bool solidUntextured = !name && !(prim.flags & RG_MESH_PRIMITIVE_TRANSLUCENT);
   if (!solidUntextured) {
      emit(std::move(prim));
      return;
   }
   // one primitive per material colour (taken from each triangle's first vertex)
   std::vector<std::pair<uint32_t, CapturedPrim>> parts;
   for (size_t t = 0; t + 2 < prim.verts.size(); t += 3) {
      const uint32_t c = prim.verts[t].color | 0xff000000u;
      auto it = std::find_if(parts.begin(), parts.end(),
                             [c](const auto &e) { return e.first == c; });
      if (it == parts.end()) {
         CapturedPrim p = prim;
         p.verts.clear();
         p.color = c;
         parts.emplace_back(c, std::move(p));
         it = parts.end() - 1;
      }
      it->second.verts.insert(it->second.verts.end(), prim.verts.begin() + t,
                              prim.verts.begin() + t + 3);
   }
   for (auto &e : parts)
      emit(std::move(e.second));
}

void RenderRTObjectBegin(uint32_t obj, int flags)
{
   g_currentObject = g_inScene ? obj : 0;
   g_currentWhole = (flags & kRenderRTObjectWhole) != 0;
   g_currentNoShadow = (flags & kRenderRTObjectNoShadow) != 0;
   g_currentCapture.clear();
}

int RenderRTObjectWanted(uint32_t obj)
{
   if (!g_inScene)
      return 0;
   auto it = g_objects.find(obj);
   if (it == g_objects.end())
      return 1;
   const CachedObject &c = it->second;
   if (c.frame == g_frameNumber)
      return 0;   // drawn this frame
   float pose[6];
   if (!c.posed || !RenderRTGameObjectPose(obj, pose))
      return 1;
   for (int k = 0; k < 3; ++k)
      if (std::fabs(pose[k] - c.pose[k]) >= 0.01f)
         return 1;
   return (pose[3] != c.pose[3] || pose[4] != c.pose[4] || pose[5] != c.pose[5]) ? 1 : 0;
}

void RenderRTCameraMedium(int underwater) { g_underwater = underwater != 0; }

void RenderRTObjectEnd(void)
{
   const uint32_t obj = g_currentObject;
   g_currentObject = 0;
   if (!obj || !g_inScene)
      return;
   if (g_freeze) {
      g_currentCapture.clear();
      return;
   }
   CachedObject &c = g_objects[obj];
   if (c.frame == g_frameNumber) {
      // drawn twice this frame (split across cells): keep both parts, unless
      // the first one already had all of it
      if (g_currentWhole && !c.whole) {
         c.prims = std::move(g_currentCapture);
         c.whole = true;
         c.hash = hash_prims(c.prims);
      } else if (!c.whole) {
         for (CapturedPrim &p : g_currentCapture)
            c.prims.push_back(std::move(p));
         c.hash = hash_prims(c.prims);
      }
   } else {
      c.whole = g_currentWhole;
      const uint64_t h = hash_prims(g_currentCapture);
      c.unchanged = (h == c.hash) ? c.unchanged + 1 : 0;
      c.hash = h;
      c.prims = std::move(g_currentCapture);
      c.frame = g_frameNumber;
      c.posed = RenderRTGameObjectPose(obj, c.pose) != 0;
      size_t n = 0;
      double sum[3] = {0, 0, 0};
      for (const CapturedPrim &p : c.prims)
         for (const RgPrimitiveVertex &v : p.verts) {
            for (int k = 0; k < 3; ++k)
               sum[k] += v.position[k];
            ++n;
         }
      for (int k = 0; k < 3; ++k)
         c.center[k] = n ? float(sum[k] / double(n)) : 0.0f;
   }
   // the normals are recomputed only when the shape changed
   size_t total = 0;
   for (const CapturedPrim &p : c.prims)
      total += p.verts.size();
   if (c.normals.size() == total && c.normalsHash == c.hash) {
      size_t i = 0;
      for (CapturedPrim &p : c.prims)
         for (RgPrimitiveVertex &v : p.verts)
            v.normalPacked = c.normals[i++];
   } else {
      compute_normals(c.prims.data(), c.prims.size());
      c.normals.clear();
      for (const CapturedPrim &p : c.prims)
         for (const RgPrimitiveVertex &v : p.verts)
            c.normals.push_back(v.normalPacked);
      c.normalsHash = c.hash;
   }
   g_currentCapture.clear();
}

int RenderRTWorldWanted(uint32_t serial)
{
   return g_active && (!g_worldValid || serial != g_worldSerial) ? 1 : 0;
}

void RenderRTWorldBegin(uint32_t serial)
{
   g_world.clear();
   g_worldIndex.clear();
   g_worldTextures.clear();
   g_worldSerial = serial;
   g_worldValid = false;
}

void RenderRTWorldPolygon(int textureId, int flags, int count, const float *xyz, const float *uv)
{
   if (count < 3)
      return;
   const uint64_t key = (uint64_t(uint32_t(textureId)) << 8) | uint64_t(flags & 0xff);
   auto found = g_worldIndex.find(key);
   if (found == g_worldIndex.end()) {
      found = g_worldIndex.emplace(key, g_world.size()).first;
      g_world.push_back(WorldGroup{textureId, flags, {}, {}});
   }
   WorldGroup &g = g_world[found->second];
   // Newell normal
   float nx = 0, ny = 0, nz = 0;
   for (int i = 0; i < count; ++i) {
      const float *a = xyz + i * 3, *b = xyz + ((i + 1) % count) * 3;
      nx += (a[1] - b[1]) * (a[2] + b[2]);
      ny += (a[2] - b[2]) * (a[0] + b[0]);
      nz += (a[0] - b[0]) * (a[1] + b[1]);
   }
   const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
   if (len > 0) { nx /= len; ny /= len; nz /= len; }
   const RgNormalPacked32 normal = rt.rgUtilPackNormal ? rt.rgUtilPackNormal(nx, ny, nz) : 0;
   const uint32_t base = uint32_t(g.verts.size());
   for (int i = 0; i < count; ++i) {
      RgPrimitiveVertex v{};
      RgFloat3D p = to_rt(xyz + i * 3);
      memcpy(v.position, p.data, sizeof(v.position));
      v.normalPacked = normal;
      v.texCoord[0] = uv[i * 2];
      v.texCoord[1] = uv[i * 2 + 1];
      v.color = pack_color(1, 1, 1, 1);
      g.verts.push_back(v);
   }
   // A zig-zag strip across the (convex) polygon rather than a fan from one
   // corner: no long slivers, whose wide bounding boxes slow every ray.
   // Same winding as the fan.
   for (int lo = 0, hi = count - 1, k = 0; hi - lo >= 2; ++k) {
      uint32_t a, b, c;
      if ((k & 1) == 0) { a = lo; b = lo + 1; c = hi; ++lo; }
      else { a = lo; b = hi - 1; c = hi; --hi; }
      g.indices.push_back(base + a);
      g.indices.push_back(base + b);
      g.indices.push_back(base + c);
   }
}

void RenderRTWorldEnd(void)
{
   g_worldValid = true;
   size_t verts = 0;
   for (const WorldGroup &g : g_world)
      verts += g.verts.size();
   fprintf(stderr, "RT: world representation: %zu texture groups, %zu vertices\n",
           g_world.size(), verts);
}

void RenderRTWorldTexture(int textureId, void *texture)
{
   g_worldTextures[textureId] = static_cast<RtTexture *>(texture);
}

void RenderRTAmbient(float r, float g, float b)
{
   // one number: RTGL1's glow takes the surface's own colour
   const float a = std::clamp(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.0f, 1.0f);
   if (std::fabs(a - g_levelAmbient) > 1e-4f)
      fprintf(stderr, "RT: level ambient light %.3f %.3f %.3f\n", r, g, b);
   g_levelAmbient = a;
}

void RenderRTLight(uint32_t id, const float pos[3], const float rgb[3], float radius,
                   const float dir[3], float inner, float outer, int reaches)
{
   if (!g_inScene)
      return;
   const float peak = std::max({rgb[0], rgb[1], rgb[2]});
   if (peak <= 0.5f) {
      PendingLight d;
      d.id = id;
      memcpy(d.pos, pos, sizeof(d.pos));
      memcpy(d.rgb, rgb, sizeof(d.rgb));
      d.peak = peak;
      g_dimPending.push_back(d);
      return;
   }
   g_lightPositions.push_back(to_rt(pos));
   PendingLight l;
   l.id = id;
   memcpy(l.pos, pos, sizeof(l.pos));
   memcpy(l.rgb, rgb, sizeof(l.rgb));
   memcpy(l.dir, dir, sizeof(l.dir));
   l.radius = radius;
   l.inner = inner;
   l.outer = outer;
   l.peak = peak;
   l.reaches = reaches != 0;
   g_pendingLights.push_back(l);
}

} // extern "C"

namespace {

void upload_light(const PendingLight &L)
{
   const float peak = L.peak;
   const uint32_t color = pack_color(L.rgb[0] / peak, L.rgb[1] / peak, L.rgb[2] / peak, 1);
   // Dark's brightness is a light level (0..255 at the source, falling off
   // with distance); RTGL1's intensity is luminous flux. Calibrated by eye.
   // Dark falls off as 1/distance up to the light's radius, RTGL1 as
   // 1/distance^2; a larger source and a lower flux keep the nearby walls
   // from burning out while the far side of a room still receives light.
   const float lumens = peak * 4.0f * g_cfg.light;
   const float *dir = L.dir;
   RgLightSphericalEXT sphere{};
   RgLightSpotEXT spot{};
   void *ext;
   if (L.inner >= 0.0f && (dir[0] != 0 || dir[1] != 0 || dir[2] != 0)) {
      const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      spot = RgLightSpotEXT{
         .sType = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
         .pNext = nullptr,
         .color = color,
         .intensity = lumens,
         .position = to_rt(L.pos),
         .direction = {{dir[0] / l, dir[1] / l, dir[2] / l}},
         .radius = g_cfg.lightSize,
         // Dark stores the cone as cosines
         .angleOuter = std::acos(std::clamp(L.outer, -1.0f, 1.0f)),
         .angleInner = std::acos(std::clamp(L.inner, -1.0f, 1.0f)),
      };
      ext = &spot;
   } else {
      sphere = RgLightSphericalEXT{
         .sType = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
         .pNext = nullptr,
         .color = color,
         .intensity = lumens,
         .position = to_rt(L.pos),
         .radius = g_cfg.lightSize,
      };
      ext = &sphere;
   }
   auto info = RgLightInfo{
      .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
      .pNext = ext,
      .uniqueID = 0x400000000ull + L.id,
      .isExportable = false,
   };
   if (rt.rgUploadLight(&info) == RG_RESULT_SUCCESS)
      ++g_frameLights;
}

// Thief's torches flicker: their brightness jumps every frame. The denoiser
// takes a jump in the light as a change and throws away what it had gathered
// for every pixel that light reaches -- walls by torchlight never settle, the
// crawling splotches. Blood RT smooths its flickering sectors the same way.
// The brightness is eased toward the game's over a fraction of a second.
void smooth_flicker()
{
   static std::unordered_map<uint32_t, std::array<float, 4>> s_state;   // rgb, last frame seen
   static std::chrono::steady_clock::time_point s_last = std::chrono::steady_clock::now();
   static uint32_t s_frame = 0;
   static double s_logAt = 0;
   static uint32_t s_changing = 0, s_counted = 0;
   const auto now = std::chrono::steady_clock::now();
   const float dt = std::clamp(std::chrono::duration<float>(now - s_last).count(), 0.0f, 0.1f);
   s_last = now;
   ++s_frame;
   const float tau = g_cfg.flicker == 2 ? 1.5f : 0.3f;
   const float a = g_cfg.flicker == 1 ? 1.0f : 1.0f - std::exp(-dt / tau);
   for (PendingLight &L : g_pendingLights) {
      auto it = s_state.find(L.id);
      if (it == s_state.end() || it->second[3] + 30 < float(s_frame)) {
         s_state[L.id] = {L.rgb[0], L.rgb[1], L.rgb[2], float(s_frame)};
         continue;
      }
      std::array<float, 4> &st = it->second;
      const float before = std::max({st[0], st[1], st[2]});
      if (std::fabs(L.peak - before) > 0.05f * std::max(before, 1.0f))
         ++s_changing;
      // put out (a torch doused): no easing, it goes dark at once
      const float b = L.peak < 0.15f * before ? 1.0f : a;
      for (int k = 0; k < 3; ++k) {
         st[k] += (L.rgb[k] - st[k]) * b;
         L.rgb[k] = st[k];
      }
      st[3] = float(s_frame);
      L.peak = std::max({L.rgb[0], L.rgb[1], L.rgb[2], 0.01f});
   }
   s_counted += uint32_t(g_pendingLights.size());
   const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
   if (t > s_logAt) {
      if (s_logAt > 0 && s_counted)
         fprintf(stderr, "RT: lights changing brightness: %.1f%% of light-frames (flicker %d)\n",
                 100.0 * double(s_changing) / double(s_counted), g_cfg.flicker);
      s_logAt = t + 5.0;
      s_changing = s_counted = 0;
   }
}

// RTGL1 picks each pixel's candidate lights uniformly from ALL the lights it
// is given (this build has no light grid). A Thief mission has hundreds, so
// nearly every candidate was a far-away light: the lamp next to you was
// rarely tried -- splotchy, unstable light that changed with where you stood.
// Only the lights that matter near the camera are given to it: ranked by
// brightness over distance squared, a light chosen last frame favoured so the
// set does not flicker between frames.
void flush_lights()
{
   if (g_freeze) {
      for (const PendingLight &L : g_frozenLights)
         upload_light(L);
      g_pendingLights.clear();
      g_dimPending.clear();
      return;
   }
   g_lightsTotal = uint32_t(g_pendingLights.size());
   smooth_flicker();
   g_lastOffered = g_pendingLights;
   g_lastDim.swap(g_dimPending);
   g_dimPending.clear();
   // A light that shines on none of the cells around the camera or in view
   // is behind walls (the level's own lighting says so): every candidate
   // RTGL1 spent on it would be shadowed, the splotches in dark places.
   // Never one within 15 m: those are kept whatever the level's lists say.
   if (g_cfg.lightCull) {
      const RgFloat3D c = to_rt(g_camera.pos);
      g_pendingLights.erase(
         std::remove_if(g_pendingLights.begin(), g_pendingLights.end(),
                        [&c](const PendingLight &L) {
                           if (L.reaches)
                              return false;
                           const RgFloat3D p = to_rt(L.pos);
                           const float dx = p.data[0] - c.data[0], dy = p.data[1] - c.data[1],
                                       dz = p.data[2] - c.data[2];
                           return dx * dx + dy * dy + dz * dz > 15.0f * 15.0f;
                        }),
         g_pendingLights.end());
   }
   g_lightsReaching = uint32_t(g_pendingLights.size());
   const RgFloat3D cam = to_rt(g_camera.pos);
   const size_t keep = g_cfg.maxLights > 0 ? size_t(g_cfg.maxLights) : g_pendingLights.size();
   static std::unordered_map<uint32_t, uint32_t> s_chosen;   // id -> frame last chosen
   static uint32_t s_frame = 0;
   ++s_frame;
   if (g_pendingLights.size() > keep) {
      for (PendingLight &L : g_pendingLights) {
         const RgFloat3D p = to_rt(L.pos);
         const float dx = p.data[0] - cam.data[0], dy = p.data[1] - cam.data[1],
                     dz = p.data[2] - cam.data[2];
         const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
         float score = L.peak / std::max(d * d, 9.0f);
         // a light whose reach (Dark's radius) ends well before the camera
         const float reach = L.radius * kMetersPerUnit;
         if (L.radius > 0 && d > reach + 15.0f)
            score *= 0.05f;
         auto it = s_chosen.find(L.id);
         if (it != s_chosen.end() && it->second + 1 == s_frame)
            score *= 1.5f;
         L.score = score;
      }
      std::nth_element(g_pendingLights.begin(), g_pendingLights.begin() + keep,
                       g_pendingLights.end(),
                       [](const PendingLight &a, const PendingLight &b) { return a.score > b.score; });
      g_pendingLights.resize(keep);
   }
   for (const PendingLight &L : g_pendingLights) {
      upload_light(L);
      s_chosen[L.id] = s_frame;
   }
   g_lastLights = g_pendingLights;
   g_pendingLights.clear();
}

} // namespace

extern "C" {

} // extern "C"
#pragma pack(pop)
