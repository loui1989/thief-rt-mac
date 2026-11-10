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
   m_live(0)
{
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
         m_blocks[address + capacity] = rest;
         m_free.insert(std::make_pair(rest.capacity, address + capacity));
         block.capacity = capacity;
      }
      block.free = false;
      block.size = size;
      ++g_Osm32HeapBlocks;
      ++m_live;
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
   const uint32_t address = m_next;
   m_blocks[address] = block;
   m_next += capacity;
   ++g_Osm32HeapBlocks;
   ++m_live;
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
