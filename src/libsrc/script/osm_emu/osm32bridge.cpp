// Host/guest ABI helpers shared by the embedded OSM runtime.

#include <osm_emu/osm32bridge.h>

#pragma pack(push, 8)
#include <algorithm>
#include <stdio.h>
#include <vector>

// (live guest allocations, across every script module: for perf.log)
extern "C" { long g_Osm32HeapBlocks = 0; }
#pragma pack(pop)

namespace
{

bool Fail(std::string *error, const char *message)
{
   if (error)
      *error = message;
   return false;
}

uint32_t Align16(uint32_t size)
{
   if (!size)
      size = 1;
   return (size + 15u) & ~15u;
}

} // namespace

namespace osm32
{

bool ReadStackArgument(const sCpuState &state, const cMemory &memory,
                       unsigned index, uint32_t *value, std::string *error)
{
   const uint64_t address = static_cast<uint64_t>(state.reg[kEsp]) + 4ull +
                            static_cast<uint64_t>(index) * 4ull;
   if (address > 0xffffffffull)
      return Fail(error, "guest stack argument address overflow");
   return memory.Read32(static_cast<uint32_t>(address), value, error);
}

bool ReturnStdcall(sCpuState *state, cMemory *memory, unsigned argumentBytes,
                   uint32_t result, std::string *error)
{
   uint32_t returnAddress;
   if (!memory->Read32(state->reg[kEsp], &returnAddress, error))
      return false;
   const uint64_t newStack = static_cast<uint64_t>(state->reg[kEsp]) + 4ull +
                             argumentBytes;
   if (newStack > 0xffffffffull)
      return Fail(error, "guest stdcall stack overflow");
   state->reg[kEax] = result;
   state->reg[kEsp] = static_cast<uint32_t>(newStack);
   state->eip = returnAddress;
   return true;
}

cGuestHeap::cGuestHeap()
 : m_memory(0),
   m_base(0),
   m_end(0),
   m_next(0),
   m_live(0),
   m_serial(0),
   m_censusAt(15000)
{
}

// What the guest's newest allocations still held are (their sizes, and what
// a few hold), once as many are held as the census mark: the log can then
// say what grows (Malazar's Tower's scripts held 47 more a second, for good)
void cGuestHeap::Census()
{
   const uint32_t recent = m_serial > 4000 ? m_serial - 4000 : 0;
   std::map<uint32_t, int> sizes;
   std::vector<uint32_t> samples;
   int count = 0;
   for (std::map<uint32_t, sBlock>::const_iterator it = m_blocks.begin(); it != m_blocks.end(); ++it)
      if (!it->second.free && it->second.serial >= recent)
      {
         ++sizes[it->second.size];
         ++count;
         if (samples.size() < 6 && it->second.serial >= recent + 2000)
            samples.push_back(it->first);
      }
   std::vector<std::pair<int, uint32_t> > top;
   for (std::map<uint32_t, int>::const_iterator it = sizes.begin(); it != sizes.end(); ++it)
      top.push_back(std::make_pair(it->second, it->first));
   std::sort(top.rbegin(), top.rend());
   fprintf(stderr, "OSM32 heap: %ld allocations held; of the last 4000 made, %d still held, by size:", m_live, count);
   for (size_t i = 0; i < top.size() && i < 8; ++i)
      fprintf(stderr, " %u bytes x%d", top[i].second, top[i].first);
   fprintf(stderr, "\n");
   for (size_t i = 0; i < samples.size(); ++i)
   {
      const sBlock &b = m_blocks[samples[i]];
      char text[100];
      unsigned n = 0;
      for (uint32_t j = 0; j < b.size && j < 48; ++j)
      {
         uint8_t v = 0;
         m_memory->Read8(samples[i] + j, &v, 0);
         n += snprintf(text + n, sizeof(text) - n, "%c", v >= 32 && v < 127 ? v : '.');
      }
      uint32_t first = 0, second = 0;
      m_memory->Read32(samples[i], &first, 0);
      m_memory->Read32(samples[i] + 4, &second, 0);
      fprintf(stderr, "OSM32 heap:   0x%08x, %u bytes: \"%s\" (%08x %08x)\n", samples[i], b.size, text, first, second);
   }
}

cGuestHeap::~cGuestHeap()
{
   g_Osm32HeapBlocks -= m_live;
}

bool cGuestHeap::Initialize(cMemory *memory, uint32_t base, uint32_t size,
                            std::string *error)
{
   if (!memory || !size)
      return Fail(error, "invalid guest heap configuration");
   const uint64_t end = static_cast<uint64_t>(base) + size;
   if (end > 0x100000000ull)
      return Fail(error, "guest heap wraps the address space");
   if (!memory->Map(base, size, kMemRead | kMemWrite, error))
      return false;
   m_memory = memory;
   m_base = base;
   m_end = end;
   m_next = base;
   g_Osm32HeapBlocks -= m_live;
   m_live = 0;
   m_blocks.clear();
   m_free.clear();
   return true;
}

cGuestHeap::sBlock *cGuestHeap::Find(uint32_t address)
{
   std::map<uint32_t, sBlock>::iterator it = m_blocks.find(address);
   return it == m_blocks.end() ? 0 : &it->second;
}

const cGuestHeap::sBlock *cGuestHeap::Find(uint32_t address) const
{
   std::map<uint32_t, sBlock>::const_iterator it = m_blocks.find(address);
   return it == m_blocks.end() ? 0 : &it->second;
}

namespace
{
void EraseFree(std::set<std::pair<uint32_t, uint32_t> > &free, uint32_t capacity, uint32_t address)
{
   free.erase(std::make_pair(capacity, address));
}
}

// a block freed: merged with free neighbours, the run at the top given back
void cGuestHeap::MarkFree(uint32_t address)
{
   std::map<uint32_t, sBlock>::iterator it = m_blocks.find(address);
   if (it == m_blocks.end())
      return;
   if (!it->second.free)
   {
      --g_Osm32HeapBlocks;
      --m_live;
   }
   it->second.free = true;
   std::map<uint32_t, sBlock>::iterator next = it;
   ++next;
   if (next != m_blocks.end() && next->second.free && next->first == address + it->second.capacity)
   {
      EraseFree(m_free, next->second.capacity, next->first);
      it->second.capacity += next->second.capacity;
      m_blocks.erase(next);
   }
   if (it != m_blocks.begin())
   {
      std::map<uint32_t, sBlock>::iterator prev = it;
      --prev;
      if (prev->second.free && prev->first + prev->second.capacity == it->first)
      {
         EraseFree(m_free, prev->second.capacity, prev->first);
         prev->second.capacity += it->second.capacity;
         m_blocks.erase(it);
         it = prev;
      }
   }
   if (it->first + it->second.capacity == m_next)
   {
      m_next = it->first;
      m_blocks.erase(it);
      return;
   }
   m_free.insert(std::make_pair(it->second.capacity, it->first));
}

uint32_t cGuestHeap::Alloc(uint32_t size, std::string *error)
{
   if (!m_memory)
   {
      Fail(error, "guest heap is not initialized");
      return 0;
   }
   if (size > 0xfffffff0u)
   {
      Fail(error, "guest allocation is too large");
      return 0;
   }
   const uint32_t capacity = Align16(size ? size : 1);
   // the smallest free block it fits in, split if much larger
   std::set<std::pair<uint32_t, uint32_t> >::iterator fit =
      m_free.lower_bound(std::make_pair(capacity, 0u));
   if (fit != m_free.end())
   {
      const uint32_t address = fit->second;
      m_free.erase(fit);
      sBlock &block = m_blocks[address];
      if (block.capacity >= capacity + 64)
      {
         sBlock rest;
         rest.size = 0;
         rest.capacity = block.capacity - capacity;
         rest.free = true;
         rest.serial = 0;
         m_blocks[address + capacity] = rest;
         m_free.insert(std::make_pair(rest.capacity, address + capacity));
         block.capacity = capacity;
      }
      block.free = false;
      block.size = size;
      block.serial = m_serial++;
      ++g_Osm32HeapBlocks;
      ++m_live;
      if (m_live >= m_censusAt)
      {
         m_censusAt *= 2;
         Census();
      }
      if (error) error->clear();
      return address;
   }

   if (static_cast<uint64_t>(m_next) + capacity > m_end)
   {
      Fail(error, "guest heap exhausted");
      return 0;
   }
   sBlock block;
   block.size = size;
   block.capacity = capacity;
   block.free = false;
   block.serial = m_serial++;
   const uint32_t address = m_next;
   m_blocks[address] = block;
   m_next += capacity;
   ++g_Osm32HeapBlocks;
   ++m_live;
   if (m_live >= m_censusAt)
   {
      m_censusAt *= 2;
      Census();
   }
   if (error) error->clear();
   return address;
}

uint32_t cGuestHeap::Realloc(uint32_t address, uint32_t size,
                             std::string *error)
{
   if (!address)
      return Alloc(size, error);
   if (!size)
   {
      Free(address, error);
      return 0;
   }

   sBlock *old = Find(address);
   if (!old || old->free)
   {
      Fail(error, "guest realloc received an invalid pointer");
      return 0;
   }
   if (size <= old->capacity)
   {
      old->size = size;
      if (error) error->clear();
      return address;
   }

   const uint32_t oldSize = old->size;
   const uint32_t replacement = Alloc(size, error);
   if (!replacement)
      return 0;
   std::vector<uint8_t> copy(oldSize);
   for (uint32_t i = 0; i < oldSize; ++i)
      if (!m_memory->Read8(address + i, &copy[i], error))
         return 0;
   if (!copy.empty() && !m_memory->Load(replacement, &copy[0], copy.size(), error))
      return 0;
   MarkFree(address);
   if (error) error->clear();
   return replacement;
}

bool cGuestHeap::Free(uint32_t address, std::string *error)
{
   if (!address)
      return true;
   sBlock *block = Find(address);
   if (!block || block->free)
   {
      if (error)
      {
         char detail[128];
         snprintf(detail, sizeof(detail),
                  "guest free received invalid pointer 0x%08x", address);
         *error = detail;
      }
      return false;
   }
   MarkFree(address);
   if (error) error->clear();
   return true;
}

uint32_t cGuestHeap::GetSize(uint32_t address) const
{
   const sBlock *block = Find(address);
   return block && !block->free ? block->size : 0xffffffffu;
}

int cGuestHeap::DidAlloc(uint32_t address) const
{
   const sBlock *block = Find(address);
   return block && !block->free ? 1 : 0;
}

} // namespace osm32
