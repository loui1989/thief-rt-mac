// What the engine knows about a loaded image beyond its pixels: its file (for
// material maps keyed by name) and its NewDark material file (.mtl).
//
// A material file has the image's name with ".mtl", in the same relative path
// (material-format.txt in NewDark's documentation). The loader reads it with
// the image and notes the parameters the engine acts on; the renderers look
// them up by the bitmap. Plain data and a spin lock (shared by libraries
// built with different structure packing).

#ifndef __RESIMGINFO_H
#define __RESIMGINFO_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct sResImageInfo
{
   char  name[160];        // the image's path from its resource root: "fam/mech/dal01"
   int   hasMaterial;      // a .mtl was found
   float tileU, tileV;     // UV scale from tile_factor (1: none)
   int   aniRate;          // ms per frame, 0: not given
   int   aniFlags;         // 1 wrap, 2 reverse, 4 pingpong (as Texture Anim Data), -1 not given
   int   forceAniSettings;
   float illumStrength;    // illum_map: how strongly it glows, 0 none
   char  illumMap[160];    // its glow map, resource path (no extension)
   int   forceOpaque;
   int   terrainScaleW, terrainScaleH;   // 0: not given
};

namespace resimginfo
{
   enum { kSlots = 1 << 16 };
   struct Slot { const void *bitmap; sResImageInfo *info; };
   inline Slot *Table() { static Slot table[kSlots]; return table; }
   inline std::atomic_flag &Lock() { static std::atomic_flag lock = ATOMIC_FLAG_INIT; return lock; }
   inline const void *Tomb() { static const char tomb = 0; return &tomb; }
   inline size_t Hash(const void *p)
   {
      uintptr_t v = reinterpret_cast<uintptr_t>(p);
      v ^= v >> 15; v *= 0x2c1b3c6du; v ^= v >> 12;
      return size_t(v) & (kSlots - 1);
   }
   struct Guard
   {
      Guard() { while (Lock().test_and_set(std::memory_order_acquire)) {} }
      ~Guard() { Lock().clear(std::memory_order_release); }
   };
}

// takes ownership of info (malloc'd)
inline void ResImageInfoRegister(const void *bitmap, sResImageInfo *info)
{
   using namespace resimginfo;
   if (!bitmap || !info)
      return;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap), tomb = size_t(-1);
   for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1))
   {
      if (t[i].bitmap == bitmap) { std::free(t[i].info); t[i].info = info; return; }
      if (t[i].bitmap == Tomb() && tomb == size_t(-1)) tomb = i;
      if (!t[i].bitmap)
      {
         const size_t at = tomb != size_t(-1) ? tomb : i;
         t[at].bitmap = bitmap; t[at].info = info;
         return;
      }
   }
   if (tomb != size_t(-1)) { t[tomb].bitmap = bitmap; t[tomb].info = info; }
   else std::free(info);
}

inline void ResImageInfoForget(const void *bitmap)
{
   using namespace resimginfo;
   if (!bitmap)
      return;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap);
   for (size_t n = 0; n < kSlots && t[i].bitmap; ++n, i = (i + 1) & (kSlots - 1))
      if (t[i].bitmap == bitmap)
      {
         std::free(t[i].info);
         t[i].bitmap = Tomb(); t[i].info = nullptr;
         return;
      }
}

// copies the image's record into *out; false if there is none
inline bool ResImageInfoFind(const void *bitmap, sResImageInfo *out)
{
   using namespace resimginfo;
   if (!bitmap)
      return false;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap);
   for (size_t n = 0; n < kSlots && t[i].bitmap; ++n, i = (i + 1) & (kSlots - 1))
      if (t[i].bitmap == bitmap)
      {
         if (out && t[i].info)
            *out = *t[i].info;
         return t[i].info != nullptr;
      }
   return false;
}

#endif
