// Host/guest ABI helpers shared by the embedded OSM runtime.

#ifndef __OSM32BRIDGE_H
#define __OSM32BRIDGE_H

#include <osm_emu/osm32cpu.h>

#pragma pack(push, 8)
#include <stdint.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#pragma pack(pop)

namespace osm32
{

bool ReadStackArgument(const sCpuState &state, const cMemory &memory,
                       unsigned index, uint32_t *value, std::string *error);
bool ReturnStdcall(sCpuState *state, cMemory *memory, unsigned argumentBytes,
                   uint32_t result, std::string *error);

class cGuestHeap
{
public:
   cGuestHeap();

   bool Initialize(cMemory *memory, uint32_t base, uint32_t size,
                   std::string *error);
   ~cGuestHeap();
   uint32_t Alloc(uint32_t size, std::string *error);
   uint32_t Realloc(uint32_t address, uint32_t size, std::string *error);
   bool Free(uint32_t address, std::string *error);
   uint32_t GetSize(uint32_t address) const;
   int DidAlloc(uint32_t address) const;

private:
   // Blocks by address (each a run of the heap, used or free, touching the
   // next), and the free ones by size: found, split and merged in log time.
   // (A list scanned from the start, its free blocks never split or merged,
   // grew all session with the script libraries' many small allocations --
   // NVScript, tnhScript -- and every allocation and free scanned it: the
   // game slowed as it was played.)
   struct sBlock
   {
      uint32_t size;
      uint32_t capacity;
      bool free;
      uint32_t serial;   // allocation order
   };

   sBlock *Find(uint32_t address);
   const sBlock *Find(uint32_t address) const;
   void MarkFree(uint32_t address);
   void Census();

   cMemory *m_memory;
   uint32_t m_base;
   uint64_t m_end;
   uint32_t m_next;
   std::map<uint32_t, sBlock> m_blocks;
   std::set<std::pair<uint32_t, uint32_t> > m_free;   // (capacity, address)
   long m_live;   // allocations in use
   uint32_t m_serial;   // allocations made
   long m_censusAt;     // allocations in use when the log next gets a census
};

} // namespace osm32

#endif // __OSM32BRIDGE_H
