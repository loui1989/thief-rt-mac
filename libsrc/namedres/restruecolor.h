// Full-colour copies of true-colour images.
//
// The image loaders (PNG, TGA, DDS) give the engine 16-bit bitmaps: RGB565,
// or ARGB4444 when the image has alpha -- four bits a channel. That is what
// the old blitters and Direct3D 6 took, and it bands every high-resolution
// texture (a fan mission's own, an HD texture pack's). The loaders keep the
// image's original 8-bit RGBA in the same allocation, after the 16-bit pixels,
// and note it here; the hardware renderers (lgd3d's modern path: OpenGL, the
// ray tracer) take their textures from it. The legacy 16-bit pixels stay as
// they were for everything else.
//
// Plain arrays and a spin lock (no library containers): the table is shared
// by libraries built with different structure packing.

#ifndef __RESTRUECOLOR_H
#define __RESTRUECOLOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace restruecolor
{
   enum { kSlots = 1 << 16 };
   struct Slot { const void *bitmap; const unsigned char *rgba; };
   inline Slot *Table() { static Slot table[kSlots]; return table; }
   inline std::atomic_flag &Lock() { static std::atomic_flag lock = ATOMIC_FLAG_INIT; return lock; }
   // a deleted slot: searches go past it
   inline const void *Tomb() { static const char tomb = 0; return &tomb; }
   inline size_t Hash(const void *p)
   {
      uintptr_t v = reinterpret_cast<uintptr_t>(p);
      v ^= v >> 17; v *= 0xed5ad4bbu; v ^= v >> 11;
      return size_t(v) & (kSlots - 1);
   }
   struct Guard
   {
      Guard() { while (Lock().test_and_set(std::memory_order_acquire)) {} }
      ~Guard() { Lock().clear(std::memory_order_release); }
   };
}

inline void ResTrueColorRegister(const void *bitmap, const unsigned char *rgba)
{
   using namespace restruecolor;
   if (!bitmap || !rgba)
      return;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap), tomb = size_t(-1);
   for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1))
   {
      if (t[i].bitmap == bitmap) { t[i].rgba = rgba; return; }
      if (t[i].bitmap == Tomb() && tomb == size_t(-1)) tomb = i;
      if (!t[i].bitmap)
      {
         const size_t at = tomb != size_t(-1) ? tomb : i;
         t[at].bitmap = bitmap;
         t[at].rgba = rgba;
         return;
      }
   }
   if (tomb != size_t(-1)) { t[tomb].bitmap = bitmap; t[tomb].rgba = rgba; }
}

inline void ResTrueColorForget(const void *bitmap)
{
   using namespace restruecolor;
   if (!bitmap)
      return;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap);
   for (size_t n = 0; n < kSlots && t[i].bitmap; ++n, i = (i + 1) & (kSlots - 1))
      if (t[i].bitmap == bitmap) { t[i].bitmap = Tomb(); t[i].rgba = nullptr; return; }
}

// the image's RGBA (width x height, rows packed), or null
inline const unsigned char *ResTrueColorFind(const void *bitmap)
{
   using namespace restruecolor;
   if (!bitmap)
      return nullptr;
   Guard guard;
   Slot *t = Table();
   size_t i = Hash(bitmap);
   for (size_t n = 0; n < kSlots && t[i].bitmap; ++n, i = (i + 1) & (kSlots - 1))
      if (t[i].bitmap == bitmap)
         return t[i].rgba;
   return nullptr;
}

#endif
