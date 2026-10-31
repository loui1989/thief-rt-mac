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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <resimginfo.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
// SDK headers end with "#pragma pack()", which restores the engine's packing
#pragma pack(8)

#include <render_rt.h>
#include "rt_backend_internal.h"
#include "rt_font.h"
extern "C" float portal_rt_cell_walk(int cell);   // portal.c: feet walked, 0 on screen, < 0 out of reach

extern "C" const char *DarkContentFMPath(void);
extern "C" const char *DarkContentBasePath(void);
extern "C" int g_RenderRTCaptureVertices = 0;
extern "C" int g_RenderRTEmissiveSprite = 0;
extern "C" int g_RenderRTCaptureCalls = 0, g_RenderRTCaptureDropped = 0;
extern "C" int g_RenderRTShadowOnly = 0;

namespace {

// Dark units are feet.
constexpr float kMetersPerUnit = 0.3048f;

RgInterface rt{};
RgLibraryHandle g_lib = nullptr;
bool g_active = false;
SDL_MetalView g_metalView = nullptr;
SDL_Window *g_sdlWindow = nullptr;
std::string g_rtDir;

struct Settings {
   int upscale = 3;
   float light = 1.0f;
   float sky = 1.0f;
   float glow = 1.0f;         // light fixtures
   float fixtureLight = 2.0f; // the light of lamps and torches (lights at a glowing fixture or a flame)
   float spriteGlow = 1.0f;   // flame sprites' glow (their emission maps)
   bool materials = true;     // material maps made from the textures (rt_materials.h)
   float bump = 1.0f;         // their bumps' strength
   float parallax = 0.03f;    // parallax depth, in texture widths (0 off)
   bool polish = true;        // polished surfaces mirror the room
   float crevice = 0.6f;      // joints and cracks darkened in the textures' colour
   int water = 0;             // 0 translucent (as the game), 1 RTGL1 water, 2 opaque
   int gi = 0;                // bounce light: 0 two bounces at half rate (as Blood RT),
                              // 1 one bounce, 2 two bounces, 3 one bounce at half rate
   bool volumetric = false;   // haze lit by every light
   int fog = 2;               // the mission's fog: 0 off, 1 light, 2 as the mission, 3 thick
   int dynres = 0;            // dynamic resolution's target fps, 0 off
   int outputHeight = 0;      // the frame's height in pixels, 0 = the screen's own (Retina)
   bool perfLog = false;      // logs/perf.log: where each frame's time goes, every 2 s
   int frameCap = 60;         // the most frames a second (the game's own pacing: 60)
   int reach = 40;            // lights fade out with their walking distance (metres); 0: nearest maxLights
   int maxLights = 64;        // lights given to the path tracer, nearest first; 0 all
   int flicker = 0;           // flickering lights: 0 smoothed, 1 as the game, 2 nearly steady
   bool smoothModels = false; // models shaded smooth (their normals blended across edges)
   float ambient = 1.0f;      // the level's ambient light, as a faint glow of every surface
   int candidates = 12;       // lights tried per pixel and per bounce (RTGL1: no rays, ALU only)
   int evMin = 3;             // eye adaptation's darkest exposure (EV; Blood RT 5, RTGL1 0)
   int response = 0;          // how fast lighting changes show: 0 calm (Blood RT), 1 quick, 2 instant
   bool rtDetection = true;   // the light gem and the guards go by the ray-traced light
   bool bloom = true;         // the glow around bright light (~0.8 ms at 4K output)
   bool lightFade = true;     // lights entering or leaving the traced set fade
   bool steadyFaces = true;   // models' faces seen or shadow-only by a fixed rule, not by the view
   bool lightFix = true;      // lights moved out of the walls they sit in
   bool fixturesPass = true;  // a torch or small lamp lets its own light out
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

constexpr float kDefaultsSpriteGlow = 1.0f;

void load_settings()
{
   FILE *f = fopen(settings_path().c_str(), "r");
   if (!f)
      return;
   char key[64];
   float value;
   int version = 1;
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
      else if (k == "fixture_light") g_cfg.fixtureLight = std::clamp(value, 0.25f, 6.0f);
      else if (k == "sprite_glow") g_cfg.spriteGlow = std::clamp(value, 0.0f, 10.0f);
      else if (k == "gi") g_cfg.gi = std::clamp(int(value), 0, 3);
      else if (k == "volumetric") g_cfg.volumetric = value != 0;
      else if (k == "fog") g_cfg.fog = std::clamp(int(value), 0, 3);
      else if (k == "materials") g_cfg.materials = value != 0;
      else if (k == "bump") g_cfg.bump = std::clamp(value, 0.0f, 4.0f);
      else if (k == "parallax") g_cfg.parallax = std::clamp(value, 0.0f, 0.1f);
      else if (k == "polish") g_cfg.polish = value != 0;
      else if (k == "crevice") g_cfg.crevice = std::clamp(value, 0.0f, 1.0f);
      else if (k == "settings_version") version = int(value);
      else if (k == "dynres") g_cfg.dynres = std::clamp(int(value), 0, 240);
      else if (k == "reach") g_cfg.reach = std::clamp(int(value), 0, 90);
      else if (k == "max_lights") g_cfg.maxLights = std::clamp(int(value), 0, 4096);
      else if (k == "flicker") g_cfg.flicker = std::clamp(int(value), 0, 2);
      else if (k == "smooth_models") g_cfg.smoothModels = value != 0;
      else if (k == "light_candidates") g_cfg.candidates = std::clamp(int(value), 4, 32);
      else if (k == "ev_min") g_cfg.evMin = std::clamp(int(value), 0, 7);
      else if (k == "light_response") g_cfg.response = std::clamp(int(value), 0, 2);
      else if (k == "rt_detection") g_cfg.rtDetection = value != 0;
      else if (k == "bloom") g_cfg.bloom = value != 0;
      else if (k == "light_fade") g_cfg.lightFade = value != 0;
      else if (k == "steady_faces") g_cfg.steadyFaces = value != 0;
      else if (k == "light_fix") g_cfg.lightFix = value != 0;
      else if (k == "fixtures_pass") g_cfg.fixturesPass = value != 0;
      else if (k == "ambient") g_cfg.ambient = std::clamp(value, 0.0f, 20.0f);
      else if (k == "light_cull") g_cfg.lightCull = value != 0;
      else if (k == "min_roughness") g_cfg.minRoughness = std::clamp(value, 0.0f, 1.0f);
      else if (k == "output_height") g_cfg.outputHeight = std::clamp(int(value), 0, 4320);
      else if (k == "perf_log") g_cfg.perfLog = value != 0;
      else if (k == "frame_cap") g_cfg.frameCap = std::clamp(int(value), 30, 240);
   }
   fclose(f);
   // settings saved before version 2: the flame glow was unused (0.35)
   if (version < 2)
      g_cfg.spriteGlow = kDefaultsSpriteGlow;
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
              "volumetric = %d\ndynres = %d\nmax_lights = %d\nmin_roughness = %.2f\nlight_cull = %d\nflicker = %d\nambient = %.3f\nsmooth_models = %d\nfixtures_pass = %d\nlight_fix = %d\nlight_response = %d\nev_min = %d\nlight_candidates = %d\nsteady_faces = %d\nlight_fade = %d\nbloom = %d\nrt_detection = %d\nreach = %d\noutput_height = %d\nperf_log = %d\nframe_cap = %d\nfog = %d\nmaterials = %d\nbump = %.2f\nparallax = %.3f\npolish = %d\ncrevice = %.2f\nfixture_light = %.2f\nsettings_version = 2\n",
           g_cfg.upscale, g_cfg.scale, int(g_cfg.fps), int(g_cfg.vsync), g_cfg.light, g_cfg.sky,
           g_cfg.lightSize, g_cfg.glow, g_cfg.water, g_cfg.spriteGlow, g_cfg.gi,
           int(g_cfg.volumetric), g_cfg.dynres, g_cfg.maxLights, g_cfg.minRoughness,
           int(g_cfg.lightCull), g_cfg.flicker, g_cfg.ambient, int(g_cfg.smoothModels),
           int(g_cfg.fixturesPass), int(g_cfg.lightFix), g_cfg.response, g_cfg.evMin,
           g_cfg.candidates, int(g_cfg.steadyFaces), int(g_cfg.lightFade), int(g_cfg.bloom), int(g_cfg.rtDetection), g_cfg.reach, g_cfg.outputHeight, int(g_cfg.perfLog), g_cfg.frameCap, g_cfg.fog, int(g_cfg.materials), g_cfg.bump, g_cfg.parallax, int(g_cfg.polish), g_cfg.crevice, g_cfg.fixtureLight);
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
   double waitSum = 0, drawSum = 0, waitMs = 0, drawMs = 0, captureSum = 0, captureMs = 0;
   double prepSum = 0, prepMs = 0;   // the game's preparations for the ray tracer
   std::string status;        // a line shown in the panel for a while
   double statusUntil = 0;
   int scroll = 0;            // first row shown, when the rows do not all fit
} g_ui;

// ---- textures ----------------------------------------------------------

namespace rtmat { struct Class; }

struct MatJob;

struct RtTexture {
   uint32_t serial = 0;
   char name[160] = {};
   // the image it was made from (resimginfo.h): its path and material file
   std::string source;
   float illumStrength = 0.0f;
   std::string illumMap;
   // what its material maps make it (rt_materials.h)
   bool polished = false, glows = false, mapsNone = false;
   const rtmat::Class *cls = nullptr;   // its material class (rt_materials.h)
   float glow = 0.0f;
   int width = 0, height = 0;
   std::vector<uint8_t> rgba;
   bool dirty = true;      // pixels changed since RTGL1 got them
   bool provided = false;  // RTGL1 has a texture of this name
   bool hasAlpha = false;  // any texel not fully opaque
   bool cutout = false;    // any texel see-through (alpha < 50%)
   bool white = false;     // the 1x1 white "no texture" texture
   // its material maps being made (off the main thread), until they are done
   // it is given to RTGL1 as it is, under its plain name
   std::shared_ptr<MatJob> job;
   // a hash of its pixels, for flame_texture (made once, not every frame)
   uint32_t pixelHash = 0;
   bool pixelHashValid = false;
};

void settle_job(RtTexture &t);

uint32_t g_textureSerial = 0;
std::deque<RtTexture> g_textures;   // never freed: world groups refer to them
std::vector<RtTexture *> g_freeTextures;
RtTexture *g_bound[2] = {};
int g_blend = kRenderBackendBlendOpaque;
bool g_alphaTest = false;

void copy_pixels(RtTexture &t, int width, int height, const void *rgba, int rowBytes)
{
   settle_job(t);   // (a job reads the pixels)
   t.pixelHashValid = false;
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

#include "rt_materials.h"

// RTGL1 names in use, and by how many textures: the same picture (named by
// its file and pixels) loaded twice shares one, and is taken back from RTGL1
// when the last of them goes
std::unordered_map<std::string, int> g_nameRefs;

void release_name(const char *name)
{
   auto it = g_nameRefs.find(name);
   if (it == g_nameRefs.end())
      return;
   if (--it->second <= 0) {
      if (g_active)
         rt.rgMarkOriginalTextureAsDeleted(name);
      g_nameRefs.erase(it);
   }
}

std::string g_matDir;

// a NewDark illum_map, by its resource path: in the fan mission, then the game
std::vector<uint8_t> *illum_image(const std::string &path, int &w, int &h)
{
   static std::unordered_map<std::string, std::pair<std::vector<uint8_t>, std::pair<int, int>>> s_cache;
   auto it = s_cache.find(path);
   if (it == s_cache.end()) {
      std::vector<uint8_t> px;
      int iw = 0, ih = 0;
      const char *roots[] = {DarkContentFMPath(), DarkContentBasePath()};
      bool ok = false;
      for (const char *root : roots) {
         if (!root || !*root)
            continue;
         ok = rtmat::read_png(std::string(root) + "/" + path + ".png", px, iw, ih);
         if (ok)
            break;
      }
      if (!ok)
         fprintf(stderr, "RT: material glow map %s.png not found\n", path.c_str());
      it = s_cache.emplace(path, std::make_pair(std::move(px), std::make_pair(iw, ih))).first;
   }
   w = it->second.second.first;
   h = it->second.second.second;
   return it->second.first.empty() ? nullptr : &it->second.first;
}

// ---- material maps, made off the main thread --------------------------------
//
// Making a texture's maps takes a while (an HD pack's 2048 texture: a second or
// so), and a mission has hundreds of textures: made as the first frame drew
// them, the game stood still for minutes. So each is made by a pool of
// threads; the texture is shown as it is meanwhile, and with its maps and its
// crevices from the frame after they are done. A small texture whose maps
// are made already is done at once (no frame without its bumps).

// this frame's texture work on the main thread (perf.log's slow frames)
double g_frameTexSeconds = 0;
int g_frameTexGiven = 0, g_frameMapsInline = 0;

constexpr int kAlbedoMax = 1024;

struct MatJob {
   // what it is made from: the texture's pixels stay as they are while the job
   // lives (settle_job before they change)
   const uint8_t *rgba = nullptr;
   int w = 0, h = 0;
   const rtmat::Class *cls = nullptr;
   std::string source, illumMap;
   const std::vector<uint8_t> *glowMap = nullptr;
   int gw = 0, gh = 0;
   float illumStrength = 0.0f, crevice = 0.0f;
   // what it makes
   char name[160] = {};
   rtmat::Result result;
   std::vector<uint8_t> creviced;   // its picture as given to RTGL1 (empty: the texture's own)
   int cw = 0, ch = 0;
   // 0 waiting, 1 being made, 2 done, 3 given up
   std::atomic<int> state{0};
};

// the job's RTGL1 name (its file, a hash of its pixels and class)
void job_name(MatJob &j)
{
   uint32_t h = rtmat::fnv(j.rgba, size_t(j.w) * j.h * 4);
   h = rtmat::fnv(&j.w, sizeof(j.w), h);
   h ^= j.cls->hash * 0x9e3779b1u;
   h ^= rtmat::kMapsVersion * 0x85ebca6bu;
   h ^= uint32_t(j.crevice * 20.0f + 0.5f) * 0xc2b2ae35u;
   if (j.illumStrength > 0)
      h = rtmat::fnv(j.illumMap.data(), j.illumMap.size(), h ^ uint32_t(j.illumStrength * 1000.0f));
   // (no dots: RTGL1 takes what follows the last dot for an extension and
   // drops it, and then finds none of the maps)
   snprintf(j.name, sizeof(j.name), "m_%s_%08x", rtmat::file_safe(j.source).c_str(), h);
}

void run_job(MatJob &j)
{
   if (!j.name[0])
      job_name(j);
   j.result = rtmat::prepare(g_matDir, j.name, *j.cls, j.rgba, j.w, j.h,
                             j.glowMap, j.gw, j.gh, j.illumStrength);
   // crevices darkened in its colour (rt_materials.h)
   if (j.crevice > 0.0f && !j.result.none && j.cls->bump > 0) {
      j.creviced.assign(j.rgba, j.rgba + size_t(j.w) * j.h * 4);
      rtmat::crevice_albedo(j.creviced, j.w, j.h, *j.cls, j.crevice);
   }
   j.cw = j.w;
   j.ch = j.h;
   // at most kAlbedoMax across: the path tracer traces some 1000 pixels
   // across, and an HD pack's 2048 textures cost their upload (a hitch) and
   // 16 MB each for detail it never shows
   if (std::max(j.w, j.h) > kAlbedoMax) {
      std::vector<uint8_t> small;
      const uint8_t *src = j.creviced.empty() ? j.rgba : j.creviced.data();
      const uint8_t *out = rtmat::shrink(src, j.w, j.h, kAlbedoMax, small, j.cw, j.ch);
      j.creviced.assign(out, out + size_t(j.cw) * j.ch * 4);
   }
}

struct MatPool {
   std::mutex m;
   std::condition_variable cv;
   std::deque<std::shared_ptr<MatJob>> queue;
   unsigned threads = 0;

   void push(std::shared_ptr<MatJob> job)
   {
      {
         std::lock_guard<std::mutex> lock(m);
         queue.push_back(std::move(job));
      }
      if (!threads) {
         // (left running to the end: the pool is never freed)
         const unsigned cores = std::thread::hardware_concurrency();
         threads = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 8u);
         for (unsigned i = 0; i < threads; ++i)
            std::thread([this] { work(); }).detach();
      }
      cv.notify_one();
   }

   void work()
   {
      for (;;) {
         std::shared_ptr<MatJob> job;
         {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [this] { return !queue.empty(); });
            job = std::move(queue.front());
            queue.pop_front();
         }
         int waiting = 0;
         if (!job->state.compare_exchange_strong(waiting, 1))
            continue;   // given up
         run_job(*job);
         job->state.store(2);
      }
   }

   size_t waiting()
   {
      std::lock_guard<std::mutex> lock(m);
      return queue.size();
   }
};

MatPool &mat_pool()
{
   static MatPool *s_pool = new MatPool;   // (never freed: its threads run on)
   return *s_pool;
}

// The texture's job given up (or waited for, when it is being made): its
// pixels are about to change or go.
void settle_job(RtTexture &t)
{
   if (!t.job)
      return;
   int waiting = 0;
   if (!t.job->state.compare_exchange_strong(waiting, 3))
      while (t.job->state.load() == 1)
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
   t.job.reset();
}

// the texture's material class, when it gets maps
const rtmat::Class *material_class(const RtTexture *t)
{
   if (!g_cfg.materials || t->source.empty() || g_rtDir.empty())
      return nullptr;
   if (g_matDir.empty()) {
      g_matDir = g_rtDir + "/mat_dev";
      mkdir(g_matDir.c_str(), 0755);
   }
   rtmat::load_table(g_rtDir);
   return rtmat::class_of(t->source);
}

// gives RTGL1 the picture under this name (or shares the one it has)
bool provide_texture(RtTexture *t, const char *name, const uint8_t *pixels, int w, int h)
{
   snprintf(t->name, sizeof(t->name), "%s", name);
   struct Timer {
      std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
      ~Timer() { g_frameTexSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
   } timer;
   auto ref = g_nameRefs.find(t->name);
   if (ref != g_nameRefs.end()) {
      ++ref->second;   // the same picture, already given to RTGL1
      return t->provided = true;
   }
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
      .pPixels = pixels,
      .size = {uint32_t(w), uint32_t(h)},
      .filter = RG_SAMPLER_FILTER_AUTO,
      .addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
   };
   t->provided = rt.rgProvideOriginalTexture(&info) == RG_RESULT_SUCCESS;
   ++g_frameTexGiven;
   if (t->provided)
      g_nameRefs[t->name] = 1;
   return t->provided;
}

// a finished job's texture, with its maps
void adopt_job(RtTexture *t)
{
   std::shared_ptr<MatJob> j = std::move(t->job);
   if (t->provided)
      release_name(t->name);
   t->provided = false;
   t->cls = j->cls;
   t->polished = j->result.polished;
   t->glows = j->result.glows;
   t->glow = j->result.glow;
   t->mapsNone = j->result.none;
   if (j->creviced.empty())
      provide_texture(t, j->name, t->rgba.data(), t->width, t->height);
   else
      provide_texture(t, j->name, j->creviced.data(), j->cw, j->ch);
}

bool g_preloading = false;
const char *texture_name(RtTexture *t);

// A mission's textures, all of them, mapped by the threads before its first
// frame (all cores; the frame waits): the walls are bump-mapped from the
// start instead of turning from flat to bumped in the first seconds, and
// none of them is mapped on the main thread during play.
void preload_textures()
{
   if (!g_cfg.materials || g_rtDir.empty())
      return;
   const auto t0 = std::chrono::steady_clock::now();
   std::vector<RtTexture *> started;
   g_preloading = true;
   for (RtTexture &t : g_textures)
      if (t.dirty && !t.white && !t.rgba.empty() && !t.source.empty()) {
         texture_name(&t);
         if (t.job)
            started.push_back(&t);
      }
   g_preloading = false;
   if (started.empty())
      return;
   size_t made = 0;
   for (RtTexture *t : started) {
      while (t->job && t->job->state.load() < 2)
         std::this_thread::sleep_for(std::chrono::milliseconds(2));
      ++made;
   }
   // given to RTGL1 now, each with its maps (texture_name adopts the jobs)
   for (RtTexture *t : started)
      texture_name(t);
   fprintf(stderr, "RT: material maps for %zu textures ready in %.1f s (%u threads)\n", made,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
           mat_pool().threads);
}

// The RTGL1 name for a texture, uploading its pixels first if they changed:
// its material name (its file, its pixels, its class) when it came from an
// image file and its maps are made; else dark.t<n>.
const char *texture_name(RtTexture *t)
{
   if (!t || t->white || t->rgba.empty())
      return nullptr;
   if (t->dirty) {
      settle_job(*t);
      if (t->provided)
         release_name(t->name);
      t->provided = false;
      t->polished = t->glows = t->mapsNone = false;
      t->glow = 0.0f;
      t->cls = nullptr;
      t->dirty = false;
      if (const rtmat::Class *c = material_class(t)) {
         auto j = std::make_shared<MatJob>();
         j->rgba = t->rgba.data();
         j->w = t->width;
         j->h = t->height;
         j->cls = c;
         j->source = t->source;
         j->illumMap = t->illumMap;
         j->illumStrength = t->illumStrength;
         j->crevice = g_cfg.crevice;
         if (t->illumStrength > 0 && !t->illumMap.empty())
            j->glowMap = illum_image(t->illumMap, j->gw, j->gh);   // (main thread: its cache)
         t->job = j;
         if (g_preloading) {
            // a mission loading (preload_textures): made by the threads, all
            // at once, and given to the path tracer when they all are
            mat_pool().push(j);
            return nullptr;
         }
         if (size_t(t->width) * t->height <= 256 * 256) {
            // small and its maps made already (on disk): at once, so it is
            // never seen flat. Making new maps waits for the threads (each
            // took ~30 ms here: the hitches as new textures came into view)
            const auto t0 = std::chrono::steady_clock::now();
            job_name(*j);
            if (rtmat::maps_made(g_matDir, j->name, *c)) {
               j->state.store(1);
               run_job(*j);
               j->state.store(2);
               g_frameTexSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
               ++g_frameMapsInline;
               adopt_job(t);
               return t->provided ? t->name : nullptr;
            }
            g_frameTexSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
         }
         mat_pool().push(j);
      }
      char plain[32];
      snprintf(plain, sizeof(plain), "dark.t%u", t->serial);
      if (t->job && std::max(t->width, t->height) > 256) {
         // a quick stand-in (every few texels) until its maps are made: a
         // big texture in full costs a hitch, and is soon given again
         int sw = t->width, sh = t->height, step = 1;
         while (std::max(sw, sh) > 256) {
            sw = std::max(1, sw / 2);
            sh = std::max(1, sh / 2);
            step *= 2;
         }
         std::vector<uint8_t> small(size_t(sw) * sh * 4);
         for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
               memcpy(&small[(size_t(y) * sw + x) * 4],
                      &t->rgba[(size_t(std::min(y * step, t->height - 1)) * t->width +
                                std::min(x * step, t->width - 1)) * 4], 4);
         provide_texture(t, plain, small.data(), sw, sh);
      } else {
         provide_texture(t, plain, t->rgba.data(), t->width, t->height);
      }
   }
   if (t->job && t->job->state.load() == 2)
      adopt_job(t);
   return t->provided ? t->name : nullptr;
}

// A flame sprite: its alpha texture again under a name of its own, with an
// emission map of its colour within its soft shape (glowing as a whole, the
// sprite's square showed). Kept for the session.
const char *flame_texture(RtTexture *base, float r, float g, float b)
{
   const char *baseName = texture_name(base);
   if (!baseName || g_rtDir.empty() || !g_cfg.materials || g_cfg.spriteGlow <= 0.0f)
      return nullptr;
   const float k = std::clamp(g_cfg.spriteGlow, 0.0f, 1.0f);
   const int qr = int(std::clamp(r * k, 0.0f, 1.0f) * 15.0f + 0.5f);
   const int qg = int(std::clamp(g * k, 0.0f, 1.0f) * 15.0f + 0.5f);
   const int qb = int(std::clamp(b * k, 0.0f, 1.0f) * 15.0f + 0.5f);
   char name[200];
   if (!base->pixelHashValid) {
      base->pixelHash = rtmat::fnv(base->rgba.data(), base->rgba.size());
      base->pixelHashValid = true;
   }
   snprintf(name, sizeof(name), "flame2_%08x_%x%x%x", base->pixelHash, qr, qg, qb);
   static std::unordered_set<std::string> s_made;
   if (!s_made.count(name)) {
      if (g_matDir.empty()) {
         g_matDir = g_rtDir + "/mat_dev";
         mkdir(g_matDir.c_str(), 0755);
      }
      std::vector<uint8_t> tinted = base->rgba;
      const float tint[3] = {qr / 15.0f, qg / 15.0f, qb / 15.0f};
      for (size_t i = 0; i < tinted.size(); i += 4)
         for (int c = 0; c < 3; ++c)
            tinted[i + c] = uint8_t(std::clamp(tinted[i + c] * tint[c], 0.0f, 255.0f));
      if (!rtmat::prepare_flame(g_matDir, name, tinted.data(), base->width, base->height))
         return nullptr;
      auto details = RgOriginalTextureDetailsEXT{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
         .pNext = nullptr, .flags = 0, .format = RG_FORMAT_R8G8B8A8_SRGB,
      };
      auto info = RgOriginalTextureInfo{
         .sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
         .pNext = &details,
         .pTextureName = name,
         .pPixels = base->rgba.data(),
         .size = {uint32_t(base->width), uint32_t(base->height)},
         .filter = RG_SAMPLER_FILTER_AUTO,
         .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
         .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
      };
      if (rt.rgProvideOriginalTexture(&info) != RG_RESULT_SUCCESS)
         return nullptr;
      s_made.insert(name);
   }
   return s_made.find(name)->c_str();
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
bool g_freeze = false;         // the models as they were (test)
bool g_freezeLights = false;   // the lights as they were (test)
int g_freezeMode = 0;          // 0 off, 1 both, 2 models only, 3 lights only
// Test switch (panel): faces turned away from the camera, which the game culls,
// are dropped instead of kept for shadows.
bool g_dropBackFaces = false;
// Test switch (panel): how the path tracer reuses light samples between
// pixels and frames. 0 normal, 1 no spatial reuse, 2 no reuse at all,
// 3 normal with 8 shadow rays and 4 light samples per pixel.
int g_reuseTest = 0;
bool g_blinkTest = false;   // the light nearest the camera goes off and on (test)
uint32_t g_blinkId = ~0u;
struct PendingLight {
   uint32_t id;
   float pos[3], rgb[3], dir[3];
   float radius, inner, outer, peak, score = 0;
   bool reaches = true;   // shines on a cell near the camera or in view
   float walk = -2.0f;    // metres from the camera through the openings; -1 out of reach, -2 not known
};
std::vector<PendingLight> g_pendingLights;
// lights a lamp's glowing glass (or a flame's bowl) was seen at: a lamp, so
// it lights its surroundings as well as the cone the mapper gave it
std::unordered_set<uint32_t> g_fixtureLights;
// of those, a flame's (a torch's, a brazier's): warm, and flickering
std::unordered_set<uint32_t> g_flameLights;
// lights moved out of walls (see fix_light_position)
struct LightFix {
   float offset[3];   // meters
   float before;      // clearance found, meters
   bool done;
};
std::unordered_map<uint64_t, LightFix> g_lightFixes;
uint32_t g_lightsTotal = 0, g_frameLightsShown = 0, g_lightsReaching = 0;
std::vector<PendingLight> g_lastLights;   // last frame's, for the inspector
std::vector<PendingLight> g_lastOffered;  // all the game gave last frame, before culling
std::vector<PendingLight> g_dimPending, g_lastDim;   // too dim to trace (off)
int g_lightEventFrames = 0;   // a light just put out or lit: the denoiser lets go for a few frames
std::vector<PendingLight> g_fadingOut;   // left the traced set, still fading out
std::vector<PendingLight> g_frozenLights;  // while g_freeze
bool g_underwater = false;
// the mission's fog for this frame (a fog zone's, eased): colour 0-255 and the
// distance in feet at which it is opaque, 0 none (portal.c, fogzone.h)
float g_fogRgb[3] = { 0, 0, 0 };
float g_fogDistFt = 0;
int g_drawableW = 1, g_drawableH = 1;
uint32_t g_framePrims = 0, g_frameDropped = 0, g_frameLights = 0;
// this frame's light positions (meters), for making light fixtures glow
std::vector<RgFloat3D> g_lightPositions;
// the level's ambient light (0..1), and the glow it gives surfaces
float g_levelAmbient = 0.0f;
// the player's light, by the game and by the path tracer (for the frame counter)
float g_noteOriginal = -1, g_noteTraced = -1;
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
   float bmin[3] = {}, bmax[3] = {};   // meters
   bool passLight = false;   // a small fixture around a light: it does not block it
   bool settled = false;     // prims made steady and given normals for hash
   bool keptOut = false;     // given out of view last frame
   uint32_t givenFrame = 0;  // last given to the path tracer
   size_t givenTris = 0;     // how many triangles it had when last given
   // each texture's look as a face turned toward the camera (flags, glow,
   // colour), for its faces captured turned away: kept across frames
   std::unordered_map<std::string, CapturedPrim> looks;
};

// cached objects farther than this from the camera are not drawn out of view
constexpr float kKeepDistance = 60.0f;

// The log of models near the camera that leave the traced scene or lose much
// of their shape (logs/thief.log), to find what makes models pop in and out.
int g_modelLogLines = 0;
constexpr int kModelLogMax = 400;
void log_model_change(uint32_t obj, const char *what, ...);

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
uint32_t g_emptyCaptures = 0, g_pieceCaptures = 0;   // for the inspector
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

// The game draws the faces of a model that are turned toward the camera; the
// ones turned away were captured too, as shadow-only. Which is which changed
// with every step: a face the path tracer's bounce light (and what it sees
// in a reflection) did not see one moment, it did the next, and a glass pane
// captured from behind was a solid shadow caster -- the shadows and light
// that came and went as you walked up to a lamp (frozen models: steady).
// So every face is given as seen, by the look its texture has from the front;
// only one of each pair of coincident opposite faces (a two-sided part) stays
// shadow-only, chosen by a fixed rule, so the two do not fight. The order of
// everything is fixed too, so the same model gives the same primitives.
void steady_faces(CachedObject &c)
{
   for (const CapturedPrim &p : c.prims)
      if (!(p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY) && !p.sprite) {
         CapturedPrim look = p;
         look.verts.clear();
         c.looks[p.texture] = look;
      }
   struct Tri {
      std::array<int64_t, 9> key;   // the corners, 2 mm grid, sorted
      float n[3];
      const CapturedPrim *from;
      size_t at;
      bool shadowOnly;
   };
   std::vector<Tri> tris;
   std::vector<CapturedPrim> keep;   // sprites: as they are
   for (const CapturedPrim &p : c.prims) {
      if (p.sprite) {
         keep.push_back(p);
         continue;
      }
      for (size_t t = 0; t + 2 < p.verts.size(); t += 3) {
         Tri tri{};
         std::array<std::array<int64_t, 3>, 3> q;
         for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
               q[k][j] = int64_t(std::llround(p.verts[t + k].position[j] * 500.0f));
         std::sort(q.begin(), q.end());
         for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
               tri.key[k * 3 + j] = q[k][j];
         const float *a = p.verts[t].position, *b = p.verts[t + 1].position, *e = p.verts[t + 2].position;
         const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
         const float e2[3] = {e[0] - a[0], e[1] - a[1], e[2] - a[2]};
         tri.n[0] = e1[1] * e2[2] - e1[2] * e2[1];
         tri.n[1] = e1[2] * e2[0] - e1[0] * e2[2];
         tri.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
         tri.from = &p;
         tri.at = t;
         tris.push_back(tri);
      }
   }
   // a fixed order: by corners, then by facing
   const float ref[3] = {0.267f, 0.535f, 0.802f};
   auto facing = [&ref](const Tri &t) { return t.n[0] * ref[0] + t.n[1] * ref[1] + t.n[2] * ref[2]; };
   std::sort(tris.begin(), tris.end(), [&](const Tri &x, const Tri &y) {
      if (x.key != y.key)
         return x.key < y.key;
      return facing(x) > facing(y);
   });
   // the same corners again: a coincident face; facing the other way, it is
   // the back of a two-sided part and only casts shadows
   for (size_t i = 1; i < tris.size(); ++i)
      if (tris[i].key == tris[i - 1].key) {
         const float d = tris[i].n[0] * tris[i - 1].n[0] + tris[i].n[1] * tris[i - 1].n[1] +
                         tris[i].n[2] * tris[i - 1].n[2];
         tris[i].shadowOnly = true;   // the same way, a duplicate: shadow-only too
         (void)d;
      }
   // regroup: by texture and final look, in a fixed order
   std::map<std::pair<std::string, int>, CapturedPrim> out;
   for (const Tri &t : tris) {
      const CapturedPrim &src = *t.from;
      const bool wasShadow = (src.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY) != 0;
      const CapturedPrim *look = &src;
      bool shadowOnly = t.shadowOnly;
      if (wasShadow && !shadowOnly) {
         auto f = c.looks.find(src.texture);
         if (f == c.looks.end())
            shadowOnly = true;   // never seen from the front yet: as it was
         else
            look = &f->second;
      }
      const std::pair<std::string, int> key{src.texture, shadowOnly ? 1 : 0};
      auto it = out.find(key);
      if (it == out.end()) {
         CapturedPrim p = shadowOnly ? src : *look;
         p.verts.clear();
         if (shadowOnly) {
            p.flags = RG_MESH_PRIMITIVE_SHADOW_ONLY | (src.flags & RG_MESH_PRIMITIVE_ALPHA_TESTED);
            p.emissive = 0.0f;
         }
         it = out.emplace(key, std::move(p)).first;
      }
      for (int k = 0; k < 3; ++k)
         it->second.verts.push_back(src.verts[t.at + k]);
   }
   std::vector<CapturedPrim> prims;
   for (auto &e : out)
      prims.push_back(std::move(e.second));
   for (CapturedPrim &p : keep)
      prims.push_back(std::move(p));
   c.prims = std::move(prims);
}

// the same as hash_prims, but the same for the same triangles in any order
uint64_t hash_prims_unordered(const std::vector<CapturedPrim> &prims)
{
   uint64_t sum = 0, x = 0;
   for (const CapturedPrim &p : prims) {
      if (p.sprite)
         continue;
      uint64_t th = 1469598103934665603ull;
      for (unsigned char ch : p.texture)
         th = (th ^ ch) * 1099511628211ull;
      for (size_t t = 0; t + 2 < p.verts.size(); t += 3) {
         uint64_t h = th;
         uint64_t corner[3];
         for (int k = 0; k < 3; ++k) {
            uint64_t ch = 0;
            for (int j = 0; j < 3; ++j)
               ch = ch * 1000003ull + uint64_t(int64_t(std::lround(p.verts[t + k].position[j] * 256.0f)));
            // and where its texture lies on it: a scrolling texture (running
            // water) changes only that, and a model taken as unchanged froze
            for (int j = 0; j < 2; ++j)
               ch = ch * 1000003ull + uint64_t(int64_t(std::lround(p.verts[t + k].texCoord[j] * 1024.0f)));
            corner[k] = ch;
         }
         std::sort(corner, corner + 3);
         for (uint64_t ch : corner)
            h = (h ^ ch) * 1099511628211ull;
         h ^= h >> 29;
         sum += h;
         x ^= h * 0x9E3779B97F4A7C15ull;
      }
   }
   return sum ^ (x << 1);
}

void upload_captured(uint64_t id, const CapturedPrim &p, bool passLight = false)
{
   // a torch's own model sits around its light: as a shadow caster it put
   // the light out (lit or doused, the room looked the same)
   if (passLight && (p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY))
      return;
   // Flat: each face shaded by its own direction. RTGL1 bends a blended
   // normal toward the viewer where it disagrees with the face (to keep
   // reflections out of the surface), so smooth shading changes with the
   // angle a model is seen from -- light that came and went as you moved.
   const RgMeshPrimitiveFlags shading =
      g_cfg.smoothModels ? 0u : RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS);
   // A glowing part (a lamp's bulb, a torch's flame cup) as Blood RT's: it
   // looks lit, but gives the bounce light only a little of it -- the light
   // it stands for is traced already, and a small bright emitter that only
   // random bounce rays find is a splotch on every wall around it.
   const bool opaque = !(p.flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_SHADOW_ONLY)) && !p.sprite;
   const float ambient = opaque ? ambient_emissive() : 0.0f;
   const bool glows = p.emissive > 0.0f && opaque;
   const RgMeshPrimitiveFlags glowFlags =
      glows ? RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE | RG_MESH_PRIMITIVE_EMISSIVE_SCREEN_SCALED) : 0u;
   auto prim = RgMeshPrimitiveInfo{
      .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
      .pNext = nullptr,
      .flags = p.flags | shading | glowFlags |
               (passLight ? RgMeshPrimitiveFlags(RG_MESH_PRIMITIVE_NO_SHADOW) : 0u),
      .primitiveIndexInMesh = 0,
      .pVertices = p.verts.data(),
      .vertexCount = uint32_t(p.verts.size()),
      .pIndices = nullptr,
      .indexCount = 0,
      .pTextureName = p.texture.empty() ? nullptr : p.texture.c_str(),
      .textureFrame = 0,
      .color = p.color,
      .emissive = std::max(p.emissive, ambient),
      .classicLight = 1.0f,
      .emissiveGi = glows ? std::max(p.emissive * 0.06f, ambient) : 0.0f,
   };
   upload(id, prim);
}

uint32_t g_cachedDrawn = 0;
// this frame's models for the performance log
uint32_t g_perfModels = 0, g_perfPrims = 0;
size_t g_perfTris = 0;

// A model's primitive named by its texture and kind, the n-th such in the
// model, not by its place in the list: the list's order changed as faces
// turned toward or away, and RTGL1 matched the wrong primitives between
// frames (a walking character smeared).
std::vector<uint64_t> object_prim_ids(uint32_t obj, const std::vector<CapturedPrim> &prims)
{
   std::vector<uint64_t> ids(prims.size());
   std::unordered_map<uint32_t, uint32_t> seen;   // key -> how many so far
   std::vector<bool> used(4096, false);
   for (size_t i = 0; i < prims.size(); ++i) {
      uint32_t h = 2166136261u;
      for (unsigned char ch : prims[i].texture)
         h = (h ^ ch) * 16777619u;
      h = (h ^ uint32_t(prims[i].flags)) * 16777619u;
      const uint32_t nth = seen[h]++;
      uint32_t slot = (h + nth * 2654435761u) & 0xfffu;
      for (int tries = 0; used[slot] && tries < 4096; ++tries)
         slot = (slot + 1) & 0xfffu;
      used[slot] = true;
      ids[i] = 0x500000000ull + (uint64_t(obj) << 12) + slot;
   }
   return ids;
}

void upload_objects()
{
   g_cachedDrawn = 0;
   g_perfModels = g_perfPrims = 0;
   g_perfTris = 0;
   if (g_freeze) {
      // the test switch: the models exactly as they were when it was set
      for (const auto &e : g_objects) {
         const std::vector<uint64_t> ids = object_prim_ids(e.first, e.second.prims);
         for (size_t i = 0; i < e.second.prims.size(); ++i)
            upload_captured(ids[i], e.second.prims[i], e.second.passLight);
      }
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
         // How far out of view a model is still given, for its shadow: by its
         // size (a lamp post's reaches far, a bottle's does not), with some
         // slack once given so models do not come and go at the edge -- each
         // one coming or going made the path tracer rebuild a piece of the
         // scene, and a long walk round the city gave it more and more.
         const float ext = std::max({c.bmax[0] - c.bmin[0], c.bmax[1] - c.bmin[1], c.bmax[2] - c.bmin[2], 0.0f});
         // (a bottle or a cabbage out of view shadows nothing seen from more
         // than a few metres: Black Parade's markets kept ~400 such models,
         // 8000 primitives a frame, costing milliseconds of uploading)
         // (4 m + 8 m per metre of size, at most 25 m: some 280 models were
         // kept out of view in a fan mission's streets, 5000 primitives
         // uploaded every frame for shadows mostly nobody saw)
         const float keepBase = std::clamp(4.0f + 8.0f * ext, 4.0f, 25.0f);
         const float keepAt = keepBase + (c.keptOut ? std::min(8.0f, 0.5f * keepBase) : 0.0f);
         if (dx * dx + dy * dy + dz * dz > std::min(keepAt, kKeepDistance) * std::min(keepAt, kKeepDistance)) {
            if (c.givenFrame + 1 == g_frameNumber && dx * dx + dy * dy + dz * dz < 20.0f * 20.0f)
               log_model_change(obj, "leaves the scene: not drawn by the game for %u frames, %.1f m away (kept to %.1f m)",
                                g_frameNumber - c.frame, std::sqrt(dx * dx + dy * dy + dz * dz),
                                std::min(keepAt, kKeepDistance));
            c.keptOut = false;
            ++it;   // remembered, but too far away to shadow anything seen
            continue;
         }
         c.keptOut = true;
         // a texture of its gone (freed by the game: a mission or a saved
         // game loaded since): it would be kept, untextured and grey, forever
         bool stale = false;
         for (const CapturedPrim &p : c.prims)
            if (p.tex && (p.tex->serial != p.texSerial || p.tex->rgba.empty())) {
               stale = true;
               break;
            }
         const bool keep = !stale && c.posed && RenderRTGameObjectPose(obj, pose) &&
                           std::fabs(pose[0] - c.pose[0]) < 0.01f &&
                           std::fabs(pose[1] - c.pose[1]) < 0.01f &&
                           std::fabs(pose[2] - c.pose[2]) < 0.01f &&
                           pose[3] == c.pose[3] && pose[4] == c.pose[4] && pose[5] == c.pose[5];
         if (!keep) {
            if (c.givenFrame + 1 == g_frameNumber && dx * dx + dy * dy + dz * dz < 20.0f * 20.0f)
               log_model_change(obj, "leaves the scene: not drawn by the game this frame and %s, %.1f m away",
                                c.posed ? "moved since it was last drawn" : "no position known",
                                std::sqrt(dx * dx + dy * dy + dz * dz));
            note_dropped(obj, c.posed ? "moved since it was last drawn" : "no position known");
            it = g_objects.erase(it);
            continue;
         }
         draw = true;
         ++g_cachedDrawn;
      }
      const bool givenLast = c.givenFrame + 1 == g_frameNumber;
      c.givenFrame = g_frameNumber;
      c.passLight = false;
      if (g_cfg.fixturesPass) {
         const float ext = std::max({c.bmax[0] - c.bmin[0], c.bmax[1] - c.bmin[1], c.bmax[2] - c.bmin[2]});
         if (ext < 1.0f)
            for (const RgFloat3D &l : g_lightPositions) {
               bool in = true;
               for (int k = 0; k < 3 && in; ++k)
                  in = l.data[k] > c.bmin[k] - 0.15f && l.data[k] < c.bmax[k] + 0.15f;
               if (in) {
                  c.passLight = true;
                  break;
               }
            }
      }
      size_t tris = 0;
      for (const CapturedPrim &p : c.prims)
         if (!(p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY))
            tris += p.verts.size() / 3;
      // (off: a model's faces turning between seen and shadow-only made most
      // of these; the pop-in they were for is found)
      if (false && givenLast && c.givenTris > 0 && tris * 10 < c.givenTris * 7) {
         const RgFloat3D cam = to_rt(g_camera.pos);
         const float dx = c.center[0] - cam.data[0], dy = c.center[1] - cam.data[1], dz = c.center[2] - cam.data[2];
         if (dx * dx + dy * dy + dz * dz < 20.0f * 20.0f)
            log_model_change(obj, "shrinks from %zu to %zu seen triangles (%s, drawn %s)", c.givenTris, tris,
                             c.whole ? "whole" : "in pieces", c.frame == g_frameNumber ? "this frame" : "earlier, kept");
      }
      c.givenTris = tris;
      ++g_perfModels;
      g_perfPrims += uint32_t(c.prims.size());
      for (const CapturedPrim &p : c.prims)
         g_perfTris += p.verts.size() / 3;
      const std::vector<uint64_t> ids = object_prim_ids(obj, c.prims);
      for (size_t i = 0; i < c.prims.size(); ++i)
         upload_captured(ids[i], c.prims[i], c.passLight);
      ++it;
   }
}

void log_model_change(uint32_t obj, const char *what, ...)
{
   if (g_modelLogLines >= kModelLogMax)
      return;
   if (++g_modelLogLines == kModelLogMax) {
      fprintf(stderr, "RT: (no more model changes logged)\n");
      return;
   }
   char name[128];
   RenderRTGameObjectName(obj, name, sizeof(name));
   char text[256];
   va_list args;
   va_start(args, what);
   vsnprintf(text, sizeof(text), what, args);
   va_end(args);
   fprintf(stderr, "RT: model %u \"%s\" %s (frame %u)\n", obj, name, text, g_frameNumber);
}

// ---- world representation -------------------------------------------------

// The world in pieces: one per texture in each 24 m block of the mission, so
// rooms far from the camera can be left out (Raze RT 0077: the biggest saving
// on big maps -- every mission's every polygon was traced, every frame).
struct WorldGroup {
   int textureId;
   int flags;
   std::vector<RgPrimitiveVertex> verts;
   std::vector<uint32_t> indices;
   std::vector<int> cells;           // the cells its polygons are in
   float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};   // meters
   bool shown = false;               // given to the path tracer last frame
};
constexpr float kWorldChunk = 24.0f;      // meters
constexpr float kWorldReach = 60.0f;      // meters walked from the camera (as Raze RT)
constexpr float kWorldNear = 20.0f;       // meters in a straight line: always (shadows, reflections)
int g_worldCell = -1;
uint32_t g_worldShownGroups = 0;
uint32_t g_worldSerial = 0;
bool g_worldValid = false;
std::vector<WorldGroup> g_world;
std::unordered_map<uint64_t, size_t> g_worldIndex;
std::unordered_map<int, RtTexture *> g_worldTextures;

bool g_worldFresh = false;   // a new world: its textures to preload

void upload_world()
{
   if (!g_worldValid)
      return;
   if (g_worldFresh) {
      g_worldFresh = false;
      preload_textures();
   }
   static uint32_t s_reported = 0;
   size_t tris = 0, cutTris = 0;
   g_worldShownGroups = 0;
   const RgFloat3D cam = to_rt(g_camera.pos);
   for (size_t i = 0; i < g_world.size(); ++i) {
      WorldGroup &g = g_world[i];
      if (g.verts.empty() || (g.flags & kRenderRTWorldSky))
         continue;
      // a room far from the camera (walking, through the openings) is left
      // out, unless near in a straight line (its shadows and reflections);
      // once given, kept a little farther, so pieces do not come and go
      {
         const float reachFt = (g.shown ? kWorldReach * 1.25f : kWorldReach) / kMetersPerUnit;
         bool in = g.cells.empty();
         for (int c : g.cells) {
            const float w = portal_rt_cell_walk(c);
            if (w >= 0.0f && w < reachFt) {
               in = true;
               break;
            }
         }
         if (!in) {
            float d2 = 0;
            for (int k = 0; k < 3; ++k) {
               const float d = std::max({g.bmin[k] - cam.data[k], 0.0f, cam.data[k] - g.bmax[k]});
               d2 += d * d;
            }
            const float near = g.shown ? kWorldNear * 1.25f : kWorldNear;
            in = d2 < near * near;
         }
         g.shown = in;
         if (!in)
            continue;
         ++g_worldShownGroups;
      }
      auto found = g_worldTextures.find(g.textureId);
      RtTexture *tex = found == g_worldTextures.end() ? nullptr : found->second;
      // (as Blood RT: the walls do not move, and each face is shaded flat)
      RgMeshPrimitiveFlags flags = RG_MESH_PRIMITIVE_NO_MOTION_VECTORS | RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS;
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
      const char *texName = texture_name(tex);
      // smooth, polished surfaces (marble, glazed tile, metal): sharp traced
      // mirror reflections over their texture (the material table)
      if (tex && tex->polished && g_cfg.polish && g_cfg.materials &&
          !(flags & (RG_MESH_PRIMITIVE_WATER | RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_ALPHA_TESTED)))
         flags |= RG_MESH_PRIMITIVE_POLISHED;
      auto prim = RgMeshPrimitiveInfo{
         .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
         .pNext = nullptr,
         .flags = flags,
         .primitiveIndexInMesh = 0,
         .pVertices = g.verts.data(),
         .vertexCount = uint32_t(g.verts.size()),
         .pIndices = g.indices.data(),
         .indexCount = uint32_t(g.indices.size()),
         .pTextureName = texName,
         .textureFrame = 0,
         .color = pack_color(1, 1, 1, alpha),
         // (a glowing texture -- a lit window -- also lights its surroundings)
         .emissive = (g.flags & kRenderRTWorldFullbright) ? 0.25f
                     : (flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_SKY)) ? 0.0f
                     : (tex && tex->glows) ? std::max(ambient_emissive(), tex->glow)
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
   if (env_float("THIEF_RT_PERF", 0) != 0)
      g_cfg.perfLog = true;
   // the GPU's time per pass (in logs/thief.log every 2 s) with the
   // performance log; read by the library when it starts
   if (env_float("THIEF_RT_PROFILE", 0) != 0 || g_cfg.perfLog)
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

   g_sdlWindow = window;
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

// the image the next texture is made from (lgd3d says, before it makes it)
const void *g_pendingSource = nullptr;

void take_source(RtTexture *t)
{
   t->source.clear();
   t->illumMap.clear();
   t->illumStrength = 0.0f;
   sResImageInfo info;
   if (g_pendingSource && ResImageInfoFind(g_pendingSource, &info) && info.name[0]) {
      t->source = info.name;
      if (info.illumStrength > 0 && info.illumMap[0]) {
         t->illumStrength = info.illumStrength;
         t->illumMap = info.illumMap;
      }
   }
   g_pendingSource = nullptr;
}

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
   take_source(t);
   copy_pixels(*t, width, height, rgba, rowBytes);
   return t;
}

int UpdateTexture(void *handle, int width, int height, const void *rgba, int rowBytes)
{
   RtTexture *t = static_cast<RtTexture *>(handle);
   if (!t || width <= 0 || height <= 0 || !rgba || rowBytes < width * 4)
      return 0;
   if (g_pendingSource)
      take_source(t);
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
   settle_job(*t);
   if (t->provided)
      release_name(t->name);
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
#if defined(__APPLE__)
// The frame's size: the screen's own pixels, or fewer (g_cfg.outputHeight)
// with the system scaling the frame up to the window -- as Blood RT's 1800p.
// The upscaler's output, and everything done at the output's size (bloom,
// tone mapping, the 2D overlay), shrink with it.
struct RtCGSize { double width, height; };
void apply_output_size()
{
   if (!g_metalView || !g_sdlWindow)
      return;
   using SelFn = void *(*)(const char *);
   static SelFn sel = reinterpret_cast<SelFn>(dlsym(RTLD_DEFAULT, "sel_registerName"));
   static void *msg = dlsym(RTLD_DEFAULT, "objc_msgSend");
   if (!sel || !msg)
      return;
   void *layer = SDL_Metal_GetLayer(g_metalView);
   int pw = 0, ph = 0, nw = 0, nh = 0;
   SDL_GetWindowSize(g_sdlWindow, &pw, &ph);
   SDL_GetWindowSizeInPixels(g_sdlWindow, &nw, &nh);
   if (!layer || pw <= 0 || ph <= 0 || nh <= 0)
      return;
   double scale = double(nh) / double(ph);
   if (g_cfg.outputHeight > 0 && g_cfg.outputHeight < nh)
      scale = double(g_cfg.outputHeight) / double(ph);
   const RtCGSize want{std::floor(pw * scale), std::floor(ph * scale)};
   const RtCGSize have = reinterpret_cast<RtCGSize (*)(void *, void *)>(msg)(layer, sel("drawableSize"));
   if (std::fabs(have.width - want.width) < 1.0 && std::fabs(have.height - want.height) < 1.0)
      return;
   reinterpret_cast<void (*)(void *, void *, double)>(msg)(layer, sel("setContentsScale:"), scale);
   reinterpret_cast<void (*)(void *, void *, RtCGSize)>(msg)(layer, sel("setDrawableSize:"), want);
}
#else
void apply_output_size() {}
#endif

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
   kRowUpscale, kRowScale, kRowOutput, kRowDynres, kRowBounces, kRowReach, kRowLights, kRowLightFade, kRowCandidates, kRowLightCull, kRowResponse, kRowFlicker, kRowVolumetric, kRowFog, kRowBloom, kRowVsync, kRowFrameCap,
   kRowFps, kRowDetection, kRowLight, kRowExposure, kRowAmbient, kRowSky, kRowLightSize, kRowRough, kRowSmooth, kRowSteady, kRowFixtures, kRowFixtureLight, kRowLightFix, kRowGlow, kRowFlameGlow, kRowMaterials, kRowBump, kRowCrevice, kRowParallax, kRowPolish, kRowWater,
   kRowDebug, kRowFreeze, kRowBackFaces, kRowReuse, kRowBlink, kRowPerf, kRowInspect, kRowReset, kRowCount
};

const char *kRowLabels[kRowCount] = {
   "Upscaler", "Render resolution", "Output resolution", "Dynamic resolution", "Bounce light", "Light reach", "Lights traced",
   "Lights coming into range", "Lights tried per pixel", "Lights behind walls", "Light changes", "Light flicker",
   "Volumetric light", "Mission fog", "Bloom", "VSync", "Frame rate limit", "FPS counter", "Light gem and guards", "Light brightness", "Darkest exposure", "Ambient light", "Sky brightness",
   "Light size (soft shadows)", "Surface roughness", "Model shading", "Model faces", "Torches and lamps", "Lamp and torch light", "Lights in walls", "Lamp glow", "Flame glow", "Materials", "Bumps", "Crevices", "Parallax depth", "Polished reflections", "Water",
   "Debug view", "Freeze scene (test)", "Back faces (test)", "Light sampling (test)", "Blink nearest light (test)", "Performance log", "Inspect centre of view", "Reset to defaults",
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
   case kRowOutput:
      if (g_cfg.outputHeight <= 0)
         snprintf(buf, sizeof(buf), "Screen's own  %dx%d", g_drawableW, g_drawableH);
      else
         snprintf(buf, sizeof(buf), "%dp  %dx%d", g_cfg.outputHeight, g_drawableW, g_drawableH);
      return buf;
   case kRowDynres:
      if (g_cfg.dynres <= 0) return "Off";
      snprintf(buf, sizeof(buf), "aim for %d fps", g_cfg.dynres);
      return buf;
   case kRowBounces:
      return g_cfg.gi == 0 ? "2, half rate" : g_cfg.gi == 1 ? "1" : g_cfg.gi == 2 ? "2" : "1, half rate";
   case kRowReach:
      if (g_cfg.reach <= 0) return "Off (nearest lights)";
      snprintf(buf, sizeof(buf), "%d m walk, fading", g_cfg.reach);
      return buf;
   case kRowLights:
      if (g_cfg.maxLights <= 0) return "All";
      snprintf(buf, sizeof(buf), "nearest %d", g_cfg.maxLights);
      return buf;
   case kRowLightCull: return g_cfg.lightCull ? "Skipped" : "Traced";
   case kRowLightFade: return g_cfg.lightFade ? "Fade in" : "Appear at once";
   case kRowCandidates: snprintf(buf, sizeof(buf), "%d", g_cfg.candidates); return buf;
   case kRowResponse: return g_cfg.response == 0 ? "As Blood RT" : g_cfg.response == 2 ? "Instant (noisier)" : "Quick";
   case kRowFlicker: return g_cfg.flicker == 1 ? "As the game" : g_cfg.flicker == 2 ? "Nearly steady" : "Smoothed";
   case kRowVolumetric: return g_cfg.volumetric ? "On" : "Off";
   case kRowFog:
      return g_cfg.fog == 0 ? "Off" : g_cfg.fog == 1 ? "Light" : g_cfg.fog == 3 ? "Thick" : "As the mission";
   case kRowBloom: return g_cfg.bloom ? "On" : "Off";
   case kRowVsync: return g_cfg.vsync ? "On" : "Off";
   case kRowFps: return g_cfg.fps ? "On" : "Off";
   case kRowLight: snprintf(buf, sizeof(buf), "%.2fx", g_cfg.light); return buf;
   case kRowDetection: return g_cfg.rtDetection ? "See the ray-traced light" : "Original lighting";
   case kRowExposure:
      snprintf(buf, sizeof(buf), "EV %d%s", g_cfg.evMin, g_cfg.evMin == 5 ? " (Blood RT)" : g_cfg.evMin == 0 ? " (brightest)" : "");
      return buf;
   case kRowAmbient:
      if (g_cfg.ambient <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2fx the level's", g_cfg.ambient);
      return buf;
   case kRowSky: snprintf(buf, sizeof(buf), "%.2fx", g_cfg.sky); return buf;
   case kRowLightSize: snprintf(buf, sizeof(buf), "%.2f m", g_cfg.lightSize); return buf;
   case kRowSmooth: return g_cfg.smoothModels ? "Smooth" : "Flat (steady)";
   case kRowLightFix: return g_cfg.lightFix ? "Moved out" : "Left as placed";
   case kRowSteady: return g_cfg.steadyFaces ? "Steady (all seen)" : "As the game draws them";
   case kRowFixtures: return g_cfg.fixturesPass ? "Let their light out" : "Block their own light";
   case kRowFreeze: {
      static const char *kModes[] = {"Off", "Models and lights", "Models only", "Lights only"};
      return kModes[std::clamp(g_freezeMode, 0, 3)];
   }
   case kRowBackFaces: return g_dropBackFaces ? "Dropped" : "Shadows only";
   case kRowBlink: return g_blinkTest ? "On" : "Off";
   case kRowFrameCap:
      snprintf(buf, sizeof(buf), "%d fps%s", g_cfg.frameCap, g_cfg.frameCap == 60 ? " (as the game)" : "");
      return buf;
   case kRowPerf: return g_cfg.perfLog ? "On: logs/perf.log" : "Off";
   case kRowReuse: {
      static const char *kReuse[] = {"Normal", "No spatial reuse", "No reuse", "8 shadow rays"};
      return kReuse[std::clamp(g_reuseTest, 0, 3)];
   }
   case kRowDebug: {
      static const char *kViews[] = {"Off", "Shadows only", "Direct light only", "Glossy only",
                                     "Bounce light only", "Change detection"};
      return kViews[std::clamp(g_cfg.debugView, 0, 5)];
   }
   case kRowRough:
      snprintf(buf, sizeof(buf), g_cfg.minRoughness >= 0.999f ? "%.2f (matte)" : "at least %.2f",
               g_cfg.minRoughness);
      return buf;
   case kRowFixtureLight:
      snprintf(buf, sizeof(buf), "%.2fx", g_cfg.fixtureLight);
      return buf;
   case kRowGlow:
      if (g_cfg.glow <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2f", g_cfg.glow);
      return buf;
   case kRowFlameGlow:
      if (g_cfg.spriteGlow <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2f", g_cfg.spriteGlow);
      return buf;
   case kRowMaterials: return g_cfg.materials ? "Made from the textures" : "Off (flat)";
   case kRowBump:
      if (g_cfg.bump <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.2fx", g_cfg.bump);
      return buf;
   case kRowCrevice:
      if (g_cfg.crevice <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.1f", g_cfg.crevice);
      return buf;
   case kRowParallax:
      if (g_cfg.parallax <= 0.0f) return "Off";
      snprintf(buf, sizeof(buf), "%.3f", g_cfg.parallax);
      return buf;
   case kRowPolish: return g_cfg.polish ? "Mirror (sharp)" : "Off";
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

extern "C" int ObjAnimLightDescribe(int light_index, char *buf, int n);
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
   case kRowOutput: {
      static const int kHeights[] = {0, 2160, 1800, 1600, 1440, 1200, 1080, 900};
      constexpr int n = int(sizeof(kHeights) / sizeof(kHeights[0]));
      int i = 0;
      while (i < n && kHeights[i] != g_cfg.outputHeight)
         ++i;
      g_cfg.outputHeight = kHeights[((i < n ? i : 0) + dir + n) % n];
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
   case kRowReach: {
      static const int kSteps[] = {0, 20, 30, 40, 60, 90};
      int i = 0;
      while (i < 5 && kSteps[i] < g_cfg.reach)
         ++i;
      g_cfg.reach = kSteps[(i + dir + 6) % 6];
      break;
   }
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
   case kRowFixtures: g_cfg.fixturesPass = !g_cfg.fixturesPass; break;
   case kRowSteady: g_cfg.steadyFaces = !g_cfg.steadyFaces; break;
   case kRowLightFix: g_cfg.lightFix = !g_cfg.lightFix; g_lightFixes.clear(); break;
   case kRowLightFade: g_cfg.lightFade = !g_cfg.lightFade; break;
   case kRowCandidates: {
      static const int kSteps[] = {8, 12, 16, 24, 32};
      int i = 0;
      while (i < 4 && kSteps[i] < g_cfg.candidates)
         ++i;
      g_cfg.candidates = kSteps[std::clamp(i + dir, 0, 4)];
      break;
   }
   case kRowResponse: g_cfg.response = (g_cfg.response + dir + 3) % 3; break;
   case kRowFlicker: g_cfg.flicker = (g_cfg.flicker + dir + 3) % 3; break;
   case kRowVolumetric: g_cfg.volumetric = !g_cfg.volumetric; break;
   case kRowFog: g_cfg.fog = (g_cfg.fog + dir + 4) % 4; break;
   case kRowBloom: g_cfg.bloom = !g_cfg.bloom; break;
   case kRowVsync: g_cfg.vsync = !g_cfg.vsync; break;
   case kRowFps: g_cfg.fps = !g_cfg.fps; break;
   case kRowLight: g_cfg.light = step_mul(g_cfg.light, dir, 0.05f, 20.0f); break;
   case kRowDetection: g_cfg.rtDetection = !g_cfg.rtDetection; break;
   case kRowExposure: g_cfg.evMin = std::clamp(g_cfg.evMin + dir, 0, 7); break;
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
   case kRowDebug: g_cfg.debugView = (g_cfg.debugView + dir + 6) % 6; return;   // not saved
   case kRowBackFaces: g_dropBackFaces = !g_dropBackFaces; return;   // not saved
   case kRowReuse: g_reuseTest = (g_reuseTest + dir + 4) % 4; return;   // not saved
   case kRowBlink: g_blinkTest = !g_blinkTest; g_blinkId = ~0u; return;   // not saved
   case kRowPerf: g_cfg.perfLog = !g_cfg.perfLog; break;
   case kRowFrameCap: {
      static const int kCaps[] = {60, 100, 120};
      int i = 0;
      while (i < 3 && kCaps[i] != g_cfg.frameCap)
         ++i;
      g_cfg.frameCap = kCaps[((i < 3 ? i : 0) + dir + 3) % 3];
      break;
   }
   case kRowFreeze: {
      g_freezeMode = (g_freezeMode + dir + 4) % 4;
      g_freeze = g_freezeMode == 1 || g_freezeMode == 2;
      const bool lights = g_freezeMode == 1 || g_freezeMode == 3;
      if (lights && !g_freezeLights)
         g_frozenLights = g_lastLights;
      g_freezeLights = lights;
      return;   // not saved
   }
   case kRowRough:
      g_cfg.minRoughness = std::clamp(std::round(g_cfg.minRoughness * 20.0f) / 20.0f + 0.05f * dir,
                                      0.0f, 1.0f);
      break;
   case kRowGlow:
      g_cfg.glow = std::clamp(std::round(g_cfg.glow * 4.0f) / 4.0f + 0.25f * dir, 0.0f, 5.0f);
      break;
   case kRowFixtureLight:
      g_cfg.fixtureLight = std::clamp(std::round(g_cfg.fixtureLight * 4.0f) / 4.0f + 0.25f * dir, 0.25f, 6.0f);
      break;
   case kRowFlameGlow:
      g_cfg.spriteGlow = std::clamp(std::round(g_cfg.spriteGlow * 4.0f) / 4.0f + 0.25f * dir, 0.0f, 1.0f);
      break;
   case kRowMaterials:
      g_cfg.materials = !g_cfg.materials;
      // every texture named again (with or without its maps)
      for (RtTexture &t : g_textures)
         if (!t.rgba.empty())
            t.dirty = true;
      break;
   case kRowBump:
      g_cfg.bump = std::clamp(std::round(g_cfg.bump * 4.0f) / 4.0f + 0.25f * dir, 0.0f, 4.0f);
      break;
   case kRowCrevice:
      g_cfg.crevice = std::clamp(std::round(g_cfg.crevice * 10.0f) / 10.0f + 0.1f * dir, 0.0f, 1.0f);
      // every texture given again with its crevices
      for (RtTexture &t : g_textures)
         if (!t.rgba.empty())
            t.dirty = true;
      break;
   case kRowParallax:
      g_cfg.parallax = std::clamp(std::round(g_cfg.parallax * 200.0f) / 200.0f + 0.005f * dir, 0.0f, 0.1f);
      break;
   case kRowPolish: g_cfg.polish = !g_cfg.polish; break;
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

// ---- performance log: logs/perf.log ----------------------------------------
// Every 2 seconds while on: frame times (average, median, 95th percentile,
// worst), where a frame's time goes (the engine's loop clients, the ray
// tracer's capture and preparations, submitting, waiting for the GPU), the
// scene's size and the settings that matter. The GPU's own time per pass
// goes to logs/thief.log (RTGL1_METAL_PROFILE) when the log was on at start.
std::string logs_dir();
struct Perf {
   bool wasOn = false;
   double start = 0, last = 0, windowStart = 0;
   std::vector<double> frames;   // ms
   double wait = 0, draw = 0, capture = 0, prep = 0;
   std::map<std::string, double> clients;   // seconds in the window
   std::unordered_map<const char *, std::string> names;
   uint64_t models = 0, prims = 0, tris = 0, kept = 0, lights = 0;
   double lastWait = 0, lastDraw = 0, lastCapture = 0, lastPrep = 0;
} g_perf;

// this frame's engine loop clients (for a slow frame's line in perf.log)
std::vector<std::pair<const char *, double>> g_frameClients;

void perf_client_time(const char *client, unsigned, double seconds)
{
   auto it = g_perf.names.find(client);
   if (it == g_perf.names.end())
      it = g_perf.names.emplace(client, client ? client : "?").first;
   g_perf.clients[it->second] += seconds;
   bool found = false;
   for (auto &c : g_frameClients)
      if (c.first == it->second.c_str()) {
         c.second += seconds;
         found = true;
      }
   if (!found)
      g_frameClients.emplace_back(it->second.c_str(), seconds);
}

} // namespace
extern "C" void (*g_LoopClientTimeHook)(const char *client, unsigned message, double seconds);
extern "C" double g_ResImageReadSeconds;   // (namedres: reading PNG and DDS images)
namespace {

void perf_frame(double t)
{
   if (!g_cfg.perfLog) {
      if (g_perf.wasOn) {
         g_LoopClientTimeHook = nullptr;
         g_perf.wasOn = false;
      }
      return;
   }
   if (!g_perf.wasOn) {
      g_perf = Perf{};
      g_perf.wasOn = true;
      g_perf.start = g_perf.windowStart = g_perf.last = t;
      g_perf.lastWait = g_ui.waitSum; g_perf.lastDraw = g_ui.drawSum;
      g_perf.lastCapture = g_ui.captureSum; g_perf.lastPrep = g_ui.prepSum;
      g_LoopClientTimeHook = perf_client_time;
      if (FILE *f = fopen((logs_dir() + "/perf.log").c_str(), "a")) {
         time_t now = time(nullptr);
         char when[64];
         strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
         fprintf(f, "\n==== performance log started %s ====\n", when);
         fclose(f);
      }
      return;
   }
   // this frame: its time and the ray tracer's sums since the last frame
   // (count_frame's window resets them, so differences are taken carefully)
   auto delta = [](double now, double &last) {
      const double d = now >= last ? now - last : now;
      last = now;
      return d;
   };
   const double frameMs = (t - g_perf.last) * 1000.0;
   g_perf.frames.push_back(frameMs);
   g_perf.last = t;
   // a hitch: where its time went (in play, not loading: under 2 s)
   if (frameMs > 35.0 && frameMs < 2000.0)
      if (FILE *f = fopen((logs_dir() + "/perf.log").c_str(), "a")) {
         fprintf(f, "  slow frame at %.1f s: %.0f ms; images read %.1f ms; ray tracer's textures: %d given, %d mapped at once, %.1f ms; clients:",
                 t - g_perf.start, frameMs, g_ResImageReadSeconds * 1000.0, g_frameTexGiven, g_frameMapsInline,
                 g_frameTexSeconds * 1000.0);
         std::sort(g_frameClients.begin(), g_frameClients.end(),
                   [](const auto &a, const auto &b) { return a.second > b.second; });
         for (size_t i = 0; i < g_frameClients.size() && i < 4; ++i)
            fprintf(f, " %s %.1f", g_frameClients[i].first, g_frameClients[i].second * 1000.0);
         fprintf(f, "\n");
         fclose(f);
      }
   g_frameClients.clear();
   g_ResImageReadSeconds = 0;
   g_frameTexSeconds = 0;
   g_frameTexGiven = g_frameMapsInline = 0;
   g_perf.wait += delta(g_ui.waitSum, g_perf.lastWait);
   g_perf.draw += delta(g_ui.drawSum, g_perf.lastDraw);
   g_perf.capture += delta(g_ui.captureSum, g_perf.lastCapture);
   g_perf.prep += delta(g_ui.prepSum, g_perf.lastPrep);
   g_perf.models += g_perfModels;
   g_perf.prims += g_perfPrims;
   g_perf.tris += g_perfTris;
   g_perf.kept += g_cachedDrawn;
   g_perf.lights += g_frameLightsShown;
   if (t - g_perf.windowStart < 2.0 || g_perf.frames.empty())
      return;

   FILE *f = fopen((logs_dir() + "/perf.log").c_str(), "a");
   if (f) {
      const double n = double(g_perf.frames.size());
      std::vector<double> sorted = g_perf.frames;
      std::sort(sorted.begin(), sorted.end());
      double sum = 0;
      for (double ms : sorted)
         sum += ms;
      const double avg = sum / n;
      auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(p * (n - 1) + 0.5))]; };
      const double ms = 1000.0 / n;   // seconds summed over the window -> ms per frame
      const double wait = g_perf.wait * ms, draw = g_perf.draw * ms, capture = g_perf.capture * ms,
                   prep = g_perf.prep * ms;
      const double game = std::max(0.0, avg - wait - draw);
      const RgFloat3D cam = to_rt(g_camera.pos);
      int rw, rh;
      render_size(rw, rh);
      static const char *kGi[] = {"2 half rate", "1", "2", "1 half rate"};
      fprintf(f, "[%7.1f s] camera (%.1f %.1f %.1f) m  traced %dx%d -> output %dx%d  %s%s  bounces %s  reach %d m  lights tried %d  bloom %d  vsync %d  limit %d fps\n",
              t - g_perf.start, cam.data[0], cam.data[1], cam.data[2], rw, rh, g_drawableW, g_drawableH,
              kUpscaleNames[std::clamp(g_cfg.upscale, 0, 5)], g_cfg.dynres > 0 ? " (dynamic)" : "",
              kGi[std::clamp(g_cfg.gi, 0, 3)], g_cfg.reach, g_cfg.candidates, int(g_cfg.bloom), int(g_vsync), g_cfg.frameCap);
      fprintf(f, "  %.1f fps  frame avg %.1f ms  median %.1f  95%% %.1f  worst %.1f\n", n / (t - g_perf.windowStart),
              avg, pct(0.5), pct(0.95), sorted.back());
      fprintf(f, "  per frame: game %.1f ms (of it: rt capture+upload %.1f, rt prep %.1f)  rt submit %.1f  gpu wait %.1f\n",
              game, capture, prep, draw, wait);
      // the engine's loop clients, busiest first
      std::vector<std::pair<double, std::string>> clients;
      double clientsSum = 0;
      for (const auto &c : g_perf.clients) {
         clients.push_back({c.second * ms, c.first});
         clientsSum += c.second * ms;
      }
      std::sort(clients.rbegin(), clients.rend());
      fprintf(f, "  engine loop clients (ms per frame, %.1f in all):", clientsSum);
      int shown = 0;
      for (const auto &c : clients) {
         if (shown >= 14 || c.first < 0.05)
            break;
         fprintf(f, "%s %s %.2f", shown ? "," : "", c.second.c_str(), c.first);
         ++shown;
      }
      fprintf(f, "\n  scene per frame: %.0f models (%.0f kept out of view), %.0f primitives, %.0fk model triangles; world %u of %zu pieces traced; %.0f lights traced of %u\n",
              g_perf.models / n, g_perf.kept / n, g_perf.prims / n, g_perf.tris / n / 1000.0,
              g_worldShownGroups, g_world.size(), g_perf.lights / n, g_lightsTotal);
      fclose(f);
   }
   g_perf.frames.clear();
   g_perf.clients.clear();
   g_perf.wait = g_perf.draw = g_perf.capture = g_perf.prep = 0;
   g_perf.models = g_perf.prims = g_perf.tris = g_perf.kept = g_perf.lights = 0;
   g_perf.windowStart = t;
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
      g_ui.captureMs = g_ui.captureSum * 1000.0 / g_ui.windowFrames;
      g_ui.prepMs = g_ui.prepSum * 1000.0 / g_ui.windowFrames;
      g_ui.waitSum = g_ui.drawSum = g_ui.captureSum = g_ui.prepSum = 0;
      g_ui.windowStart = t;
      g_ui.windowFrames = 0;
      g_ui.windowWorst = 0;
   }
   if (scene && g_ui.hintUntil < 0)
      g_ui.hintUntil = t + 8.0;
   perf_frame(t);
}

void draw_ui(bool scene)
{
   const bool hint = scene && g_ui.hintUntil > now_seconds();
   const bool note = !g_ui.status.empty() && now_seconds() < g_ui.statusUntil;
   if (!g_cfg.fps && !g_ui.open && !hint && !note)
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
      char line[128], line2[192];
      if (g_cfg.dynres > 0)
         snprintf(line, sizeof(line), "%3.0f fps  %5.1f ms (worst %5.1f)  dynamic \x7f%dx%d",
                  g_ui.fps, g_ui.frameMs, g_ui.worstMs, g_drawableW, g_drawableH);
      else
         snprintf(line, sizeof(line), "%3.0f fps  %5.1f ms (worst %5.1f)  %dx%d\x7f%dx%d",
                  g_ui.fps, g_ui.frameMs, g_ui.worstMs, w, h, g_drawableW, g_drawableH);
      // waiting in rgStartFrame is the GPU still busy with earlier frames
      const double game = std::max(0.0, g_ui.frameMs - g_ui.waitMs - g_ui.drawMs);
      char seen[48] = "";
      if (g_noteTraced >= 0)
         snprintf(seen, sizeof(seen), "  lit %d (game %d)", int(std::lround(g_noteTraced * 100)),
                  int(std::lround(g_noteOriginal * 100)));
      snprintf(line2, sizeof(line2), "game %4.1f ms (capture %4.1f, rt prep %4.1f)  rt submit %4.1f ms  gpu wait %4.1f ms  lights %u/%u/%u%s",
               game, g_ui.captureMs, g_ui.prepMs, g_ui.drawMs, g_ui.waitMs, g_frameLightsShown, g_lightsReaching, g_lightsTotal, seen);
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

   // The inspector's note (what it looked at) shows even with the panel shut.
   const bool statusShown = !g_ui.status.empty() && now_seconds() < g_ui.statusUntil;
   if (statusShown && !g_ui.open) {
      const float s = std::round(px * 0.8f);
      const float tw = text_width(s, g_ui.status.c_str());
      const float x = std::round((g_drawableW - tw) * 0.5f), y = std::round(g_drawableH * 0.62f);
      panel(x - pad, y - pad * 0.5f, x + tw + pad, y + s + pad * 0.5f, 0.7f);
      text(fg, x, y, s, g_ui.status.c_str(), gold);
   }

   if (g_ui.open) {
      const float rowH = px * 1.5f;
      const float labelW = text_width(px, "Light size (soft shadows)") + px * 2.0f;
      const float valueW = text_width(px, "< MetalFX (not on this Mac) >") + px;
      const float w = labelW + valueW + 4 * pad + px * 2.0f;
      // As many rows as fit the window; the list scrolls with the selection.
      const float chrome = rowH * 6.5f + 2 * pad;   // title, more-lines, help, note
      const int fit = std::max(4, int((g_drawableH * 0.94f - chrome) / rowH));
      const int shown = std::min<int>(kRowCount, fit);
      if (g_ui.row < g_ui.scroll)
         g_ui.scroll = g_ui.row;
      if (g_ui.row >= g_ui.scroll + shown)
         g_ui.scroll = g_ui.row - shown + 1;
      g_ui.scroll = std::clamp(g_ui.scroll, 0, kRowCount - shown);
      const float h = rowH * (shown + 6.5f) + 2 * pad;
      const float x0 = std::round((g_drawableW - w) * 0.5f);
      const float y0 = std::round((g_drawableH - h) * 0.5f);
      panel(x0, y0, x0 + w, y0 + h, 0.82f);
      float y = y0 + pad + rowH * 0.25f;
      text(fg, x0 + 2 * pad, y, px, "Ray tracing settings", gold);
      if (shown < kRowCount) {
         char where[32];
         snprintf(where, sizeof(where), "%d / %d", g_ui.row + 1, int(kRowCount));
         const float ss = std::round(px * 0.75f);
         text(fg, x0 + w - 2 * pad - text_width(ss, where), y + (px - ss), ss, where, dim);
      }
      y += rowH * 1.25f;
      const float ms = std::round(px * 0.7f);
      if (g_ui.scroll > 0)
         text(fg, x0 + 2 * pad, y, ms, "... more above (Up / Page Up)", dim);
      y += rowH;
      for (int r = g_ui.scroll; r < g_ui.scroll + shown; ++r) {
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
      if (g_ui.scroll + shown < kRowCount)
         text(fg, x0 + 2 * pad, y, ms, "... more below (Down / Page Down)", dim);
      y += rowH * 1.25f;
      const float s = std::round(px * 0.75f);
      text(fg, x0 + 2 * pad, y, s, "Up/Down: choose  Left/Right: change  Esc: close  Cmd+I: inspect", dim);
      if (statusShown)
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
   // (time since start: RTGL1 keeps it as a 32-bit float)
   static const double s_start = now_seconds();
   double current = now_seconds() - s_start;
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
   // The mission's fog, in the volume Raze RT's haze uses: one medium
   // between the camera and everything, lit by the lamps it hangs around,
   // its own colour the glow it has unlit. The game's fog is linear, clear
   // at the eye and opaque at its distance; the volume's is exponential, so
   // its density rises along the view (a fifth near, the rest far), which
   // keeps near things clear the way the game's fog does. Its reach is the
   // fog's distance: what lies beyond is fog.
   const bool fogOn = g_cfg.fog > 0 && g_fogDistFt > 0;
   float volFar = 40.0f, volNear = 0.3f, volFarDens = 0.3f;
   RgFloat3D volAmbient = {{0.004f, 0.005f, 0.008f}};
   if (fogOn) {
      const float D = g_fogDistFt * 0.3048f;
      const float thick = g_cfg.fog == 1 ? 0.5f : g_cfg.fog == 3 ? 1.8f : 1.0f;
      volFar = std::clamp(D, 12.0f, 300.0f);
      // optical depth over the reach (~3 at the fog's distance: 95% fog);
      // the volume puts 0.001 x density in each of its 64 slices
      const float tau = 3.0f * thick * volFar / D;
      volNear = 0.4f * tau / 0.064f;
      volFarDens = 1.6f * tau / 0.064f;
      auto lin = [](float c) {
         c = std::clamp(c / 255.0f, 0.0f, 1.0f);
         return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
      };
      // as bright as the sky it is usually matched to
      for (int i = 0; i < 3; ++i)
         volAmbient.data[i] = lin(g_fogRgb[i]) * g_cfg.sky;
   }
   auto volumetric = RgDrawFrameVolumetricParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_PARAMS,
      .pNext = &refl,
      // RTGL1's default is on, with a bright ambient haze: costly, and it
      // greyed Thief's darkness. Optional, lit by the lamps when on.
      .enable = g_cfg.volumetric || fogOn,
      .maxHistoryLength = 8.0f,
      .useSimpleDepthBased = false,
      .volumetricFar = volFar,
      .ambientColor = volAmbient,
      .scaterring = volNear,
      .assymetry = 0.6f,
      .useIlluminationVolume = false,
      .fallbackSourceColor = {{0, 0, 0}},
      .fallbackSourceDirection = {{0, 0, -1}},
      .lightMultiplier = 1.0f,
      .illuminateFromAllLights = true,
      .mediaColor = {{1.0f, 1.0f, 1.0f}},
      .mediaColorFar = {{1.0f, 1.0f, 1.0f}},
      .farScattering = volFarDens,
      .densityCurve = 1.0f,
      .occludeEmission = false,
      .ditherRadius = 5.0f,   // (Raze RT's: with the depth gate)
      .ditherRadiusZ = 1.0f,
      .spatialBlur = 1.0f,
      .lightNearFade = 0.3f,
      // Raze RT's: each light's haze skips the cells behind what is seen
      // (shader 0015 runs only with the gate on), the saving that made
      // volumetric light affordable there
      .depthGate = 1.0f,
      .depthGateBias = 0.0f,
      .depthGateFeather = 1.0f,
      .depthGateTaps = 5,
   };
   const bool lightEvent = g_lightEventFrames > 0;
   if (g_lightEventFrames > 0)
      --g_lightEventFrames;
   int renderW = 0, renderH = 0;
   render_size(renderW, renderH);
   auto illum = RgDrawFrameIlluminationParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS,
      .pNext = &volumetric,
      .maxBounceShadows = 2,
      .indirectBounces = (g_cfg.gi == 1 || g_cfg.gi == 3) ? 1u : 2u,
      .indirectLegacyBounceWeight = true,
      .cellWorldSize = 1.0f,
      // How strongly the denoiser throws away what it has gathered where the
      // lighting changed. It notices a change for one frame only (after that
      // the place is dark and there is nothing to compare); at Blood RT's
      // values that frame removed only part of the old light, and the rest
      // faded at 1% a frame -- a doused torch took seconds to go dark.
      // (Blood RT's values; for the few frames after a light is put out or
      // lit in the game, everything at once, so the torch goes dark at once)
      .directDiffuseSensitivityToChange = (g_cfg.response == 0 && !lightEvent) ? 0.5f : 1.0f,
      .indirectDiffuseSensitivityToChange = lightEvent || g_cfg.response == 2 ? 1.0f : g_cfg.response == 1 ? 0.6f : 0.2f,
      .specularSensitivityToChange = lightEvent || g_cfg.response == 2 ? 1.0f : g_cfg.response == 1 ? 0.8f : 0.3f,
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
                        : g_cfg.debugView == 4 ? 128u : g_cfg.debugView == 5 ? 2u : 0u,
      .restirTemporalJitter = g_reuseTest == 2 ? 0.0f : 2.0f,
      .rrSpecularHitDistance = true,
      .directSamples = g_reuseTest == 3 ? 4u : 1u,
      // 0: bounce light traced for half of the pixels each frame
      .indirectSamples = (g_cfg.gi == 0 || g_cfg.gi == 3) ? 0u : 1u,
      // more candidate lights per pixel (no extra rays): Thief has many small lights
      .restirInitialSamples = uint32_t(g_cfg.candidates),
      .restirSpatialSamples = (g_reuseTest == 1 || g_reuseTest == 2) ? 0u : 8u,
      // in pixels: 30 at ~1150 wide (Blood RT's), wider at higher resolutions so
      // neighbours are taken from as far across the surface as there
      .restirSpatialRadius = 30.0f,   // (as Blood RT)
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
      // RTGL1's own (as Blood RT) but for the frames after a light event
      .svgfFpGrad = lightEvent,
      .svgfIndirMaxHist = lightEvent ? 16.0f : 0.0f,
      .svgfIndirAntilag = lightEvent,
   };
   // No surface glossier than 0.7 (Blood RT's rt_min_roughness): a glossy
   // surface's reflection is one noisy sample per pixel, the crawling.
   auto textures = RgDrawFrameTexturesParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_TEXTURES_PARAMS,
      .pNext = &illum,
      .dynamicSamplerFilter = RG_SAMPLER_FILTER_LINEAR,
      .mipLodBiasOffset = 0.0f,
      .normalMapStrength = g_cfg.materials ? g_cfg.bump : 0.0f,
      .emissionMapBoost = 50.0f,   // (as Blood RT)
      .emissionMaxScreenColor = 1.5f,
      .minRoughness = g_cfg.minRoughness,
      .heightMapDepth = g_cfg.materials ? g_cfg.parallax : 0.0f,
      // the material maps' roughness and metal (rt_materials.h): RTGL1's
      // defaults for these leave every surface a plain matte dielectric
      .forceNonMetallic = false,
      .metallicMax = 0.75f,
      .metallicRoughCut = 0.7f,
      .metallicRoughBand = 0.3f,
      .spritePbr = 0.35f,
      .spriteMetallicMax = 0.9f,
      .spriteRoughMin = 0.0f,
      .spriteNormalStrength = 1.0f,
      .worldPbr = g_cfg.materials ? 1.0f : 0.0f,
      .albedoFilterMode = 0,
   };
   // Blood RT's eye adaptation: a dark place is not brightened up to 32 times
   // (RTGL1's own range starts at EV 0), its noise with it
   auto bloom = RgDrawFrameBloomParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_BLOOM_PARAMS,
      .pNext = &textures,
      .inputEV = 6.0f,
      .inputThreshold = 16.0f,
      .bloomIntensity = g_cfg.bloom ? 1.0f : -1.0f,   // (negative: the pass is skipped)
      .lensDirtIntensity = 1.0f,
   };
   auto tonemap = RgDrawFrameTonemappingParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_TONEMAPPING_PARAMS,
      .pNext = &bloom,
      .disableEyeAdaptation = false,
      .ev100Min = float(g_cfg.evMin),
      .ev100Max = std::max(float(g_cfg.evMin) + 0.1f, 7.7f),
      .luminanceWhitePoint = 3.0f,
      .saturation = {{0, 0, 0}},
      .crosstalk = {{1.0f, 1.0f, 1.0f / 16.0f}},
      .contrast = 0.1f,
      .hdrBrightness = 1.0f,
      .hdrContrast = 0.1f,
      .hdrSaturation = {{0.25f, 0.25f, 0.25f}},
   };
   auto sky = RgDrawFrameSkyParams{
      .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS,
      .pNext = &tonemap,
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
   {
      // every triangle of the models within 10 m of the camera, as given to
      // the path tracer: two of these from different places show whether the
      // shadow-casting geometry changes with where you stand
      char gpath[64];
      snprintf(gpath, sizeof(gpath), "/inspect-%d-geometry.txt", n);
      if (FILE *g = fopen((dir + gpath).c_str(), "w")) {
         fprintf(g, "camera %.3f %.3f %.3f frame %u\n", o[0], o[1], o[2], g_frameNumber);
         for (const auto &e : g_objects) {
            const CachedObject &c = e.second;
            const float dx = c.center[0] - o[0], dy = c.center[1] - o[1], dz = c.center[2] - o[2];
            if (dx * dx + dy * dy + dz * dz > 100.0f)
               continue;
            char name[128];
            RenderRTGameObjectName(e.first, name, sizeof(name));
            fprintf(g, "object %u \"%s\" drawn %u whole %d pass %d\n", e.first, name, c.frame,
                    int(c.whole), int(c.passLight));
            for (const CapturedPrim &p : c.prims) {
               if (p.sprite)
                  continue;
               for (size_t t = 0; t + 2 < p.verts.size(); t += 3) {
                  fprintf(g, "%c", (p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY) ? 'S'
                                   : (p.flags & RG_MESH_PRIMITIVE_NO_SHADOW) ? 'N' : 'V');
                  for (int k = 0; k < 3; ++k)
                     fprintf(g, " %.3f %.3f %.3f", p.verts[t + k].position[0],
                             p.verts[t + k].position[1], p.verts[t + k].position[2]);
                  fprintf(g, " %s\n", p.texture.c_str());
               }
            }
         }
         fclose(g);
      }
   }
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
             "drawn frame %u (now %u), posed %d, flags 0x%x%s\n",
             e.first, name, tris, shadowTris, c.prims.size(), int(c.whole), c.frame, g_frameNumber,
             int(c.posed), allFlags, c.passLight ? ", lets its light out" : "");
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
         char fixed[96] = "";
         for (const auto &f : g_lightFixes)
            if ((f.first >> 40) == L.id && f.second.done) {
               const float m = std::sqrt(f.second.offset[0] * f.second.offset[0] +
                                         f.second.offset[1] * f.second.offset[1] +
                                         f.second.offset[2] * f.second.offset[2]);
               snprintf(fixed, sizeof(fixed), "; %.0f cm from the nearest wall%s", f.second.before * 100,
                        m > 0.001f ? "" : " (left there)");
               if (m > 0.001f)
                  snprintf(fixed + strlen(fixed), sizeof(fixed) - strlen(fixed), ", moved out %.0f cm",
                           m * 100);
            }
         out("  light %u at (%.2f %.2f %.2f), %.2f m away: rgb %.0f %.0f %.0f, radius %.1f ft, "
             "%s inner %.3f outer %.3f dir (%.2f %.2f %.2f)%s\n",
             L.id, p.data[0], p.data[1], p.data[2], dist, L.rgb[0], L.rgb[1], L.rgb[2], L.radius,
             L.inner >= 0 ? "spot" : "point", L.inner, L.outer, L.dir[0], L.dir[1], L.dir[2], fixed);
         char anim[256];
         if (L.id < 0x10000u && ObjAnimLightDescribe(int(L.id), anim, sizeof(anim)))
            out("    %s\n", anim);
         else if (L.id < 0x10000u)
            out("    (no anim light drives it: a static light)\n");
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
   {
      // the game's own view of what is near: objects the ray tracer may not
      // have (a creature never drawn, an object not loaded)
      out("the game's objects within 8 m of the camera:\n");
      struct Sink { FILE *log; };
      Sink sink{log};
      RenderRTGameDescribeNearby(g_camera.pos, 8.0f / 0.3048f, [](const char *text, void *ctx) {
         fputs(text, stderr);
         if (FILE *f = static_cast<Sink *>(ctx)->log)
            fputs(text, f);
      }, &sink);
   }
   out("model draws so far that captured nothing: %u; drawn only in pieces (the whole model kept): %u\n",
       g_emptyCaptures, g_pieceCaptures);
   // the models hit by earlier inspections: where they are now, if anywhere
   static std::vector<uint32_t> s_inspected;
   if (bestObj && std::find(s_inspected.begin(), s_inspected.end(), bestObj) == s_inspected.end())
      s_inspected.push_back(bestObj);
   if (s_inspected.size() > 8)
      s_inspected.erase(s_inspected.begin());
   for (uint32_t id : s_inspected) {
      char name[128];
      RenderRTGameObjectName(id, name, sizeof(name));
      auto it = g_objects.find(id);
      if (it == g_objects.end()) {
         out("earlier inspected object %u \"%s\": not known to the ray tracer now\n", id, name);
         continue;
      }
      const CachedObject &c = it->second;
      size_t tris = 0, shadow = 0;
      for (const CapturedPrim &p : c.prims) {
         tris += p.verts.size() / 3;
         if (p.flags & RG_MESH_PRIMITIVE_SHADOW_ONLY)
            shadow += p.verts.size() / 3;
      }
      out("earlier inspected object %u \"%s\": at (%.2f %.2f %.2f), %zu triangles (%zu shadow-only) in %zu "
          "primitives, whole %d, drawn frame %u, given frame %u (now %u)\n",
          id, name, c.center[0], c.center[1], c.center[2], tris, shadow, c.prims.size(), int(c.whole),
          c.frame, c.givenFrame, g_frameNumber);
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
   g_ui.statusUntil = now_seconds() + 8.0;
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
   // Cmd+I: inspect what is at the centre of the view without the panel
   // (which covers the centre) being open.
   if (scancode == SDL_SCANCODE_I && (mods & KMOD_GUI)) {
      if (down && !repeat)
         inspect_view();
      return true;
   }
   if (!g_ui.open || !down)
      return false;   // key releases always reach the game: no stuck keys
   switch (scancode) {
   case SDL_SCANCODE_ESCAPE: if (!repeat) g_ui.open = false; break;
   case SDL_SCANCODE_UP: g_ui.row = (g_ui.row + kRowCount - 1) % kRowCount; break;
   case SDL_SCANCODE_DOWN: g_ui.row = (g_ui.row + 1) % kRowCount; break;
   case SDL_SCANCODE_PAGEUP: g_ui.row = std::max(g_ui.row - 10, 0); break;
   case SDL_SCANCODE_PAGEDOWN: g_ui.row = std::min(g_ui.row + 10, int(kRowCount) - 1); break;
   case SDL_SCANCODE_HOME: g_ui.row = 0; break;
   case SDL_SCANCODE_END: g_ui.row = int(kRowCount) - 1; break;
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
// How lit the player is, by the path tracer's lights: for the light gem and
// the guards' eyes (the game's own estimate is its baked lighting, which the
// ray-traced picture does not match). The lights as given to the path tracer
// this frame, their brightness falling off as RTGL1's do, each one shadowed
// or not by rays from the player's body to it against the level's walls (a
// tree of boxes over their triangles) and the models given this frame. No
// bounce light. Scaled so a light 3 m away counts as in the game.

namespace {

struct Bvh {
   struct Node {
      float mn[3], mx[3];
      int left = -1, right = -1, start = 0, count = 0;
   };
   std::vector<Node> nodes;
   std::vector<float> tris;   // 9 floats each
   std::vector<int> order;
   uint32_t serial = ~0u;

   void build(std::vector<float> &&t)
   {
      tris = std::move(t);
      const int n = int(tris.size() / 9);
      order.resize(size_t(n));
      for (int i = 0; i < n; ++i)
         order[size_t(i)] = i;
      nodes.clear();
      nodes.reserve(size_t(n) * 2 / 4 + 8);
      if (n)
         split(0, n);
   }
   int split(int start, int count)
   {
      const int id = int(nodes.size());
      nodes.emplace_back();
      Node nd;
      for (int k = 0; k < 3; ++k) {
         nd.mn[k] = 1e30f;
         nd.mx[k] = -1e30f;
      }
      float cmn[3] = {1e30f, 1e30f, 1e30f}, cmx[3] = {-1e30f, -1e30f, -1e30f};
      for (int i = start; i < start + count; ++i) {
         const float *t = &tris[size_t(order[size_t(i)]) * 9];
         for (int k = 0; k < 3; ++k) {
            const float c = (t[k] + t[3 + k] + t[6 + k]) / 3.0f;
            cmn[k] = std::min(cmn[k], c);
            cmx[k] = std::max(cmx[k], c);
            for (int v = 0; v < 3; ++v) {
               nd.mn[k] = std::min(nd.mn[k], t[v * 3 + k]);
               nd.mx[k] = std::max(nd.mx[k], t[v * 3 + k]);
            }
         }
      }
      if (count <= 6) {
         nd.start = start;
         nd.count = count;
         nodes[size_t(id)] = nd;
         return id;
      }
      int axis = 0;
      for (int k = 1; k < 3; ++k)
         if (cmx[k] - cmn[k] > cmx[axis] - cmn[axis])
            axis = k;
      const int mid = start + count / 2;
      std::nth_element(order.begin() + start, order.begin() + mid, order.begin() + start + count,
                       [this, axis](int a, int b) {
                          const float *x = &tris[size_t(a) * 9], *y = &tris[size_t(b) * 9];
                          return x[axis] + x[3 + axis] + x[6 + axis] < y[axis] + y[3 + axis] + y[6 + axis];
                       });
      nodes[size_t(id)] = nd;
      const int l = split(start, mid - start);
      const int r = split(mid, start + count - mid);
      nodes[size_t(id)].left = l;
      nodes[size_t(id)].right = r;
      return id;
   }
   // anything between o and o + d * len?
   bool blocked(const float o[3], const float d[3], float len) const
   {
      if (nodes.empty())
         return false;
      float inv[3];
      for (int k = 0; k < 3; ++k)
         inv[k] = std::fabs(d[k]) > 1e-9f ? 1.0f / d[k] : 1e30f;
      int stack[96], sp = 0;
      stack[sp++] = 0;
      while (sp) {
         const Node &nd = nodes[size_t(stack[--sp])];
         float t0 = 0.0f, t1 = len;
         bool miss = false;
         for (int k = 0; k < 3 && !miss; ++k) {
            float a = (nd.mn[k] - o[k]) * inv[k], b = (nd.mx[k] - o[k]) * inv[k];
            if (a > b)
               std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            miss = t0 > t1;
         }
         if (miss)
            continue;
         if (nd.left < 0) {
            for (int i = nd.start; i < nd.start + nd.count; ++i) {
               const float *t = &tris[size_t(order[size_t(i)]) * 9];
               float hit;
               if (ray_triangle(o, d, t, t + 3, t + 6, hit) && hit < len)
                  return true;
            }
         } else if (sp + 2 <= 96) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.right;
         }
      }
      return false;
   }
};

Bvh g_worldBvh;

void ensure_world_bvh()
{
   if (g_worldBvh.serial == g_worldSerial && g_worldValid)
      return;
   std::vector<float> t;
   for (const WorldGroup &g : g_world) {
      // (see-through or not there for light: sky, water, grates)
      if (g.flags & (kRenderRTWorldSky | kRenderRTWorldWater | kRenderRTWorldTransparent))
         continue;
      for (size_t k = 0; k + 2 < g.indices.size(); k += 3)
         for (int v = 0; v < 3; ++v)
            for (int j = 0; j < 3; ++j)
               t.push_back(g.verts[g.indices[k + v]].position[j]);
   }
   g_worldBvh.build(std::move(t));
   g_worldBvh.serial = g_worldSerial;
}

// anything given this frame between a and b (metres)?
bool segment_blocked(const float a[3], const float b[3])
{
   float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
   const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
   if (len < 1e-4f)
      return false;
   for (float &x : d)
      x /= len;
   // stop short of the light: its own lamp's glass and fittings are round it
   const float reach = std::max(0.0f, len - 0.12f);
   if (g_worldBvh.blocked(a, d, reach))
      return true;
   float smn[3], smx[3];
   for (int k = 0; k < 3; ++k) {
      smn[k] = std::min(a[k], b[k]);
      smx[k] = std::max(a[k], b[k]);
   }
   for (const auto &e : g_objects) {
      const CachedObject &c = e.second;
      if (c.givenFrame + 1 < g_frameNumber || c.passLight)
         continue;
      bool overlap = true;
      for (int k = 0; k < 3 && overlap; ++k)
         overlap = c.bmin[k] <= smx[k] && c.bmax[k] >= smn[k];
      if (!overlap)
         continue;
      for (const CapturedPrim &p : c.prims) {
         if (p.sprite || (p.flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_NO_SHADOW)))
            continue;
         for (size_t t = 0; t + 2 < p.verts.size(); t += 3) {
            float hit;
            if (ray_triangle(a, d, p.verts[t].position, p.verts[t + 1].position, p.verts[t + 2].position,
                             hit) &&
                hit < reach)
               return true;
         }
      }
   }
   return false;
}

} // namespace

// ============================================================================
// render_rt.h: called by the engine

extern "C" {

int RenderRTActive(void) { return g_active ? 1 : 0; }

int RenderRTFrameCap(void) { return g_active ? g_cfg.frameCap : 0; }

int RenderRTDetectionWanted(void) { return g_active && g_cfg.rtDetection && g_frameNumber > 2 ? 1 : 0; }

static std::chrono::steady_clock::time_point s_prepStart;
static int s_prepDepth = 0;
void RenderRTPrepBegin(void)
{
   if (s_prepDepth++ == 0)
      s_prepStart = std::chrono::steady_clock::now();
}
void RenderRTPrepEnd(void)
{
   if (s_prepDepth > 0 && --s_prepDepth == 0)
      g_ui.prepSum += std::chrono::duration<double>(std::chrono::steady_clock::now() - s_prepStart).count();
}

int RenderRTPointLighting(const float pos[3], float *value)
{
   if (!g_active || !value)
      return 0;
   struct PrepScope {
      PrepScope() { RenderRTPrepBegin(); }
      ~PrepScope() { RenderRTPrepEnd(); }
   } prepScope;
   // asked many times a frame (each guard): once
   static uint32_t s_frame = 0;
   static float s_pos[3] = {1e30f, 0, 0}, s_value = 0;
   if (s_frame == g_frameNumber && pos[0] == s_pos[0] && pos[1] == s_pos[1] && pos[2] == s_pos[2]) {
      *value = s_value;
      return 1;
   }
   ensure_world_bvh();
   // the body: legs, middle, head (Dark units are feet)
   const float offs[3] = {-1.6f, 0.0f, 1.3f};
   float body[3][3];
   for (int i = 0; i < 3; ++i) {
      const float q[3] = {pos[0], pos[1], pos[2] + offs[i]};
      const RgFloat3D r = to_rt(q);
      memcpy(body[i], r.data, sizeof(body[i]));
   }
   // the game's own scale: a light of brightness B 3 m (9.84 ft) away gives
   // sqrt(3)*B/32/9.84 there; RTGL1's falls off with the square of distance
   const float kScale = 1.7320508f * 9.0f / (32.0f * 9.8425f * 4.0f);
   float total = 0.0f;
   for (const PendingLight &L : g_lastLights) {
      if (L.peak <= 0.5f)
         continue;
      const RgFloat3D lp = to_rt(L.pos);
      const float lumens = L.peak * 4.0f * g_cfg.light;
      const float r = std::max(g_cfg.lightSize, 0.02f);
      float sum = 0.0f;
      for (int i = 0; i < 3; ++i) {
         const float dx = lp.data[0] - body[i][0], dy = lp.data[1] - body[i][1], dz = lp.data[2] - body[i][2];
         const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
         // as RTGL1's sphere: the solid angle it covers, over that of its disc far away
         const float sinT = r / std::max(r, d);
         const float omega = 2.0f * 3.14159265f * (1.0f - std::sqrt(1.0f - sinT * sinT));
         float e = lumens * omega / (3.14159265f * r * r);
         if (L.inner >= 0.0f && (L.dir[0] != 0 || L.dir[1] != 0 || L.dir[2] != 0)) {
            const float dl = std::sqrt(L.dir[0] * L.dir[0] + L.dir[1] * L.dir[1] + L.dir[2] * L.dir[2]);
            const float cosA = d > 1e-4f ? -(dx * L.dir[0] + dy * L.dir[1] + dz * L.dir[2]) / (d * dl) : 1.0f;
            const float lo = std::min(L.outer, L.inner), hi = std::max(L.outer, L.inner);
            float t = hi > lo ? std::clamp((cosA - lo) / (hi - lo), 0.0f, 1.0f) : (cosA >= hi ? 1.0f : 0.0f);
            t = t * t * (3.0f - 2.0f * t);
            e *= t * t;
         }
         if (e * kScale < 0.002f)
            continue;
         if (!segment_blocked(body[i], lp.data))
            sum += e;
      }
      total += kScale * sum / 3.0f;
   }
   s_frame = g_frameNumber;
   memcpy(s_pos, pos, sizeof(s_pos));
   s_value = total;
   *value = total;
   return 1;
}

void RenderRTNoteLighting(float original, float traced)
{
   g_noteOriginal = original;
   g_noteTraced = traced;
}

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

// the CPU time the ray tracer's own capture takes (for the frame counter)
struct CaptureTimer {
   std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
   ~CaptureTimer()
   {
      g_ui.captureSum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   }
};

void RenderRTSceneEnd(void)
{
   if (!g_active || !g_inScene)
      return;
   CaptureTimer timer;
   flush_lights();
   upload_world();
   upload_objects();
   // the material maps being made (texture_name), now and then
   if ((g_frameNumber & 63) == 0) {
      static size_t s_reported = 0;
      const size_t n = mat_pool().threads ? mat_pool().waiting() : 0;
      if (n != s_reported) {
         if (n)
            fprintf(stderr, "RT: material maps: %zu textures to go (%u threads)\n", n, mat_pool().threads);
         else
            fprintf(stderr, "RT: material maps: all made\n");
         s_reported = n;
      }
   }
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
   CaptureTimer timer;
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
      // A flame (a torch's, a candle's: warm and saturated, not grey smoke)
      // glows in its colour through an emission map of its own -- black
      // outside its soft shape, so no square. Its light is the torch's own.
      if (tex) {
         float c[3] = {0, 0, 0};
         for (int i = 0; i < count; ++i) {
            c[0] += v[i].r / count;
            c[1] += v[i].g / count;
            c[2] += v[i].b / count;
         }
         const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
         if (hi > 0.2f && c[0] >= c[1] && c[1] >= c[2] * 0.9f && (hi - lo) > 0.25f * hi)
            if (const char *flame = flame_texture(tex, c[0] / hi, c[1] / hi, c[2] / hi)) {
               prim.texture = flame;
               // its emission map is black outside the flame's shape, so it
               // glows without the sprite's square (with none, the rasterised
               // translucent pass showed no glow at all: a dim, brown flame)
               prim.emissive = 2.0f * std::clamp(g_cfg.spriteGlow, 0.0f, 1.0f);
               // the light at a flame is a torch's (or a candle's, a brazier's)
               float m[3] = {0, 0, 0};
               for (const RgPrimitiveVertex &d : prim.verts)
                  for (int k = 0; k < 3; ++k)
                     m[k] += d.position[k] / float(prim.verts.size());
               for (size_t li = 0; li < g_lightPositions.size() && li < g_pendingLights.size(); ++li) {
                  const RgFloat3D &l = g_lightPositions[li];
                  const float dx = l.data[0] - m[0], dy = l.data[1] - m[1], dz = l.data[2] - m[2];
                  if (dx * dx + dy * dy + dz * dz < 0.6f * 0.6f) {
                     g_fixtureLights.insert(g_pendingLights[li].id);
                     g_flameLights.insert(g_pendingLights[li].id);
                     break;
                  }
               }
            }
      }
   } else if (g_blend == kRenderBackendBlendAdd) {
      prim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
      prim.emissive = 1.0f;
   } else if (g_blend == kRenderBackendBlendAlpha && minAlpha < 0.99f) {
      prim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
   } else if (tex && tex->cutout && (g_alphaTest || g_blend == kRenderBackendBlendAlpha)) {
      prim.flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
   }

   // a polished object (a marble statue, a metal one): mirror reflections
   if (tex && tex->polished && g_cfg.polish && g_cfg.materials && !sprite && !g_RenderRTShadowOnly &&
       !(prim.flags & (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_ALPHA_TESTED)))
      prim.flags |= RG_MESH_PRIMITIVE_POLISHED;

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
         for (size_t li = 0; li < g_lightPositions.size(); ++li) {
            const RgFloat3D &l = g_lightPositions[li];
            const float dx = l.data[0] - c[0], dy = l.data[1] - c[1], dz = l.data[2] - c[2];
            if (dx * dx + dy * dy + dz * dz < 0.5f * 0.5f) {
               prim.emissive = std::max(prim.emissive, g_cfg.glow);
               // the glass lets its light out: a street lamp's light sits
               // inside its lantern, and the panes kept the street dark
               prim.flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
               // (g_lightPositions is in the order of g_pendingLights)
               if (li < g_pendingLights.size())
                  g_fixtureLights.insert(g_pendingLights[li].id);
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
      // named by what it is (texture, where it starts, its size), not by its
      // place in this frame's drawing order: the same thing keeps its name
      // from frame to frame, and so its motion and history
      uint64_t h = 1469598103934665603ull;
      for (unsigned char ch : p.texture)
         h = (h ^ ch) * 1099511628211ull;
      for (int k = 0; k < 3 && !p.verts.empty(); ++k)
         h = (h ^ uint64_t(int64_t(std::lround(p.verts[0].position[k] * 64.0f)))) * 1099511628211ull;
      h = (h ^ p.verts.size()) * 1099511628211ull;
      uint32_t slot = uint32_t(h ^ (h >> 32)) & 0x0fffffffu;
      while (!g_primitiveCounters.emplace(slot, 1).second)
         slot = (slot + 1) & 0x0fffffffu;
      upload_captured(0x300000000ull + uint64_t(slot), p);
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
   g_RenderRTCaptureCalls = g_RenderRTCaptureDropped = 0;
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

void RenderRTTextureSource(const void *bitmap) { darkrt::g_pendingSource = bitmap; }

void RenderRTFog(const float rgb[3], float distFeet)
{
   if ((distFeet > 0) != (g_fogDistFt > 0))
      fprintf(stderr, "RT: mission fog %s (%.0f %.0f %.0f, opaque at %.1f m)\n",
              distFeet > 0 ? "on" : "off", rgb[0], rgb[1], rgb[2], distFeet * 0.3048f);
   for (int i = 0; i < 3; ++i)
      g_fogRgb[i] = rgb[i];
   g_fogDistFt = distFeet > 0 ? distFeet : 0;
}

void RenderRTObjectEnd(void)
{
   CaptureTimer timer;
   const uint32_t obj = g_currentObject;
   g_currentObject = 0;
   if (!obj || !g_inScene)
      return;
   if (g_freeze) {
      g_currentCapture.clear();
      return;
   }
   // Nothing of the model reached the capture this time (a piece of a model
   // split across cells that its cell clipped away, a draw that bailed
   // out): that says nothing about the model, so what is known of it stays.
   // Replacing it with nothing made models vanish from the traced scene
   // while the game still drew them.
   if (g_currentCapture.empty()) {
      ++g_emptyCaptures;
      // logged once a model: first those never captured at all, then (the
      // interesting ones) models captured before that now come empty
      static std::unordered_set<uint32_t> s_emptyLogged, s_lostLogged;
      auto known = g_objects.find(obj);
      if (known != g_objects.end() && !known->second.prims.empty()) {
         static std::unordered_set<uint32_t> s_lostWithDraws;
         if (s_lostLogged.size() < 100 &&
             (s_lostLogged.insert(obj).second ||
              (g_RenderRTCaptureCalls > 0 && s_lostWithDraws.insert(obj).second)))
            log_model_change(obj, "drawn by the game with nothing captured, though captured before (%s; %d draws, %d triangles without a world position)",
                             g_currentWhole ? "whole" : "in pieces", g_RenderRTCaptureCalls, g_RenderRTCaptureDropped);
      } else if (s_emptyLogged.size() < 40 && s_emptyLogged.insert(obj).second)
         log_model_change(obj, "drawn by the game with nothing captured (%s)",
                          g_currentWhole ? "whole" : "in pieces");
      return;
   }
   CachedObject &c = g_objects[obj];
   if (c.frame != g_frameNumber && !g_currentWhole && c.whole && !c.prims.empty() && c.posed) {
      // Only a piece of a model this time (drawn split across cells, each
      // piece clipped to its cell), where the whole model is known and has
      // not moved: the whole one stays, else the rest of it -- and its
      // shadow -- went missing until the game next drew it in one piece.
      float pose[6];
      bool same = RenderRTGameObjectPose(obj, pose) != 0;
      for (int k = 0; k < 3 && same; ++k)
         same = std::fabs(pose[k] - c.pose[k]) < 0.01f;
      same = same && pose[3] == c.pose[3] && pose[4] == c.pose[4] && pose[5] == c.pose[5];
      if (same) {
         ++g_pieceCaptures;
         c.frame = g_frameNumber;
         g_currentCapture.clear();
         return;
      }
   }
   if (c.frame == g_frameNumber) {
      // drawn twice this frame (split across cells): keep both parts, unless
      // the first one already had all of it
      if (g_currentWhole && !c.whole) {
         c.prims = std::move(g_currentCapture);
         c.whole = true;
         c.hash = hash_prims_unordered(c.prims);
      } else if (!c.whole) {
         for (CapturedPrim &p : g_currentCapture)
            c.prims.push_back(std::move(p));
         c.hash = hash_prims_unordered(c.prims);
      }
   } else {
      c.whole = g_currentWhole;
      // (in any order: the game draws a model's faces in an order that
      // changes as they turn toward or away from the camera)
      const uint64_t h = hash_prims_unordered(g_currentCapture);
      // a model that has not changed since it was last made ready: as it was
      // (sorting its faces and finding its normals again each frame cost
      // milliseconds a frame in a busy street)
      // (not a model with camera-facing sprites -- a water stream, a glow:
      // the hash leaves sprites out, so it would never change, and the
      // sprite's animation froze on its first frame)
      bool sprites = false;
      for (const CapturedPrim &p : g_currentCapture)
         sprites = sprites || p.sprite;
      if (!sprites && h == c.hash && c.settled && c.whole && g_cfg.steadyFaces) {
         ++c.unchanged;
         c.frame = g_frameNumber;
         c.posed = RenderRTGameObjectPose(obj, c.pose) != 0;
         g_currentCapture.clear();
         return;
      }
      c.settled = false;
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
      float was[3] = {c.center[0], c.center[1], c.center[2]};
      for (int k = 0; k < 3; ++k)
         c.center[k] = n ? float(sum[k] / double(n)) : 0.0f;
      // a model that has not moved but whose captured shape is somewhere
      // else now (drawn with another transform): log where it went
      const float jx = c.center[0] - was[0], jy = c.center[1] - was[1], jz = c.center[2] - was[2];
      if (c.unchanged == 0 && (was[0] != 0 || was[1] != 0 || was[2] != 0) &&
          jx * jx + jy * jy + jz * jz > 1.0f) {
         const RgFloat3D cam = to_rt(g_camera.pos);
         log_model_change(obj, "captured %.1f m from where it was: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), "
                          "camera (%.1f %.1f %.1f), %zu primitives, %s",
                          std::sqrt(jx * jx + jy * jy + jz * jz), was[0], was[1], was[2], c.center[0],
                          c.center[1], c.center[2], cam.data[0], cam.data[1], cam.data[2], c.prims.size(),
                          g_currentWhole ? "whole" : "in pieces");
      }
   }
   for (int k = 0; k < 3; ++k) {
      c.bmin[k] = 1e30f;
      c.bmax[k] = -1e30f;
   }
   for (const CapturedPrim &p : c.prims) {
      if (p.sprite || (p.flags & RG_MESH_PRIMITIVE_TRANSLUCENT))
         continue;   // particles and glows: not the model's body
      for (const RgPrimitiveVertex &v : p.verts)
         for (int k = 0; k < 3; ++k) {
            c.bmin[k] = std::min(c.bmin[k], v.position[k]);
            c.bmax[k] = std::max(c.bmax[k], v.position[k]);
         }
   }
   // only for a model that holds still: sorting a moving one's faces anew
   // each frame broke the match between its frames (its motion), so the
   // denoiser smeared characters as they walked
   bool hasSprites = false;
   for (const CapturedPrim &p : c.prims)
      hasSprites = hasSprites || p.sprite;
   if (g_cfg.steadyFaces && c.whole && c.unchanged >= 1 && !hasSprites) {
      steady_faces(c);
      c.settled = true;
   }
   const uint64_t shapeHash = hash_prims(c.prims);
   // the normals are recomputed only when the shape changed
   size_t total = 0;
   for (const CapturedPrim &p : c.prims)
      total += p.verts.size();
   if (c.normals.size() == total && c.normalsHash == shapeHash) {
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
      c.normalsHash = shapeHash;
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
   g_fixtureLights.clear();   // (marked again as the lamps are drawn)
   g_flameLights.clear();
   // a new world (a mission or a saved game loaded): the models kept from the
   // last one are not this one's, though some have the same numbers and places
   g_objects.clear();
   g_worldSerial = serial;
   g_worldValid = false;
}

void RenderRTWorldCell(int cell) { g_worldCell = cell; }

void RenderRTWorldPolygon(int textureId, int flags, int count, const float *xyz, const float *uv)
{
   if (count < 3)
      return;
   // its block of the mission, by its first corner
   const RgFloat3D p0 = to_rt(xyz);
   auto block = [](float v) { return uint64_t(std::clamp(int(std::floor(v / kWorldChunk)) + 4096, 0, 8191)); };
   const uint64_t chunk = block(p0.data[0]) | (block(p0.data[1]) << 13) | (block(p0.data[2]) << 26);
   const uint64_t key = (uint64_t(uint32_t(textureId) & 0xffffu) << 48) | (uint64_t(flags & 0xff) << 40) | chunk;
   auto found = g_worldIndex.find(key);
   if (found == g_worldIndex.end()) {
      found = g_worldIndex.emplace(key, g_world.size()).first;
      g_world.push_back(WorldGroup{textureId, flags, {}, {}});
   }
   WorldGroup &g = g_world[found->second];
   if (g_worldCell >= 0 && (g.cells.empty() || g.cells.back() != g_worldCell))
      g.cells.push_back(g_worldCell);
   for (int i = 0; i < count; ++i) {
      const RgFloat3D q = to_rt(xyz + i * 3);
      for (int k = 0; k < 3; ++k) {
         g.bmin[k] = std::min(g.bmin[k], q.data[k]);
         g.bmax[k] = std::max(g.bmax[k], q.data[k]);
      }
   }
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
   g_worldFresh = true;
   size_t verts = 0;
   for (const WorldGroup &g : g_world)
      verts += g.verts.size();
   g_worldCell = -1;
   fprintf(stderr, "RT: world representation: %zu texture groups (by %.0f m blocks), %zu vertices\n",
           g_world.size(), kWorldChunk, verts);
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
                   const float dir[3], float inner, float outer, int reaches, float walk)
{
   if (!g_inScene)
      return;
   const float peak = std::max({rgb[0], rgb[1], rgb[2]});
   {
      // a timeline of brightness changes (a doused torch: at once, or a fade?)
      static std::unordered_map<uint32_t, float> s_last;
      static int s_lines = 0;
      static const auto s_start = std::chrono::steady_clock::now();
      auto it = s_last.find(id);
      if (it != s_last.end() && std::fabs(it->second - peak) >= 1.0f && s_lines < 3000) {
         ++s_lines;
         fprintf(stderr, "RT: light %u brightness %.1f -> %.1f at %.2f s\n", id, it->second, peak,
                 std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start).count());
      }
      if (it == s_last.end() || std::fabs(it->second - peak) >= 1.0f)
         s_last[id] = peak;
   }
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
   l.walk = walk;
   g_pendingLights.push_back(l);
}

} // extern "C"

namespace {

void upload_light(const PendingLight &L)
{
   const float peak = L.peak;
   // Dark's brightness is a light level (0..255 at the source, falling off
   // with distance); RTGL1's intensity is luminous flux. Calibrated by eye.
   // Dark falls off as 1/distance up to the light's radius, RTGL1 as
   // 1/distance^2; a larger source and a lower flux keep the nearby walls
   // from burning out while the far side of a room still receives light.
   // a lamp's or a torch's light: brighter than the level's fill lights
   // (Dark lit the walls around them with baked lightmaps; traced, at the
   // level's own brightness, they hardly showed against their surroundings)
   const bool fixture = g_fixtureLights.count(L.id) != 0;
   float lumens = peak * 4.0f * g_cfg.light * (fixture ? g_cfg.fixtureLight : 1.0f);
   uint32_t color = pack_color(L.rgb[0] / peak, L.rgb[1] / peak, L.rgb[2] / peak, 1);
   if (g_flameLights.count(L.id)) {
      // a flame's light: firelight orange (Thief's torches are a pale 88 73 62,
      // white in the path tracer), and a flicker of its own -- the game's
      // torches burn steadily (their anim lights stay at full)
      float c[3] = {L.rgb[0] / peak, L.rgb[1] / peak, L.rgb[2] / peak};
      static const float kFire[3] = {1.0f, 0.58f, 0.26f};
      for (int k = 0; k < 3; ++k)
         c[k] = c[k] * 0.35f + kFire[k] * 0.65f;
      color = pack_color(c[0], c[1], c[2], 1);
      if (g_cfg.flicker != 2) {
         const double t = now_seconds();
         const float ph = float(L.id % 97) * 0.71f;
         const float n = 0.5f * std::sin(float(t) * 7.3f + ph) + 0.3f * std::sin(float(t) * 13.1f + ph * 2.3f) +
                         0.2f * std::sin(float(t) * 23.7f + ph * 3.7f);
         lumens *= 1.0f + 0.12f * n;
      }
   }
   const float *dir = L.dir;
   RgLightSphericalEXT sphere{};
   RgLightSpotEXT spot{};
   void *ext;
   // A lamp the mapper lit with a spotlight (a pool under it, the walls
   // around it lit by separate dim fill lights): its glass glows, so it is
   // one light all round, a little dimmer -- it was a spot and a sphere at
   // the same place, two lights the sampler took turns with, the shimmer in
   // the pool under it.
   const bool lampSpot = fixture && L.inner >= 0.0f && !g_flameLights.count(L.id);
   if (lampSpot)
      lumens *= 0.6f;
   if (!lampSpot && L.inner >= 0.0f && (dir[0] != 0 || dir[1] != 0 || dir[2] != 0)) {
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

// A torch's light sits where the torch is fixed to the wall, often a few
// centimetres inside it. Dark's own lighting does not mind; a ray-traced
// light inside a wall is shadowed for everything in front of it -- a lit
// torch and a doused one looked the same. Each light is moved out of the
// walls around it (once; the result is kept) until it has room for its size.

uint64_t light_fix_key(const PendingLight &L)
{
   return (uint64_t(L.id) << 40) ^ (uint64_t(uint32_t(int32_t(L.pos[0] * 8))) << 20) ^
          uint64_t(uint32_t(int32_t(L.pos[1] * 8)) * 2654435761u) ^ uint64_t(int32_t(L.pos[2] * 8));
}

// the nearest world surface along each of 26 directions, up to 1 m
float world_clearance(const float p[3], const std::vector<const float *> &tris, float *openDir)
{
   static float dirs[26][3];
   static bool init = false;
   if (!init) {
      int n = 0;
      for (int x = -1; x <= 1; ++x)
         for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
               if (!x && !y && !z)
                  continue;
               const float l = std::sqrt(float(x * x + y * y + z * z));
               dirs[n][0] = x / l; dirs[n][1] = y / l; dirs[n][2] = z / l;
               ++n;
            }
      init = true;
   }
   float minT = 1.0f;
   float open[3] = {0, 0, 0};
   for (const auto &d : dirs) {
      float best = 1.0f;
      for (size_t i = 0; i + 2 < tris.size(); i += 3) {
         float t;
         if (ray_triangle(p, d, tris[i], tris[i + 1], tris[i + 2], t) && t < best)
            best = t;
      }
      minT = std::min(minT, best);
      for (int k = 0; k < 3; ++k)
         open[k] += d[k] * best;
   }
   if (openDir) {
      const float l = std::sqrt(open[0] * open[0] + open[1] * open[1] + open[2] * open[2]);
      for (int k = 0; k < 3; ++k)
         openDir[k] = l > 1e-6f ? open[k] / l : 0.0f;
   }
   return minT;
}

void fix_light_position(const PendingLight &L, LightFix &fix)
{
   fix = LightFix{{0, 0, 0}, 1.0f, true};
   const RgFloat3D c = to_rt(L.pos);
   // the world's triangles within reach
   std::vector<const float *> tris;
   for (const WorldGroup &g : g_world) {
      if (g.flags & (kRenderRTWorldSky | kRenderRTWorldWater | kRenderRTWorldTransparent))
         continue;
      for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
         const float *a = g.verts[g.indices[k]].position, *b = g.verts[g.indices[k + 1]].position,
                     *e = g.verts[g.indices[k + 2]].position;
         bool near = true;
         for (int j = 0; j < 3 && near; ++j)
            near = std::min({a[j], b[j], e[j]}) < c.data[j] + 1.2f &&
                   std::max({a[j], b[j], e[j]}) > c.data[j] - 1.2f;
         if (near) {
            tris.push_back(a);
            tris.push_back(b);
            tris.push_back(e);
         }
      }
   }
   if (tris.empty())
      return;
   const float want = std::min(g_cfg.lightSize + 0.05f, 0.5f);
   float p[3] = {c.data[0], c.data[1], c.data[2]}, open[3];
   float clear = world_clearance(p, tris, open);
   fix.before = clear;
   for (int step = 0; step < 12 && clear < want; ++step) {
      if (open[0] == 0 && open[1] == 0 && open[2] == 0)
         break;
      for (int k = 0; k < 3; ++k)
         p[k] += open[k] * 0.05f;
      clear = world_clearance(p, tris, open);
   }
   if (clear >= want * 0.6f)
      for (int k = 0; k < 3; ++k)
         fix.offset[k] = p[k] - c.data[k];
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
   if (g_freezeLights) {
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
   // Raze RT's light reach: each light fades with how far it is to walk to
   // from the camera, through the openings -- smoothly, in place, so a light
   // never pops in or out where you stand, and none behind walls a long way
   // round is traced at all.
   if (g_cfg.reach > 0) {
      const RgFloat3D c = to_rt(g_camera.pos);
      const float reach = float(g_cfg.reach);
      std::vector<PendingLight> kept;
      for (PendingLight L : g_pendingLights) {
         float d = L.walk;
         const RgFloat3D p = to_rt(L.pos);
         const float straight = std::sqrt((p.data[0] - c.data[0]) * (p.data[0] - c.data[0]) +
                                          (p.data[1] - c.data[1]) * (p.data[1] - c.data[1]) +
                                          (p.data[2] - c.data[2]) * (p.data[2] - c.data[2]));
         if (d == -2.0f)
            d = straight;
         // A light that shines on a cell on screen (the level's light lists
         // say so) is in reach as for a light seen: its straight distance
         // over 2.5. Walked to through the openings, the lamps lighting a
         // big square's far side, round a corner, were out of reach and
         // came on one by one as you walked toward them.
         if (L.reaches && L.id < 0x10000u)
            d = d < 0.0f ? straight / 2.5f : std::min(d, straight / 2.5f);
         if (d < 0.0f)
            continue;
         float t = std::clamp((reach - d) / (reach * 0.33f), 0.0f, 1.0f);
         t = t * t * (3.0f - 2.0f * t);
         if (t <= 0.002f)
            continue;
         for (float &x : L.rgb)
            x *= t;
         L.peak *= t;
         kept.push_back(L);
      }
      g_pendingLights.swap(kept);
   }
   // A light that shines on none of the cells around the camera or in view
   // is behind walls (the level's own lighting says so): every candidate
   // RTGL1 spent on it would be shadowed, the splotches in dark places.
   // Never one within 15 m: those are kept whatever the level's lists say.
   if (g_cfg.lightCull && g_cfg.reach <= 0) {
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
   const size_t keep = g_cfg.reach > 0 ? size_t(512)
                       : g_cfg.maxLights > 0 ? size_t(g_cfg.maxLights) : g_pendingLights.size();
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
   // the test switch: the light nearest the camera off for 20 frames, on for 20
   if (g_blinkTest && !g_pendingLights.empty()) {
      if (g_blinkId == ~0u) {
         float best = 1e30f;
         for (const PendingLight &L : g_pendingLights) {
            const RgFloat3D p = to_rt(L.pos);
            const float dx = p.data[0] - cam.data[0], dy = p.data[1] - cam.data[1],
                        dz = p.data[2] - cam.data[2];
            if (dx * dx + dy * dy + dz * dz < best) {
               best = dx * dx + dy * dy + dz * dz;
               g_blinkId = L.id;
            }
         }
         fprintf(stderr, "RT: blinking light %u\n", g_blinkId);
      }
      if ((s_frame / 20) % 2 == 1)
         g_pendingLights.erase(std::remove_if(g_pendingLights.begin(), g_pendingLights.end(),
                                              [](const PendingLight &L) { return L.id == g_blinkId; }),
                               g_pendingLights.end());
   }
   int fixesLeft = 6;   // the new ones this frame (a few ms at most)
   for (PendingLight &L : g_pendingLights) {
      if (g_cfg.lightFix && L.id < 0x10000u) {
         LightFix &fix = g_lightFixes[light_fix_key(L)];
         if (!fix.done && fixesLeft-- > 0)
            fix_light_position(L, fix);
         if (fix.done)
            for (int k = 0; k < 3; ++k)
               L.pos[k] += fix.offset[k] / kMetersPerUnit;
      }
      s_chosen[L.id] = s_frame;
   }
   // A light that comes into the traced set (walking up to it) fades in over
   // a moment, and one that drops out of it fades out, instead of appearing
   // or vanishing at once: the lights "loading in" ahead of you. One switched
   // on or put out in the game changes at once, as there.
   {
      static std::unordered_map<uint32_t, float> s_fade;
      static std::unordered_map<uint32_t, PendingLight> s_seen;
      static std::unordered_map<uint32_t, bool> s_dimBefore;
      static auto s_t = std::chrono::steady_clock::now();
      const auto tn = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(tn - s_t).count(), 0.0f, 0.1f);
      s_t = tn;
      const float step = g_cfg.lightFade ? dt / 0.6f : 1.0f;
      const bool first = s_seen.empty();
      std::unordered_map<uint32_t, bool> chosen, offered, dim;
      for (const PendingLight &L : g_pendingLights)
         chosen[L.id] = true;
      for (const PendingLight &L : g_lastOffered)
         offered[L.id] = true;
      for (const PendingLight &L : g_lastDim)
         dim[L.id] = true;
      auto upload_scaled = [](PendingLight L, float f) {
         f = std::clamp(f, 0.0f, 1.0f);
         if (f < 1.0f) {
            const float peak = std::max(L.peak * f, 0.01f);
            const float scale = peak / std::max(L.peak, 0.01f);
            for (float &c : L.rgb)
               c *= scale;
            L.peak = peak;
         }
         upload_light(L);
      };
      for (const PendingLight &L : g_pendingLights) {
         auto it = s_fade.find(L.id);
         if (it == s_fade.end()) {
            if (!first && s_dimBefore.count(L.id))
               g_lightEventFrames = 10;   // lit in the game
            it = s_fade.emplace(L.id, (first || s_dimBefore.count(L.id)) ? 1.0f : 0.0f).first;
         }
         it->second = std::min(1.0f, it->second + step);
         upload_scaled(L, it->second);
         s_seen[L.id] = L;
      }
      for (auto it = s_seen.begin(); it != s_seen.end();) {
         const uint32_t id = it->first;
         if (chosen.count(id)) {
            ++it;
            continue;
         }
         float &f = s_fade[id];
         // out in the game (or gone): not faded, the ghost below takes it
         if (dim.count(id))
            g_lightEventFrames = 10;   // put out in the game
         if (dim.count(id) || !offered.count(id) || g_cfg.reach > 0 || (f -= step) <= 0.0f) {
            s_fade.erase(id);
            it = s_seen.erase(it);
            continue;
         }
         // the light as the game has it now, at its fading strength
         PendingLight L = it->second;
         // (with the reach, it faded out in place: as last given)
         if (g_cfg.reach <= 0)
         for (const PendingLight &O : g_lastOffered)
            if (O.id == id) {
               const float off[3] = {L.pos[0] - O.pos[0], L.pos[1] - O.pos[1], L.pos[2] - O.pos[2]};
               L = O;
               // keep the moved-out-of-the-wall position
               for (int k = 0; k < 3; ++k)
                  L.pos[k] += off[k];
               break;
            }
         upload_scaled(L, f);
         chosen[id] = true;   // still given: not a light gone for the ghost below
         ++it;
      }
      s_dimBefore = dim;
      // the ones still fading count as given
      std::vector<PendingLight> stillFading;
      for (const auto &e : s_seen)
         if (!std::any_of(g_pendingLights.begin(), g_pendingLights.end(),
                          [&e](const PendingLight &L) { return L.id == e.first; }))
            stillFading.push_back(e.second);
      g_fadingOut.swap(stillFading);
   }
   // A light that goes out (a doused torch) or is no longer among those
   // traced is still given, at no brightness, for half a second. Gone from
   // the list at once, the denoiser could not measure the change -- its
   // test re-lights last frame's samples with this frame's lights, and a
   // light that is not there is not a change -- so the torch's glow on the
   // walls stayed for seconds. Seen dark, the change is found at once.
   {
      static std::unordered_map<uint32_t, std::pair<PendingLight, int>> s_fading;
      static std::vector<PendingLight> s_prev;
      std::unordered_map<uint32_t, bool> now;
      for (const PendingLight &L : g_pendingLights)
         now[L.id] = true;
      for (const PendingLight &L : g_fadingOut)
         now[L.id] = true;
      {
         // the traced set's changes, for matching against lighting that pops
         static int s_lines = 0;
         std::unordered_map<uint32_t, bool> before;
         for (const PendingLight &L : s_prev)
            before[L.id] = true;
         auto dist = [&cam](const PendingLight &L) {
            const RgFloat3D p = to_rt(L.pos);
            const float dx = p.data[0] - cam.data[0], dy = p.data[1] - cam.data[1],
                        dz = p.data[2] - cam.data[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
         };
         if (!s_prev.empty() && s_lines < 4000) {
            for (const PendingLight &L : g_pendingLights)
               if (!before.count(L.id) && s_lines++ < 4000)
                  fprintf(stderr, "RT: light %u joins the traced set, %.1f m away (frame %u)\n", L.id,
                          dist(L), g_frameNumber);
            for (const PendingLight &L : s_prev)
               if (!now.count(L.id) && s_lines++ < 4000)
                  fprintf(stderr, "RT: light %u leaves the traced set, %.1f m away (frame %u)\n", L.id,
                          dist(L), g_frameNumber);
         }
      }
      for (const PendingLight &L : s_prev)
         if (!now.count(L.id) && !s_fading.count(L.id) &&
             // (one faded out by its distance is not a change: not given dark)
             (g_cfg.reach <= 0 || std::any_of(g_lastDim.begin(), g_lastDim.end(),
                                              [&L](const PendingLight &D) { return D.id == L.id; })))
            s_fading[L.id] = {L, 30};
      for (auto it = s_fading.begin(); it != s_fading.end();) {
         if (now.count(it->first) || --it->second.second < 0) {
            it = s_fading.erase(it);
            continue;
         }
         PendingLight dark = it->second.first;
         const float scale = 0.01f / std::max(dark.peak, 0.01f);
         for (float &c : dark.rgb)
            c *= scale;
         dark.peak = 0.01f;
         upload_light(dark);
         ++it;
      }
      s_prev = g_pendingLights;
      s_prev.insert(s_prev.end(), g_fadingOut.begin(), g_fadingOut.end());
   }
   g_lastLights = g_pendingLights;
   g_pendingLights.clear();
}

} // namespace

extern "C" {

} // extern "C"
#pragma pack(pop)

namespace darkrt {
void ApplyOutputSize()
{
   if (g_active)
      apply_output_size();
}
} // namespace darkrt
