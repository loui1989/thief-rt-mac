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
// Settings (environment, read at start):
//   THIEF_RT=1            use the ray tracer (run.sh sets it)
//   THIEF_RT_UPSCALE      0 native, 1 quality, 2 balanced, 3 performance (default),
//                         4 ultra performance  (MetalFX)
//   THIEF_RT_LIGHT        light brightness multiplier (default 1)
//   THIEF_RT_SKY          sky brightness (default 1)
//   THIEF_RT_FLIP=1       mirror the camera's right vector (diagnostic)
//   THIEF_RT_DEBUG=1      RTGL1 info messages, per-second statistics

#define RG_USE_SURFACE_METAL
#pragma pack(push, 8)
#include <RTGL1/RTGL1.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>

#include <algorithm>
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

#include <render_rt.h>
#include "rt_backend_internal.h"

extern "C" int g_RenderRTCaptureVertices = 0;

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
   bool flip = false;
   bool debug = false;
} g_cfg;

float env_float(const char *name, float fallback)
{
   const char *v = getenv(name);
   return v && *v ? (float)atof(v) : fallback;
}

// ---- textures ----------------------------------------------------------

struct RtTexture {
   uint32_t serial = 0;
   char name[24] = {};
   int width = 0, height = 0;
   std::vector<uint8_t> rgba;
   bool dirty = true;      // pixels changed since RTGL1 got them
   bool provided = false;  // RTGL1 has a texture of this name
   bool hasAlpha = false;
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
   bool alpha = false;
   for (int y = 0; y < height; ++y) {
      const uint8_t *src = static_cast<const uint8_t *>(rgba) + size_t(y) * rowBytes;
      uint8_t *dst = t.rgba.data() + size_t(y) * width * 4;
      memcpy(dst, src, size_t(width) * 4);
      if (!alpha)
         for (int x = 0; x < width; ++x)
            if (dst[x * 4 + 3] != 255) { alpha = true; break; }
   }
   t.hasAlpha = alpha;
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
   std::vector<uint8_t> rgba;
   int width = 0, height = 0;
   void upload()
   {
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
int g_drawableW = 1, g_drawableH = 1;
uint32_t g_framePrims = 0, g_frameDropped = 0, g_frameLights = 0;

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
   if (g_cfg.upscale >= 1 && g_cfg.upscale <= 4) {
      // on Metal the AMD FSR2 slot is the MetalFX temporal scaler
      resolution.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
      resolution.sharpenTechnique = RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
   } else {
      resolution.upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_LINEAR;
      resolution.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
      resolution.customRenderSize = {uint32_t(std::max(1, g_drawableW)),
                                     uint32_t(std::max(1, g_drawableH))};
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
   RgResult r = rt.rgStartFrame(&info);
   g_frameStarted = r == RG_RESULT_SUCCESS;
   if (!g_frameStarted) {
      static int warned = 0;
      if (warned++ < 5)
         fprintf(stderr, "RT: rgStartFrame failed: %s\n",
                 rt.rgUtilGetResultDescription ? rt.rgUtilGetResultDescription(r) : "?");
   }
   g_cameraUploaded = false;
   g_framePrims = g_frameDropped = g_frameLights = 0;
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
   for (size_t i = 0; i < g_world.size(); ++i) {
      WorldGroup &g = g_world[i];
      if (g.verts.empty() || (g.flags & kRenderRTWorldSky))
         continue;
      auto found = g_worldTextures.find(g.textureId);
      RtTexture *tex = found == g_worldTextures.end() ? nullptr : found->second;
      RgMeshPrimitiveFlags flags = 0;
      if ((g.flags & kRenderRTWorldTransparent) || (tex && tex->hasAlpha))
         flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
      if (g.flags & kRenderRTWorldWater)
         flags |= RG_MESH_PRIMITIVE_WATER;
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
         .color = pack_color(1, 1, 1, 1),
         .emissive = (g.flags & kRenderRTWorldFullbright) ? 0.25f : 0.0f,
         .classicLight = 1.0f,
         .emissiveGi = 0.0f,
      };
      upload(0x100000000ull + i, prim);
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
   g_cfg.upscale = std::clamp(int(env_float("THIEF_RT_UPSCALE", 3)), 0, 4);
   g_cfg.light = env_float("THIEF_RT_LIGHT", 1.0f);
   g_cfg.sky = env_float("THIEF_RT_SKY", 1.0f);
   g_cfg.flip = env_float("THIEF_RT_FLIP", 0) != 0;
   g_cfg.debug = env_float("THIEF_RT_DEBUG", 0) != 0;

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
   t.width = width;
   t.height = height;
   t.rgba.resize(size_t(width) * height * 4);
   for (int y = 0; y < height; ++y) {
      uint8_t *dst = t.rgba.data() + size_t(y) * width * 4;
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

void draw_frame(bool sceneDrawn)
{
   double current = now_seconds();
   auto info = RgDrawFrameInfo{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_INFO,
      .pNext = nullptr,
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
   RgResult r = rt.rgDrawFrame(&info);
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
   g_vsync = vsync;
   g_drawableW = std::max(drawableWidth, 1);
   g_drawableH = std::max(drawableHeight, 1);
   start_frame();
   if (!g_frameStarted) {
      g_overlays.clear();
      return;
   }
   const bool scene = g_cameraUploaded;

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

      g_canvasTex.upload();
      RgPrimitiveVertex q[6];
      quad(q, 0, 0, float(g_canvas.width), float(g_canvas.height), pack_color(1, 1, 1, 1));
      upload_2d(0x2ffffff01ull, q, 6, g_canvasTex.provided ? g_canvasTex.name : nullptr,
                g_canvas.scene, g_canvas.width, g_canvas.height, vp);
   }
   draw_frame(scene);
   g_canvas.valid = false;
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
   upload_world();
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
   if (!g_inScene || primitive != kRenderBackendTriangles || count < 3)
      return;
   RtTexture *tex = g_bound[0];
   const char *name = texture_name(tex);
   std::vector<RgPrimitiveVertex> verts(static_cast<size_t>(count));
   float minAlpha = 1.0f;
   for (int i = 0; i < count; ++i) {
      RgPrimitiveVertex &d = verts[size_t(i)];
      d = {};
      RgFloat3D p = to_rt(xyz + i * 3);
      memcpy(d.position, p.data, sizeof(d.position));
      d.texCoord[0] = v[i].u0;
      d.texCoord[1] = v[i].v0;
      minAlpha = std::min(minAlpha, v[i].a);
      // lit polygons carry the game's baked lighting in their colour; the
      // path tracer lights them itself, so only untextured ones keep it
      d.color = name ? pack_color(1, 1, 1, v[i].a) : pack_color(v[i].r, v[i].g, v[i].b, v[i].a);
   }
   RgMeshPrimitiveFlags flags = 0;
   float emissive = 0.0f;
   if (g_blend == kRenderBackendBlendAdd) {
      flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
      emissive = 1.0f;
   } else if (g_blend == kRenderBackendBlendAlpha && minAlpha < 0.99f) {
      flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
   } else if (g_alphaTest || (tex && tex->hasAlpha)) {
      flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
   }
   if (g_blend == kRenderBackendBlendMultiply)
      return;   // a lightmap or shadow pass over something already submitted
   const uint32_t key = tex ? tex->serial : 0;
   const uint32_t n = g_primitiveCounters[key]++;
   const uint64_t id = 0x300000000ull + (uint64_t(key) << 12) + n;
   auto prim = RgMeshPrimitiveInfo{
      .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
      .pNext = nullptr,
      .flags = flags,
      .primitiveIndexInMesh = 0,
      .pVertices = verts.data(),
      .vertexCount = uint32_t(verts.size()),
      .pIndices = nullptr,
      .indexCount = 0,
      .pTextureName = name,
      .textureFrame = 0,
      .color = pack_color(1, 1, 1, 1),
      .emissive = emissive,
      .classicLight = 1.0f,
      .emissiveGi = 0.0f,
   };
   upload(id, prim);
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
   for (int i = 2; i < count; ++i) {
      g.indices.push_back(base);
      g.indices.push_back(base + uint32_t(i - 1));
      g.indices.push_back(base + uint32_t(i));
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

void RenderRTLight(uint32_t id, const float pos[3], const float rgb[3], float radius,
                   const float dir[3], float inner, float outer)
{
   if (!g_inScene)
      return;
   const float peak = std::max({rgb[0], rgb[1], rgb[2]});
   if (peak <= 0.5f)
      return;
   const uint32_t color = pack_color(rgb[0] / peak, rgb[1] / peak, rgb[2] / peak, 1);
   // Dark's brightness is a light level (0..255 at the source, falling off
   // with distance); RTGL1's intensity is luminous flux. Calibrated by eye.
   float lumens = peak * 40.0f * g_cfg.light;
   (void)radius;
   RgLightSphericalEXT sphere{};
   RgLightSpotEXT spot{};
   void *ext;
   if (inner >= 0.0f && (dir[0] != 0 || dir[1] != 0 || dir[2] != 0)) {
      const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      spot = RgLightSpotEXT{
         .sType = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT,
         .pNext = nullptr,
         .color = color,
         .intensity = lumens,
         .position = to_rt(pos),
         .direction = {{dir[0] / l, dir[1] / l, dir[2] / l}},
         .radius = 0.05f,
         // Dark stores the cone as cosines
         .angleOuter = std::acos(std::clamp(outer, -1.0f, 1.0f)),
         .angleInner = std::acos(std::clamp(inner, -1.0f, 1.0f)),
      };
      ext = &spot;
   } else {
      sphere = RgLightSphericalEXT{
         .sType = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
         .pNext = nullptr,
         .color = color,
         .intensity = lumens,
         .position = to_rt(pos),
         .radius = 0.05f,
      };
      ext = &sphere;
   }
   auto info = RgLightInfo{
      .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
      .pNext = ext,
      .uniqueID = 0x400000000ull + id,
      .isExportable = false,
   };
   if (rt.rgUploadLight(&info) == RG_RESULT_SUCCESS)
      ++g_frameLights;
}

} // extern "C"
#pragma pack(pop)
