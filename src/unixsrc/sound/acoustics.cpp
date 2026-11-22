// Traced sound with Steam Audio (acoustics.h).
//
// The level's walls (portal.c, portal_sound_world) go into a Steam Audio scene once a
// mission is loaded, each with what it is made of (from its texture's name: stone, brick,
// wood, metal, glass, cloth, earth...). A worker thread then traces, about ten times a
// second:
//   - rays from the listener, bounced round the walls: the place's reverb -- how loud,
//     how long, how bright, how soon, from which side (Steam Audio's own I3DL2 estimate
//     of the energy that comes back, libphonon's iplThiefSourceGetReverb) -- and how
//     enclosed it is (rays that find no wall: out in the open);
//   - from each playing sound's source to the listener: whether walls stand between (a
//     sound reaching round a corner loses its highs).
// libphonon is opened at run time (the kit builds it into the app's Frameworks); without
// it, or turned off, the game's sound is as it was.

// The engine is built with byte packing (-fpack-struct=1); Steam Audio's structs, the
// system's and the standard library's have their natural layout: all of this file has it.
#pragma pack(push, 8)

#include <acoustics.h>
#include <resimginfo.h>

#include "phonon/phonon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// (SDK headers end with "#pragma pack()", back to the engine's packing)
#pragma pack(8)

extern "C" unsigned portal_sound_world(void (*fn)(void *user, int n, const float *xyz,
                                                  void *texture, int flags), void *user);
extern "C" int wr_num_cells;

namespace {

constexpr float kMetersPerFoot = 0.3048f;
constexpr int kSlots = 128;            // appsfx's SFX_MAX_COUNT
constexpr int kRays = 4096;            // per reverb trace
constexpr int kBounces = 16;
constexpr float kDuration = 2.0f;      // s of reverb traced
constexpr int kOrder = 1;              // directions: first order

// the Dark world (x, y, z up, feet) to Steam Audio's (x right, y up, z back; meters)
inline IPLVector3 to_sa(const float v[3], float scale = kMetersPerFoot)
{
   return {v[0] * scale, v[2] * scale, -v[1] * scale};
}

// ---- libphonon ---------------------------------------------------------------------

typedef struct {
   IPLfloat32 room, roomHigh, roomLow, decayTime, decayHighRatio, decayLowRatio;
   IPLfloat32 reflections, reflectionsDelay, reverb, reverbDelay, diffusion, density;
   IPLfloat32 earlyDirection[3], lateDirection[3];
   IPLint32 valid;
} ThiefReverb;   // (libphonon's api_thief.cpp)

struct Phonon
{
   void *lib = nullptr;
#define P(name) decltype(&::name) name = nullptr;
   P(iplContextCreate) P(iplContextRelease) P(iplSceneCreate) P(iplSceneRelease) P(iplSceneCommit)
   P(iplStaticMeshCreate) P(iplStaticMeshRelease) P(iplStaticMeshAdd) P(iplSimulatorCreate)
   P(iplSimulatorRelease) P(iplSimulatorSetScene) P(iplSimulatorCommit) P(iplSimulatorSetSharedInputs)
   P(iplSimulatorRunDirect) P(iplSimulatorRunReflections) P(iplSourceCreate) P(iplSourceRelease)
   P(iplSourceAdd) P(iplSourceRemove) P(iplSourceSetInputs) P(iplSourceGetOutputs)
#undef P
   IPLerror (*iplThiefSourceGetReverb)(IPLSource, ThiefReverb *) = nullptr;
   IPLfloat32 (*iplThiefSceneRaycast)(IPLScene, IPLVector3, IPLVector3, IPLfloat32) = nullptr;

   bool load(std::string &where)
   {
      std::vector<std::string> candidates;
      if (const char *p = getenv("THIEF_PHONON"))
         candidates.push_back(p);
#if defined(__APPLE__)
      const char *file = "libphonon.dylib";
      char exe[PATH_MAX];
      uint32_t size = sizeof(exe);
      if (_NSGetExecutablePath(exe, &size) == 0) {
         std::string e(exe);
         e = e.substr(0, e.rfind('/'));                   // .../Contents/MacOS
         candidates.push_back(e + "/../Frameworks/" + file);
         candidates.push_back(e + "/../Resources/rt/bin/" + file);
         candidates.push_back(e + "/../../../rt/bin/" + file);
      }
#else
      const char *file = "libphonon.so";
      char exe[PATH_MAX];
      const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
      if (n > 0) {
         exe[n] = 0;
         std::string e(exe);
         e = e.substr(0, e.rfind('/'));
         candidates.push_back(e + "/" + file);
         candidates.push_back(e + "/rt/bin/" + file);
      }
#endif
      candidates.push_back(std::string("rt/bin/") + file);
      candidates.push_back(file);
      for (const std::string &c : candidates) {
         if (c.find('/') != std::string::npos) {
            struct stat st;
            if (stat(c.c_str(), &st) != 0)
               continue;
         }
         lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
         if (lib) {
            where = c;
            break;
         }
      }
      if (!lib)
         return false;
      bool ok = true;
#define P(name) ok = ok && (name = reinterpret_cast<decltype(name)>(dlsym(lib, #name))) != nullptr;
      P(iplContextCreate) P(iplContextRelease) P(iplSceneCreate) P(iplSceneRelease) P(iplSceneCommit)
      P(iplStaticMeshCreate) P(iplStaticMeshRelease) P(iplStaticMeshAdd) P(iplSimulatorCreate)
      P(iplSimulatorRelease) P(iplSimulatorSetScene) P(iplSimulatorCommit) P(iplSimulatorSetSharedInputs)
      P(iplSimulatorRunDirect) P(iplSimulatorRunReflections) P(iplSourceCreate) P(iplSourceRelease)
      P(iplSourceAdd) P(iplSourceRemove) P(iplSourceSetInputs) P(iplSourceGetOutputs)
      P(iplThiefSourceGetReverb) P(iplThiefSceneRaycast)
#undef P
      if (!ok) {
         dlclose(lib);
         lib = nullptr;
      }
      return ok;
   }
};

// ---- what the walls are made of ------------------------------------------------------

enum Mat { kStone, kRock, kBrick, kMarble, kPlaster, kWood, kMetal, kGlass, kCloth, kEarth, kWater, kMatCount };

// absorption (400 Hz, 2.5 kHz, 15 kHz), scattering, transmission (as Steam Audio's own table)
const IPLMaterial kMaterials[kMatCount] = {
   {{0.05f, 0.07f, 0.08f}, 0.10f, {0.015f, 0.002f, 0.001f}},   // stone (dressed, as concrete)
   {{0.13f, 0.20f, 0.24f}, 0.40f, {0.015f, 0.002f, 0.001f}},   // rock (rough, caves)
   {{0.03f, 0.04f, 0.07f}, 0.20f, {0.015f, 0.015f, 0.015f}},   // brick
   {{0.01f, 0.02f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}},   // marble, tile (as ceramic)
   {{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}},   // plaster
   {{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}},   // wood
   {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}},   // metal
   {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}},   // glass
   {{0.24f, 0.69f, 0.73f}, 0.05f, {0.020f, 0.005f, 0.003f}},   // cloth, carpet
   {{0.30f, 0.55f, 0.70f}, 0.50f, {0.015f, 0.002f, 0.001f}},   // earth, grass
   {{0.01f, 0.01f, 0.02f}, 0.05f, {0.300f, 0.100f, 0.050f}},   // water (its surface)
};
const char *const kMatNames[kMatCount] = {"stone", "rock", "brick", "marble", "plaster", "wood", "metal",
                                          "glass", "cloth", "earth", "water"};

bool has(const std::string &s, const char *k) { return s.find(k) != std::string::npos; }

// by its texture's path (fam/city/marble: lower case), as the path tracer's material
// table names surfaces; stone if nothing says otherwise -- most of Thief is stone
Mat material_of(std::string name)
{
   for (char &c : name)
      c = char(tolower((unsigned char)c));
   const std::string leaf = name.substr(name.rfind('/') + 1);
   if (has(name, "water"))
      return kWater;
   if (has(name, "glass") || has(name, "glpries") || has(name, "glsmelt") ||
       leaf.compare(0, 3, "win") == 0 || has(leaf, "window"))
      return kGlass;
   if (has(name, "carp") || has(name, "rug") || has(name, "curtain") || has(name, "banner") ||
       has(name, "cloth") || has(name, "tapest") || has(name, "fabric") || has(name, "drape"))
      return kCloth;
   if (has(name, "grass") || has(name, "dirt") || has(name, "mud") || has(name, "earth") ||
       has(name, "sand") || has(name, "soil") || has(name, "leaf") || has(name, "leaves") ||
       has(name, "moss") || has(name, "hay") || has(name, "straw") || has(name, "ground"))
      return kEarth;
   if (has(name, "rust") || has(name, "iron") || has(name, "bronz") || has(name, "brass") ||
       has(name, "metal") || has(name, "gird") || has(name, "grate") || has(name, "grat") ||
       has(name, "gear") || has(name, "pipe") || has(name, "steel") || has(name, "copper") ||
       has(name, "gold"))
      return kMetal;
   if (has(name, "marb") || has(name, "marwal") || has(name, "tile") || has(name, "mosaic") ||
       (leaf.size() > 3 && leaf.compare(0, 3, "mar") == 0 && isdigit((unsigned char)leaf[3])))
      return kMarble;
   if (has(name, "wood") || has(name, "plank") || has(name, "plnk") || has(name, "parkay") ||
       has(name, "wgrain") || has(name, "beam") || has(name, "bark") || has(name, "door") ||
       has(name, "book") || leaf.compare(0, 2, "wd") == 0)
      return kWood;
   if (has(name, "stuc") || has(name, "plas") || has(name, "paper") || has(name, "cement"))
      return kPlaster;
   if (has(name, "bric") || has(name, "brik"))
      return kBrick;
   if (has(name, "rock") || has(name, "cave") || has(name, "crag") || has(name, "/rubble/") ||
       has(name, "/mine/") || (leaf.size() > 3 && leaf.compare(0, 3, "roc") == 0))
      return kRock;
   if (has(name, "/sewer"))
      return kBrick;
   return kStone;
}

// ---- the level, as the worker will have it ----------------------------------------------

struct Geometry
{
   unsigned serial = 0;
   std::vector<IPLVector3> vertices;
   std::vector<IPLTriangle> triangles;
   std::vector<IPLint32> materials;
   int count[kMatCount] = {};
};

struct Collect
{
   Geometry *g;
   std::unordered_map<void *, Mat> known;
};

void collect_poly(void *user, int n, const float *xyz, void *texture, int flags)
{
   Collect &c = *static_cast<Collect *>(user);
   Mat m = kStone;
   if (flags & 1)
      m = kWater;
   else if (texture) {
      auto it = c.known.find(texture);
      if (it == c.known.end()) {
         sResImageInfo info;
         const Mat found = (ResImageInfoFind(texture, &info) && info.name[0]) ? material_of(info.name) : kStone;
         it = c.known.emplace(texture, found).first;
      }
      m = it->second;
   }
   Geometry &g = *c.g;
   const int base = int(g.vertices.size());
   for (int i = 0; i < n; ++i)
      g.vertices.push_back(to_sa(xyz + 3 * i));
   for (int i = 1; i + 1 < n; ++i) {
      g.triangles.push_back({{base, base + i, base + i + 1}});
      g.materials.push_back(m);
      ++g.count[m];
   }
}

// ---- state shared by the game and the worker -----------------------------------------------

struct Slot
{
   IPLVector3 pos{};
   double lastSeen = -1.0;     // s (steady clock), < 0: not playing
   float target = 1.0f;        // the worker's latest
   float clear = 1.0f;         // eased toward it, as the game asks
};

struct Shared
{
   std::mutex lock;
   std::condition_variable wake;
   bool quit = false;
   std::unique_ptr<Geometry> pending;    // a new level for the worker
   IPLCoordinateSpace3 listener{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 0}};
   bool haveListener = false;
   Slot slots[kSlots];
   sAcousticReverb reverb{};             // the worker's latest
   bool reverbValid = false;
   float enclosure = 1.0f;
   double traceMs = 0.0;
   int triangles = 0;
};

struct Settings
{
   int enabled = 1;
   int hrtf = 0;
};

Settings g_settings;
bool g_settingsLoaded = false;
Shared g_shared;
Phonon g_api;
std::thread g_worker;
bool g_started = false, g_failed = false;
std::string g_status = "not started";
unsigned g_serial = 0;
int g_frames = 0;
sAcousticReverb g_eased{};
bool g_easedValid = false;

double now_s()
{
   return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string settings_path()
{
   const char *home = getenv("HOME");
   if (!home || !*home)
      return "sound_settings.txt";
#if defined(__APPLE__)
   return std::string(home) + "/Library/Application Support/Dark Engine Restored/sound_settings.txt";
#else
   return std::string(home) + "/.config/dark-engine-restored/sound_settings.txt";
#endif
}

void load_settings()
{
   if (g_settingsLoaded)
      return;
   g_settingsLoaded = true;
   if (FILE *f = fopen(settings_path().c_str(), "r")) {
      char key[64];
      int value;
      while (fscanf(f, "%63s %d", key, &value) == 2) {
         if (!strcmp(key, "traced_sound")) g_settings.enabled = value ? 1 : 0;
         else if (!strcmp(key, "hrtf")) g_settings.hrtf = std::clamp(value, 0, 2);
      }
      fclose(f);
   }
}

void save_settings()
{
   if (FILE *f = fopen(settings_path().c_str(), "w")) {
      fprintf(f, "traced_sound %d\nhrtf %d\n", g_settings.enabled, g_settings.hrtf);
      fclose(f);
   }
}

// ---- the worker -----------------------------------------------------------------------------

inline float dot(const IPLVector3 &a, const IPLVector3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// the energy that came back, as an EAX reverb (calibrated so a 10 x 10 x 4 m stone room
// sounds as loud as the game's own "generic" room did)
sAcousticReverb to_reverb(const ThiefReverb &r, float enclosure, const IPLCoordinateSpace3 &l)
{
   sAcousticReverb o{};
   const float decay = std::clamp(r.decayTime, 0.1f, 20.0f);
   const float early = std::pow(10.0f, (r.room + r.reflections) / 1000.0f);
   const float late = std::pow(10.0f, (r.room + r.reverb) / 1000.0f);
   const float earlyRef = std::pow(10.0f, -1.943f);
   const float lateRef = std::pow(10.0f, -1.941f) / 0.67f;
   o.gain = 0.316f;
   o.gainHF = std::clamp(std::pow(10.0f, (r.roomHigh - r.room) / 2000.0f), 0.01f, 1.0f);
   o.gainLF = std::clamp(std::pow(10.0f, (r.roomLow - r.room) / 2000.0f), 0.01f, 1.0f);
   o.decay = decay;
   o.decayHFRatio = std::clamp(r.decayHighRatio, 0.1f, 2.0f);
   o.decayLFRatio = std::clamp(r.decayLowRatio, 0.1f, 2.0f);
   o.reflectionsGain = std::clamp(0.3f * std::sqrt(early / earlyRef) * (0.3f + 0.7f * enclosure), 0.0f, 3.16f);
   o.reflectionsDelay = std::clamp(r.reflectionsDelay, 0.0f, 0.3f);
   o.lateGain = std::clamp(1.1f * std::sqrt((late / decay) / lateRef) * std::pow(enclosure, 1.5f), 0.0f, 10.0f);
   o.lateDelay = std::clamp(r.reverbDelay, 0.0f, 0.1f);
   o.density = std::clamp(r.density / 100.0f, 0.0f, 1.0f);
   o.diffusion = std::clamp(r.diffusion / 100.0f, 0.0f, 1.0f);
   // the directions, into the listener's frame (EFX: x right, y up, z ahead), gently
   const IPLVector3 &right = l.right;
   const IPLVector3 e{r.earlyDirection[0], r.earlyDirection[1], r.earlyDirection[2]};
   const IPLVector3 t{r.lateDirection[0], r.lateDirection[1], r.lateDirection[2]};
   const float focus = 0.6f;
   o.reflectionsPan[0] = focus * dot(e, right);
   o.reflectionsPan[1] = focus * dot(e, l.up);
   o.reflectionsPan[2] = focus * dot(e, l.ahead);
   o.latePan[0] = focus * dot(t, right);
   o.latePan[1] = focus * dot(t, l.up);
   o.latePan[2] = focus * dot(t, l.ahead);
   return o;
}

void worker_main()
{
   Phonon &P = g_api;
   IPLContextSettings cs{};
   cs.version = STEAMAUDIO_VERSION;
#if defined(__aarch64__) || defined(__arm64__)
   cs.simdLevel = IPL_SIMDLEVEL_NEON;
#else
   cs.simdLevel = IPL_SIMDLEVEL_SSE4;
#endif
   IPLContext ctx = nullptr;
   if (P.iplContextCreate(&cs, &ctx) != IPL_STATUS_SUCCESS) {
      fprintf(stderr, "Sound: Steam Audio would not start; the game's own sound is used\n");
      return;
   }
   IPLSimulationSettings sim{};
   sim.flags = IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
   sim.sceneType = IPL_SCENETYPE_DEFAULT;
   sim.reflectionType = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
   sim.maxNumOcclusionSamples = 16;
   sim.maxNumRays = kRays;
   sim.numDiffuseSamples = 32;
   sim.maxDuration = kDuration;
   sim.maxOrder = kOrder;
   sim.maxNumSources = kSlots + 1;
   sim.numThreads = 2;
   sim.samplingRate = 44100;
   sim.frameSize = 1024;
   IPLSimulator simulator = nullptr;
   if (P.iplSimulatorCreate(ctx, &sim, &simulator) != IPL_STATUS_SUCCESS) {
      fprintf(stderr, "Sound: Steam Audio's simulator would not start; the game's own sound is used\n");
      P.iplContextRelease(&ctx);
      return;
   }
   IPLSource reverbSource = nullptr, sources[kSlots] = {};
   IPLSourceSettings rs{};
   rs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
   P.iplSourceCreate(simulator, &rs, &reverbSource);
   P.iplSourceAdd(reverbSource, simulator);
   IPLSourceSettings ds{};
   ds.flags = IPL_SIMULATIONFLAGS_DIRECT;
   for (int i = 0; i < kSlots; ++i) {
      P.iplSourceCreate(simulator, &ds, &sources[i]);
      P.iplSourceAdd(sources[i], simulator);
   }
   P.iplSimulatorCommit(simulator);

   IPLScene scene = nullptr;
   IPLStaticMesh mesh = nullptr;

   // directions round and above the listener, for how enclosed the place is (none
   // below: the floor says nothing of it)
   std::vector<IPLVector3> around;
   for (int ring = 0; ring < 4; ++ring) {
      const float elev = ring * 0.4f;                 // 0, 23, 46, 69 degrees
      const int n = 12 - ring * 2;
      for (int i = 0; i < n; ++i) {
         const float a = 6.2831853f * (i + 0.5f * ring) / n;
         around.push_back({std::cos(elev) * std::cos(a), std::sin(elev), std::cos(elev) * std::sin(a)});
      }
   }
   around.push_back({0, 1, 0});

   float enclosure = 1.0f;
   for (;;) {
      std::unique_ptr<Geometry> level;
      IPLCoordinateSpace3 listener;
      bool haveListener;
      bool alive[kSlots];
      IPLVector3 pos[kSlots];
      {
         std::unique_lock<std::mutex> lk(g_shared.lock);
         g_shared.wake.wait_for(lk, std::chrono::milliseconds(scene ? 90 : 500),
                                [] { return g_shared.quit || g_shared.pending; });
         if (g_shared.quit)
            break;
         level = std::move(g_shared.pending);
         listener = g_shared.listener;
         haveListener = g_shared.haveListener;
         const double t = now_s();
         for (int i = 0; i < kSlots; ++i) {
            alive[i] = g_shared.slots[i].lastSeen >= 0.0 && t - g_shared.slots[i].lastSeen < 1.0;
            pos[i] = g_shared.slots[i].pos;
         }
      }
      if (level) {
         const auto t0 = std::chrono::steady_clock::now();
         if (mesh)
            P.iplStaticMeshRelease(&mesh);
         if (scene)
            P.iplSceneRelease(&scene);
         IPLSceneSettings ss{};
         ss.type = IPL_SCENETYPE_DEFAULT;
         if (!level->triangles.empty() && P.iplSceneCreate(ctx, &ss, &scene) == IPL_STATUS_SUCCESS) {
            IPLStaticMeshSettings ms{};
            ms.numVertices = int(level->vertices.size());
            ms.numTriangles = int(level->triangles.size());
            ms.numMaterials = kMatCount;
            ms.vertices = level->vertices.data();
            ms.triangles = level->triangles.data();
            ms.materialIndices = level->materials.data();
            ms.materials = const_cast<IPLMaterial *>(kMaterials);
            if (P.iplStaticMeshCreate(scene, &ms, &mesh) == IPL_STATUS_SUCCESS) {
               P.iplStaticMeshAdd(mesh, scene);
               P.iplSceneCommit(scene);
            } else {
               P.iplSceneRelease(&scene);
            }
         }
         P.iplSimulatorSetScene(simulator, scene);
         P.iplSimulatorCommit(simulator);
         std::string made;
         for (int m = 0; m < kMatCount; ++m)
            if (level->count[m]) {
               char b[48];
               snprintf(b, sizeof(b), "%s%s %d%%", made.empty() ? "" : ", ", kMatNames[m],
                        int(100.0 * level->count[m] / level->triangles.size() + 0.5));
               made += b;
            }
         fprintf(stderr, "Sound: the level's walls traced for sound: %zu triangles (%s), in %.0f ms\n",
                 level->triangles.size(), made.c_str(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
         std::lock_guard<std::mutex> lk(g_shared.lock);
         g_shared.triangles = int(level->triangles.size());
         g_shared.reverbValid = false;
      }
      if (!scene || !haveListener)
         continue;

      const auto t0 = std::chrono::steady_clock::now();
      IPLSimulationSharedInputs sh{};
      sh.listener = listener;
      sh.numRays = kRays;
      sh.numBounces = kBounces;
      sh.duration = kDuration;
      sh.order = kOrder;
      sh.irradianceMinDistance = 1.0f;
      P.iplSimulatorSetSharedInputs(simulator, IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &sh);

      IPLSimulationInputs in{};
      in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
      in.source = listener;
      in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
      in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
      in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
      P.iplSourceSetInputs(reverbSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);

      for (int i = 0; i < kSlots; ++i) {
         IPLSimulationInputs di{};
         di.flags = IPL_SIMULATIONFLAGS_DIRECT;
         di.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
         di.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
         if (alive[i]) {
            di.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
            di.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            di.occlusionRadius = 0.6f;
            di.numOcclusionSamples = 12;
            di.source = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, pos[i]};
         } else {
            di.directFlags = IPLDirectSimulationFlags(0);
            di.source = listener;
         }
         P.iplSourceSetInputs(sources[i], IPL_SIMULATIONFLAGS_DIRECT, &di);
      }
      P.iplSimulatorCommit(simulator);
      P.iplSimulatorRunDirect(simulator);
      P.iplSimulatorRunReflections(simulator);

      // how enclosed: the share of rays round and above that meet a wall within 40 m
      int hit = 0;
      for (const IPLVector3 &d : around)
         if (P.iplThiefSceneRaycast(scene, listener.origin, d, 40.0f) >= 0.0f)
            ++hit;
      const float enclosed = float(hit) / float(around.size());
      enclosure += (enclosed - enclosure) * 0.5f;

      ThiefReverb tr{};
      const bool haveReverb = P.iplThiefSourceGetReverb(reverbSource, &tr) == IPL_STATUS_SUCCESS && tr.valid;
      float clear[kSlots];
      for (int i = 0; i < kSlots; ++i) {
         clear[i] = 1.0f;
         if (alive[i]) {
            IPLSimulationOutputs out{};
            P.iplSourceGetOutputs(sources[i], IPL_SIMULATIONFLAGS_DIRECT, &out);
            clear[i] = std::clamp(out.direct.occlusion, 0.0f, 1.0f);
         }
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

      std::lock_guard<std::mutex> lk(g_shared.lock);
      for (int i = 0; i < kSlots; ++i)
         g_shared.slots[i].target = clear[i];
      if (haveReverb) {
         g_shared.reverb = to_reverb(tr, enclosure, listener);
         g_shared.reverbValid = true;
      }
      g_shared.enclosure = enclosure;
      g_shared.traceMs += (ms - g_shared.traceMs) * 0.2;
   }

   for (int i = 0; i < kSlots; ++i) {
      P.iplSourceRemove(sources[i], simulator);
      P.iplSourceRelease(&sources[i]);
   }
   P.iplSourceRemove(reverbSource, simulator);
   P.iplSourceRelease(&reverbSource);
   P.iplSimulatorRelease(&simulator);
   if (mesh)
      P.iplStaticMeshRelease(&mesh);
   if (scene)
      P.iplSceneRelease(&scene);
   P.iplContextRelease(&ctx);
}

void stop_worker()
{
   if (!g_started)
      return;
   {
      std::lock_guard<std::mutex> lk(g_shared.lock);
      g_shared.quit = true;
   }
   g_shared.wake.notify_all();
   if (g_worker.joinable())
      g_worker.join();
   g_started = false;
}

bool start()
{
   if (g_started)
      return true;
   if (g_failed)
      return false;
   std::string where;
   if (!g_api.lib && !g_api.load(where)) {
      g_failed = true;
      g_status = "libphonon not found";
      fprintf(stderr, "Sound: Steam Audio (libphonon) not found; the game's own sound is used\n");
      return false;
   }
   if (!where.empty())
      fprintf(stderr, "Sound: Steam Audio from %s\n", where.c_str());
   g_shared.quit = false;
   g_serial = 0;
   g_worker = std::thread(worker_main);
   g_started = true;
   static bool once = false;
   if (!once) {
      once = true;
      atexit(stop_worker);
   }
   return true;
}

void ease(float &v, float to, float k) { v += (to - v) * k; }

}  // namespace

extern "C" {

void Acoustics_Frame(const float head[3], const float ahead[3], const float up[3])
{
   load_settings();
   if (!g_settings.enabled) {
      if (g_started)
         stop_worker();
      return;
   }
   if (!start())
      return;
   // a new level: its walls, for the worker
   if (++g_frames % 30 == 1 && wr_num_cells > 0) {
      const unsigned serial = portal_sound_world(nullptr, nullptr);
      if (serial != g_serial) {
         g_serial = serial;
         auto g = std::make_unique<Geometry>();
         g->serial = serial;
         Collect c{g.get(), {}};
         portal_sound_world(collect_poly, &c);
         std::lock_guard<std::mutex> lk(g_shared.lock);
         g_shared.pending = std::move(g);
         g_shared.wake.notify_all();
      }
   }
   std::lock_guard<std::mutex> lk(g_shared.lock);
   g_shared.listener.origin = to_sa(head);
   g_shared.listener.ahead = to_sa(ahead, 1.0f);
   g_shared.listener.up = to_sa(up, 1.0f);
   g_shared.listener.right = {g_shared.listener.ahead.y * g_shared.listener.up.z - g_shared.listener.ahead.z * g_shared.listener.up.y,
                              g_shared.listener.ahead.z * g_shared.listener.up.x - g_shared.listener.ahead.x * g_shared.listener.up.z,
                              g_shared.listener.ahead.x * g_shared.listener.up.y - g_shared.listener.ahead.y * g_shared.listener.up.x};
   g_shared.haveListener = true;
}

int Acoustics_GetReverb(sAcousticReverb *out)
{
   if (!g_settings.enabled || !g_started || !out)
      return 0;
   sAcousticReverb r;
   {
      std::lock_guard<std::mutex> lk(g_shared.lock);
      if (!g_shared.reverbValid) {
         g_easedValid = false;
         return 0;
      }
      r = g_shared.reverb;
   }
   if (!g_easedValid) {
      g_eased = r;
      g_easedValid = true;
   } else {
      // over a few tenths of a second, so walking through a doorway is a change, not a jump
      const float k = 0.08f;
      ease(g_eased.gain, r.gain, k);
      ease(g_eased.gainHF, r.gainHF, k);
      ease(g_eased.gainLF, r.gainLF, k);
      ease(g_eased.decay, r.decay, k);
      ease(g_eased.decayHFRatio, r.decayHFRatio, k);
      ease(g_eased.decayLFRatio, r.decayLFRatio, k);
      ease(g_eased.reflectionsGain, r.reflectionsGain, k);
      ease(g_eased.reflectionsDelay, r.reflectionsDelay, k);
      ease(g_eased.lateGain, r.lateGain, k);
      ease(g_eased.lateDelay, r.lateDelay, k);
      ease(g_eased.density, r.density, k);
      ease(g_eased.diffusion, r.diffusion, k);
      for (int i = 0; i < 3; ++i) {
         ease(g_eased.reflectionsPan[i], r.reflectionsPan[i], k);
         ease(g_eased.latePan[i], r.latePan[i], k);
      }
   }
   *out = g_eased;
   return 1;
}

float Acoustics_SourceClear(int slot, const float src[3])
{
   if (!g_settings.enabled || !g_started || slot < 0 || slot >= kSlots || !src)
      return 1.0f;
   std::lock_guard<std::mutex> lk(g_shared.lock);
   if (!g_shared.reverbValid && g_shared.triangles == 0)
      return 1.0f;
   Slot &s = g_shared.slots[slot];
   const bool fresh = s.lastSeen < 0.0;
   s.pos = to_sa(src);
   s.lastSeen = now_s();
   if (fresh) {
      s.target = s.clear = 1.0f;   // until it is traced
      return 1.0f;
   }
   ease(s.clear, s.target, 0.25f);
   return s.clear;
}

void Acoustics_SourceStop(int slot)
{
   if (slot < 0 || slot >= kSlots)
      return;
   std::lock_guard<std::mutex> lk(g_shared.lock);
   g_shared.slots[slot].lastSeen = -1.0;
   g_shared.slots[slot].target = g_shared.slots[slot].clear = 1.0f;
}

int Acoustics_Enabled(void)
{
   load_settings();
   return g_settings.enabled;
}

void Acoustics_SetEnabled(int on)
{
   load_settings();
   g_settings.enabled = on ? 1 : 0;
   save_settings();
   if (!g_settings.enabled)
      stop_worker();
   g_easedValid = false;
   std::lock_guard<std::mutex> lk(g_shared.lock);
   g_shared.reverbValid = false;
}

int Acoustics_HrtfMode(void)
{
   load_settings();
   return g_settings.hrtf;
}

void Acoustics_SetHrtfMode(int mode)
{
   load_settings();
   g_settings.hrtf = std::clamp(mode, 0, 2);
   save_settings();
   OpenALApplyHrtf(g_settings.hrtf);
}

const char *Acoustics_Status(void)
{
   static char buf[160];
   load_settings();
   if (!g_settings.enabled)
      return "Off (the game's room reverb)";
   if (g_failed)
      return "Off: libphonon not found";
   if (!g_started)
      return "On";
   std::lock_guard<std::mutex> lk(g_shared.lock);
   if (!g_shared.reverbValid) {
      snprintf(buf, sizeof(buf), "On (tracing %d triangles...)", g_shared.triangles);
      return buf;
   }
   snprintf(buf, sizeof(buf), "On: reverb %.1f s, %.0f%% enclosed, %.0f ms a trace", g_shared.reverb.decay,
            g_shared.enclosure * 100.0f, g_shared.traceMs);
   return buf;
}

}  // extern "C"

#pragma pack(pop)
