// Default portable host environment for legacy 32-bit OSM modules.

#include <osm_emu/osm32env.h>
#include <osm_emu/osm32imports.h>

#pragma pack(push, 8)
#include <chrono>
#include <ctype.h>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma pack(pop)

namespace
{

const uint32_t kImportTrap = 0xf0000000;
const uint32_t kScriptManTrap = 0xf1000000;
const uint32_t kAllocatorTrap = 0xf1001000;
const uint32_t kServiceTrap = 0xf1002000;
const uint32_t kPrintTrap = 0xf2000000;
const uint32_t kInitializerTrap = 0xf2000010;
const uint32_t kTrapStride = 16;
const uint32_t kProxyBase = 0x60000000;
const uint32_t kScriptMan = kProxyBase + 0x100;
const uint32_t kScriptManVtable = kProxyBase + 0x200;
const uint32_t kAllocator = kProxyBase + 0x300;
const uint32_t kAllocatorVtable = kProxyBase + 0x400;
const uint32_t kCommandLine = kProxyBase + 0x800;
const uint32_t kEnvironment = kProxyBase + 0x840;
const uint32_t kImportDataBase = kProxyBase + 0x900;
const uint32_t kThreadEnvironmentBlock = kProxyBase + 0xa00;
const uint32_t kHeapBase = 0x40000000;
const uint32_t kHeapSize = 0x02000000;
const uint32_t kGuestHeapHandle = kProxyBase + 0xb00;

void Write32(uint8_t *target, uint32_t value)
{
   target[0] = static_cast<uint8_t>(value);
   target[1] = static_cast<uint8_t>(value >> 8);
   target[2] = static_cast<uint8_t>(value >> 16);
   target[3] = static_cast<uint8_t>(value >> 24);
}

bool Fail(std::string *error, const std::string &message)
{
   if (error)
      *error = message;
   return false;
}

bool ReadCString(const osm32::cMemory &memory, uint32_t address,
                 std::string *value, std::string *error)
{
   value->clear();
   if (!address)
      return Fail(error, "null guest string");
   for (uint32_t i = 0; i < 0x00100000; ++i)
   {
      uint8_t character;
      if (!memory.Read8(address + i, &character, error))
         return false;
      if (!character)
         return true;
      value->push_back(static_cast<char>(character));
   }
   return Fail(error, "unterminated guest string");
}

// printf's formatting for a guest: its format, its arguments read from guest
// memory one after another from argCursor (the stack, or a va_list)
bool FormatGuest(const osm32::cMemory &memory, const std::string &format,
                 uint32_t argCursor, std::string *out, std::string *error)
{
   out->clear();
   auto next32 = [&](uint32_t *v) {
      const bool ok = memory.Read32(argCursor, v, error);
      argCursor += 4;
      return ok;
   };
   for (size_t i = 0; i < format.size(); ++i)
   {
      if (format[i] != '%')
      {
         out->push_back(format[i]);
         continue;
      }
      std::string spec = "%";
      ++i;
      while (i < format.size() && strchr("-+ #0", format[i])) spec += format[i++];
      if (i < format.size() && format[i] == '*')
      {
         uint32_t w;
         if (!next32(&w)) return false;
         spec += std::to_string(static_cast<int32_t>(w));
         ++i;
      }
      while (i < format.size() && isdigit(static_cast<unsigned char>(format[i]))) spec += format[i++];
      if (i < format.size() && format[i] == '.')
      {
         spec += format[i++];
         if (i < format.size() && format[i] == '*')
         {
            uint32_t p;
            if (!next32(&p)) return false;
            spec += std::to_string(static_cast<int32_t>(p));
            ++i;
         }
         while (i < format.size() && isdigit(static_cast<unsigned char>(format[i]))) spec += format[i++];
      }
      bool wide64 = false;
      while (i < format.size() && strchr("hlLIwjzt", format[i]))
      {
         if (format.compare(i, 3, "I64") == 0) { wide64 = true; i += 3; continue; }
         if (format.compare(i, 2, "ll") == 0) { wide64 = true; i += 2; continue; }
         ++i;   // (h, l, L, w...: 32 bits here, as on the guest)
      }
      if (i >= format.size()) break;
      const char conv = format[i];
      char buffer[512];
      buffer[0] = 0;
      if (conv == '%')
         out->push_back('%');
      else if (strchr("diouxXc", conv))
      {
         uint32_t lo, hi = 0;
         if (!next32(&lo) || (wide64 && !next32(&hi))) return false;
         if (wide64)
         {
            const uint64_t v = (static_cast<uint64_t>(hi) << 32) | lo;
            snprintf(buffer, sizeof(buffer), (spec + "ll" + conv).c_str(),
                     (conv == 'd' || conv == 'i') ? static_cast<long long>(v) : static_cast<long long>(v));
         }
         else if (conv == 'd' || conv == 'i')
            snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), static_cast<int>(static_cast<int32_t>(lo)));
         else
            snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), static_cast<unsigned>(lo));
         *out += buffer;
      }
      else if (strchr("eEfFgGaA", conv))
      {
         uint32_t lo, hi;
         if (!next32(&lo) || !next32(&hi)) return false;
         const uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;
         double v;
         memcpy(&v, &bits, sizeof(v));
         snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), v);
         *out += buffer;
      }
      else if (conv == 's' || conv == 'S')
      {
         uint32_t address;
         if (!next32(&address)) return false;
         std::string text = "(null)";
         if (address && !ReadCString(memory, address, &text, error)) return false;
         snprintf(buffer, sizeof(buffer), (spec + 's').c_str(), text.c_str());
         *out += strlen(buffer) + 1 < sizeof(buffer) ? std::string(buffer) : text;
      }
      else if (conv == 'p')
      {
         uint32_t v;
         if (!next32(&v)) return false;
         snprintf(buffer, sizeof(buffer), "%08X", v);
         *out += buffer;
      }
      else if (conv == 'n')
      {
         uint32_t v;
         if (!next32(&v)) return false;   // (not written)
      }
      else
         out->push_back(conv);
   }
   return true;
}

bool WriteGuestText(osm32::cMemory *memory, uint32_t destination, const std::string &text,
                    uint32_t capacity, std::string *error)
{
   // (capacity counts the terminator; 0xffffffff: as long as it is)
   const uint32_t n = static_cast<uint32_t>(text.size());
   const uint32_t copy = capacity == 0xffffffffu ? n : (n < capacity ? n : capacity);
   for (uint32_t i = 0; i < copy; ++i)
      if (!memory->Write8(destination + i, static_cast<uint8_t>(text[i]), error)) return false;
   if (copy < capacity || capacity == 0xffffffffu)
      return memory->Write8(destination + copy, 0, error);
   return true;
}

bool ReadStackDouble(const osm32::sCpuState &state,
                     const osm32::cMemory &memory, unsigned wordOffset,
                     double *value, std::string *error)
{
   uint64_t bits;
   if (!memory.Read64(state.reg[osm32::kEsp] + 4 + wordOffset * 4,
                      &bits, error))
      return false;
   memcpy(value, &bits, sizeof(*value));
   return true;
}

bool ReturnCrtDouble(osm32::sCpuState *state, osm32::cMemory *memory,
                     double value, std::string *error)
{
   if (state->x87Depth == 8)
      return Fail(error, "guest CRT x87 stack overflow");
   for (unsigned i = state->x87Depth; i != 0; --i)
      state->x87[i] = state->x87[i - 1];
   state->x87[0] = value;
   ++state->x87Depth;
   return osm32::ReturnStdcall(state, memory, 0, state->reg[osm32::kEax],
                              error);
}

uint8_t AsciiLower(uint8_t character)
{
   return character >= 'A' && character <= 'Z' ?
      static_cast<uint8_t>(character + ('a' - 'A')) : character;
}

// the handle KERNEL32 is found by (GetModuleHandleW): never a real module
const uint32_t kKernel32Handle = 0x7c800000u;

bool IsKernelImport(const osm32::sImport &item, const char *name)
{
   return !item.byOrdinal && item.module == "KERNEL32.dll" &&
          item.name == name;
}

bool IsCrtImport(const osm32::sImport &item, const char *name)
{
   if (item.byOrdinal || item.name != name)
      return false;
   std::string module = item.module;
   for (size_t i = 0; i < module.size(); ++i)
      if (module[i] >= 'A' && module[i] <= 'Z')
         module[i] += 'a' - 'A';
   return module == "msvcr90.dll" || module == "msvcrt.dll";
}

bool IsCrtDataImport(const osm32::sImport &item)
{
   return !item.byOrdinal &&
          (item.name == "_encoded_null" || item.name == "_adjust_fdiv" ||
           item.name == "_pctype" || item.name == "__mb_cur_max");
}

bool EqualAsciiNoCase(const std::string &left, const std::string &right)
{
   if (left.size() != right.size()) return false;
   for (size_t i = 0; i < left.size(); ++i)
      if (AsciiLower(static_cast<uint8_t>(left[i])) !=
          AsciiLower(static_cast<uint8_t>(right[i])))
         return false;
   return true;
}

} // namespace

namespace osm32
{

cEnvironment::cEnvironment()
 : m_imports(0),
   m_memory(0),
   m_moduleHandle(0),
   m_errno(0),
   m_strtokNext(0),
   m_lastError(0)
{
}

uint32_t cEnvironment::ScriptManagerAddress() const { return kScriptMan; }
uint32_t cEnvironment::PrintAddress() const { return kPrintTrap; }
uint32_t cEnvironment::AllocatorAddress() const { return kAllocator; }

uint32_t cEnvironment::AllocGuest(uint32_t size, std::string *error)
{
   return m_heap.Alloc(size, error);
}

bool cEnvironment::FreeGuest(uint32_t address, std::string *error)
{
   return m_heap.Free(address, error);
}

bool cEnvironment::Prepare(cImage *image, cMemory *memory,
                           std::string *error)
{
   m_imports = &image->Imports();
   m_moduleHandle = image->LoadedBase();
   m_memory = memory;
   for (size_t i = 0; i < m_imports->size(); ++i)
   {
      if (getenv("DARK_OSM32_TRACE") &&
          !IsSupportedImport((*m_imports)[i]))
         fprintf(stderr, "OSM32 preflight unsupported import %s!%s\n",
                 (*m_imports)[i].module.c_str(),
                 (*m_imports)[i].byOrdinal ? "<ordinal>" :
                                             (*m_imports)[i].name.c_str());
      Write32(image->Data() + (*m_imports)[i].iatRva,
              IsCrtDataImport((*m_imports)[i]) ?
                 kImportDataBase + static_cast<uint32_t>(i) * 4 :
                 kImportTrap + static_cast<uint32_t>(i) * kTrapStride);
   }

   uint8_t proxies[0x1000] = {};
   const char commandLine[] = "darkengine";
   memcpy(proxies + (kCommandLine - kProxyBase), commandLine,
          sizeof(commandLine));
   // The VC9 DLL startup lock reads fs:[0x18] for the TEB and then uses the
   // stack-base field at TEB+4 as a stable per-thread identity.
   Write32(proxies + (kThreadEnvironmentBlock - kProxyBase) + 0x18,
           kThreadEnvironmentBlock);
   Write32(proxies + (kThreadEnvironmentBlock - kProxyBase) + 4,
           0x70100000);
   Write32(proxies + (kScriptMan - kProxyBase), kScriptManVtable);
   Write32(proxies + (kAllocator - kProxyBase), kAllocatorVtable);
   for (unsigned i = 0; i < 64; ++i)
      Write32(proxies + (kScriptManVtable - kProxyBase) + i * 4,
              kScriptManTrap + i * kTrapStride);
   for (unsigned i = 0; i < 16; ++i)
      Write32(proxies + (kAllocatorVtable - kProxyBase) + i * 4,
              kAllocatorTrap + i * kTrapStride);
   m_atoms.clear();
   m_services.clear();
   m_initializers.clear();

   if (!memory->Map(kProxyBase, sizeof(proxies), kMemRead | kMemWrite,
                    error) ||
       !memory->Load(kProxyBase, proxies, sizeof(proxies), error) ||
       !m_heap.Initialize(memory, kHeapBase, kHeapSize, error))
      return false;
   // the old runtime's data a module imports: its character types (as
   // pointed to, by _pctype: from EOF, -1, on), one byte a character
   // (__mb_cur_max); errno's place
   m_errno = m_heap.Alloc(4, error);
   if (!m_errno || !memory->Write32(m_errno, 0, error))
      return false;
   for (size_t i = 0; i < m_imports->size(); ++i)
   {
      const sImport &item = (*m_imports)[i];
      const uint32_t cell = kImportDataBase + static_cast<uint32_t>(i) * 4;
      if (!IsCrtDataImport(item))
         continue;
      if (item.name == "__mb_cur_max" && !memory->Write32(cell, 1, error))
         return false;
      if (item.name == "_pctype")
      {
         const uint32_t table = m_heap.Alloc(257 * 2, error);
         if (!table || !memory->Write16(table, 0, error)) return false;
         for (unsigned ch = 0; ch < 256; ++ch)
         {
            uint16_t type = 0;
            if (ch >= 'A' && ch <= 'Z') type |= 0x101;
            if (ch >= 'a' && ch <= 'z') type |= 0x102;
            if (ch >= '0' && ch <= '9') type |= 0x04;
            if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || (ch >= '0' && ch <= '9')) type |= 0x80;
            if (ch == ' ' || (ch >= 9 && ch <= 13)) type |= 0x08;
            if (ch == ' ') type |= 0x40;
            if (ch < 32 || ch == 127) type |= 0x20;
            if (ch > 32 && ch < 127 && !(type & 0x107)) type |= 0x10;
            if (!memory->Write16(table + 2 + ch * 2, type, error)) return false;
         }
         if (!memory->Write32(cell, table + 2, error)) return false;
      }
   }
   return true;
}

bool cEnvironment::Handles(uint32_t address) const
{
   if (address == kPrintTrap || address == kInitializerTrap)
      return true;
   if (address >= kScriptManTrap && address < kScriptManTrap + 0x3000)
      return (address - kScriptManTrap) % kTrapStride == 0;
   if (!m_imports || address < kImportTrap)
      return false;
   const uint32_t offset = address - kImportTrap;
   return offset % kTrapStride == 0 &&
          offset / kTrapStride < m_imports->size();
}

bool cEnvironment::InvokeAllocator(unsigned slot, sCpuState *state,
                                   cMemory *memory, std::string *error)
{
   uint32_t object, value, value2;
   if (!ReadStackArgument(*state, *memory, 0, &object, error) ||
       object != kAllocator)
      return Fail(error, "invalid guest allocator object");
   switch (slot)
   {
      case 0:
      {
         uint32_t iid, output, iidData1;
         if (!ReadStackArgument(*state, *memory, 1, &iid, error) ||
             !ReadStackArgument(*state, *memory, 2, &output, error) ||
             !memory->Read32(iid, &iidData1, error))
            return false;
         const bool supported = iidData1 == 2;
         if (!memory->Write32(output, supported ? kAllocator : 0, error))
            return false;
         return ReturnStdcall(state, memory, 12,
                              supported ? 0 : 0x80004002u, error);
      }
      case 1: case 2:
         return ReturnStdcall(state, memory, 4, 1, error);
      case 3:
         if (!ReadStackArgument(*state, *memory, 1, &value, error)) return false;
         value2 = m_heap.Alloc(value, error);
         if (getenv("DARK_OSM32_TRACE"))
         {
            uint32_t returnAddress = 0;
            memory->Read32(state->reg[kEsp], &returnAddress, 0);
            fprintf(stderr,
                    "OSM32 allocator Alloc size=%u result=%08x caller=%08x\n",
                    value, value2, returnAddress);
            fflush(stderr);
         }
         return (!value2 && value) ? false :
                ReturnStdcall(state, memory, 8, value2, error);
      case 4:
         if (!ReadStackArgument(*state, *memory, 1, &value, error) ||
             !ReadStackArgument(*state, *memory, 2, &value2, error)) return false;
         value = m_heap.Realloc(value, value2, error);
         return (!value && value2) ? false :
                ReturnStdcall(state, memory, 12, value, error);
      case 5:
         if (!ReadStackArgument(*state, *memory, 1, &value, error))
            return false;
         if (!m_heap.Free(value, error))
         {
            uint32_t returnAddress = 0;
            memory->Read32(state->reg[kEsp], &returnAddress, 0);
            if (error)
            {
               char context[64];
               snprintf(context, sizeof(context), " (caller 0x%08x)",
                        returnAddress);
               *error += context;
            }
            return false;
         }
         return ReturnStdcall(state, memory, 8, 0, error);
      case 6:
         if (!ReadStackArgument(*state, *memory, 1, &value, error)) return false;
         return ReturnStdcall(state, memory, 8, m_heap.GetSize(value), error);
      case 7:
         if (!ReadStackArgument(*state, *memory, 1, &value, error)) return false;
         return ReturnStdcall(state, memory, 8,
                              static_cast<uint32_t>(m_heap.DidAlloc(value)),
                              error);
      case 8:
         return ReturnStdcall(state, memory, 4, 0, error);
   }
   return Fail(error, "unsupported guest allocator method");
}

bool cEnvironment::InvokeScriptManager(unsigned slot, sCpuState *state,
                                       cMemory *memory, std::string *error)
{
   uint32_t object;
   if (!ReadStackArgument(*state, *memory, 0, &object, error) ||
       object != kScriptMan)
      return Fail(error, "invalid guest script manager object");
   if (slot == 12) // IScriptMan::GetService
   {
      uint32_t guidAddress;
      uint16_t serviceId;
      if (!ReadStackArgument(*state, *memory, 1, &guidAddress, error) ||
          !memory->Read16(guidAddress, &serviceId, error))
         return false;

      for (size_t i = 0; i < m_services.size(); ++i)
         if (m_services[i].id == serviceId)
         {
            ++m_services[i].references;
            return ReturnStdcall(state, memory, 8, m_services[i].object,
                                 error);
         }

      const unsigned kServiceSlots = 64;
      sServiceProxy proxy;
      proxy.id = serviceId;
      proxy.native = 0;
      proxy.object = m_heap.Alloc(4, error);
      proxy.vtable = m_heap.Alloc(kServiceSlots * 4, error);
      proxy.references = 1;
      if (!proxy.object || !proxy.vtable ||
          !memory->Write32(proxy.object, proxy.vtable, error))
         return false;
      for (unsigned i = 0; i < kServiceSlots; ++i)
         if (!memory->Write32(proxy.vtable + i * 4,
                              kServiceTrap + i * kTrapStride, error))
            return false;
      m_services.push_back(proxy);
      return ReturnStdcall(state, memory, 8, proxy.object, error);
   }
   if (slot == 0)
      return QueryAggregate(state, memory, error);
   if (slot == 1 || slot == 2)
      return ReturnStdcall(state, memory, 4, 1, error);   // AddRef, Release
   char message[80];
   snprintf(message, sizeof(message),
            "unsupported guest IScriptMan slot %u", slot);
   return Fail(error, message);
}

// QueryInterface on the script manager, or on an engine interface it gave
// (stack: this, the IID, where the interface goes).
bool cEnvironment::QueryAggregate(sCpuState *state, cMemory *memory, std::string *error)
{
   // In the game the script manager is part of the application's
   // aggregate, as each of its interfaces is: asked for any of the
   // engine's interfaces (Telliamed's and NamelessVoice's script
   // libraries ask for the trait, sim and property managers), it hands
   // it over. Here: the script manager itself, or IUnknown; the engine
   // interfaces the host has proxies for (EngineInterfaceAvailable);
   // for the others, none (the guest's libraries go without, or say so).
   uint32_t guidAddress, result;
   uint32_t guid[4];
   if (!ReadStackArgument(*state, *memory, 1, &guidAddress, error) ||
       !ReadStackArgument(*state, *memory, 2, &result, error))
      return false;
   for (unsigned i = 0; i < 4; ++i)
      if (!memory->Read32(guidAddress + i * 4, &guid[i], error))
         return false;
   // (IUnknown: {00000000-0000-0000-C000-000000000046}; an LG one, n:
   // {n...-7a80+n-11cf+n-8348-00aa00a82b51}, IScriptMan's n 0xcf)
   const bool unknown = !guid[0] && !guid[1] && guid[2] == 0xc0u && guid[3] == 0x46000000u;
   const uint32_t n = guid[1] & 0xffffu;
   const bool lg = n >= 0x7a80u && guid[3] == 0x512ba800u;
   const bool scriptMan = lg && n - 0x7a80u == 0xcfu;
   static const bool s_trace = getenv("DARK_OSM32_TRACE") != NULL;
   if (s_trace || (!unknown && !scriptMan && !(lg && EngineInterfaceAvailable(n - 0x7a80u))))
   {
      static unsigned s_told = 0;
      if (s_told++ < 16)
         fprintf(stderr, "OSM32: a script module asked for %s%#x: %s\n",
                 lg ? "LG interface " : "interface ", lg ? n - 0x7a80u : guid[0],
                 unknown || scriptMan ? "itself" : "not here");
   }
   if (unknown || scriptMan)
   {
      if (result && !memory->Write32(result, kScriptMan, error)) return false;
      return ReturnStdcall(state, memory, 12, 0, error);
   }
   if (lg && n - 0x7a80u < 0x4000u && EngineInterfaceAvailable(n - 0x7a80u))
   {
      const uint16_t id = static_cast<uint16_t>(kEngineInterface | (n - 0x7a80u));
      uint32_t object = 0;
      for (size_t i = 0; i < m_services.size() && !object; ++i)
         if (m_services[i].id == id)
         {
            object = m_services[i].object;
            ++m_services[i].references;
         }
      if (!object && !(object = CreateInterfaceProxy(id, 0, error)))
         return false;
      if (result && !memory->Write32(result, object, error)) return false;
      return ReturnStdcall(state, memory, 12, 0, error);
   }
   if (result && !memory->Write32(result, 0, error)) return false;
   return ReturnStdcall(state, memory, 12, 0x80004002u, error);
}

uint32_t cEnvironment::CreateInterfaceProxy(uint16_t id, void *native, std::string *error)
{
const unsigned kSlots = 64;
sServiceProxy proxy;
proxy.id = id;
proxy.native = native;
proxy.references = 1;
proxy.object = m_heap.Alloc(4, error);
proxy.vtable = m_heap.Alloc(kSlots * 4, error);
if (!proxy.object || !proxy.vtable || !m_memory ||
    !m_memory->Write32(proxy.object, proxy.vtable, error))
   return 0;
for (unsigned i = 0; i < kSlots; ++i)
   if (!m_memory->Write32(proxy.vtable + i * 4, kServiceTrap + i * kTrapStride, error))
      return 0;
m_services.push_back(proxy);
return proxy.object;
}

void *cEnvironment::GetProxyNative(uint32_t object) const
{
   for (size_t i = 0; i < m_services.size(); ++i)
      if (m_services[i].object == object)
         return m_services[i].native;
   return 0;
}

bool cEnvironment::GetScriptServiceId(uint32_t object, uint16_t *id) const
{
   for (size_t i = 0; i < m_services.size(); ++i)
      if (m_services[i].object == object)
      {
         if (id)
            *id = m_services[i].id;
         return true;
      }
   return false;
}

bool cEnvironment::InvokeScriptService(unsigned slot, sCpuState *state,
                                       cMemory *memory, std::string *error)
{
   uint32_t object;
   if (!ReadStackArgument(*state, *memory, 0, &object, error))
      return false;
   sServiceProxy *proxy = 0;
   for (size_t i = 0; i < m_services.size(); ++i)
      if (m_services[i].object == object)
      {
         proxy = &m_services[i];
         break;
      }
   if (!proxy)
      return Fail(error, "invalid guest script service object");
   if (slot == 0 && (proxy->id & kEngineInterface) && !(proxy->id & 0x4000))
      return QueryAggregate(state, memory, error);
   if (slot == 0)
   {
      // itself, as IUnknown or as its own interface (or, an engine object,
      // as whatever it is asked to be: an IProperty as IStringProperty);
      // not as another -- NVScript asks the Link Tools service for an
      // interface only another game has, and stops if it is there
      uint32_t guidAddress, output;
      uint32_t guid[4];
      if (!ReadStackArgument(*state, *memory, 1, &guidAddress, error) ||
          !ReadStackArgument(*state, *memory, 2, &output, error))
         return false;
      for (unsigned i = 0; i < 4; ++i)
         if (!memory->Read32(guidAddress + i * 4, &guid[i], error))
            return false;
      const bool unknown = !guid[0] && !guid[1] && guid[2] == 0xc0u && guid[3] == 0x46000000u;
      const uint32_t n = guid[1] & 0xffffu;
      const bool lg = n >= 0x7a80u && guid[3] == 0x512ba800u;
      const bool itself = unknown || (proxy->id & kEngineInterface) ||
                          (lg && n - 0x7a80u == proxy->id);
      if (!itself)
      {
         if (output && !memory->Write32(output, 0, error)) return false;
         return ReturnStdcall(state, memory, 12, 0x80004002u, error);
      }
      if (!memory->Write32(output, proxy->object, error))
         return false;
      ++proxy->references;
      return ReturnStdcall(state, memory, 12, 0, error);
   }
   if (slot == 1)
      return ReturnStdcall(state, memory, 4, ++proxy->references, error);
   if (slot == 2)
   {
      if (proxy->references)
         --proxy->references;
      const uint32_t left = proxy->references;
      if (!left && proxy->native)
      {
         // (an object an engine interface returned: let go of, and its proxy)
         const sServiceProxy gone = *proxy;
         m_services.erase(m_services.begin() + (proxy - &m_services[0]));
         ReleaseProxyNative(gone.id, gone.native);
         m_heap.Free(gone.vtable, 0);
         m_heap.Free(gone.object, 0);
      }
      return ReturnStdcall(state, memory, 4, left, error);
   }
   char message[96];
   snprintf(message, sizeof(message),
            "unsupported guest script service 0x%04x slot %u",
            static_cast<unsigned>(proxy->id), slot);
   return Fail(error, message);
}

void cEnvironment::DebugOutput(const std::string &text)
{
   const char *enabled = getenv("DARK_SCRIPT_DEBUG");
   if (enabled && *enabled)
      fputs(text.c_str(), stderr);
}

bool cEnvironment::Invoke(uint32_t address, sCpuState *state, cMemory *memory,
                          std::string *error)
{
   if (address == kInitializerTrap)
      return ContinueInitializers(state, memory, error);
   if (address == kPrintTrap)
   {
      // the script manager's printf (the engine's monolog): its format and
      // arguments read off the guest's stack (cdecl)
      uint32_t formatAddress;
      std::string format, text;
      if (!ReadStackArgument(*state, *memory, 0, &formatAddress, error) ||
          !ReadCString(*memory, formatAddress, &format, error) ||
          !FormatGuest(*memory, format, state->reg[kEsp] + 8, &text, error))
         return false;
      DebugOutput(text);
      return ReturnStdcall(state, memory, 0, static_cast<uint32_t>(text.size()), error);
   }
   if (address >= kScriptManTrap && address < kAllocatorTrap)
      return InvokeScriptManager((address - kScriptManTrap) / kTrapStride,
                                 state, memory, error);
   if (address >= kAllocatorTrap && address < kServiceTrap)
      return InvokeAllocator((address - kAllocatorTrap) / kTrapStride,
                             state, memory, error);
   if (address >= kServiceTrap && address < kServiceTrap + 0x1000)
      return InvokeScriptService((address - kServiceTrap) / kTrapStride,
                                 state, memory, error);
   if (!m_imports || address < kImportTrap)
      return Fail(error, "invalid guest host trap");
   return InvokeImport((*m_imports)[(address - kImportTrap) / kTrapStride],
                       state, memory, error);
}

bool cEnvironment::BeginInitializers(bool stopOnError, sCpuState *state,
                                     cMemory *memory, std::string *error)
{
   uint32_t begin, end;
   if (!ReadStackArgument(*state, *memory, 0, &begin, error) ||
       !ReadStackArgument(*state, *memory, 1, &end, error))
      return false;
   if (end < begin || ((end - begin) & 3) != 0 || end - begin > 0x00100000)
      return Fail(error, "invalid guest CRT initializer range");
   sInitializerContext context;
   context.current = begin;
   context.end = end;
   context.stopOnError = stopOnError;
   m_initializers.push_back(context);
   return ContinueInitializers(state, memory, error);
}

bool cEnvironment::ContinueInitializers(sCpuState *state, cMemory *memory,
                                        std::string *error)
{
   if (m_initializers.empty())
      return Fail(error, "unexpected guest CRT initializer return");

   sInitializerContext &context = m_initializers.back();
   if (state->eip == kInitializerTrap && context.stopOnError &&
       state->reg[kEax] != 0)
   {
      const uint32_t result = state->reg[kEax];
      m_initializers.pop_back();
      return ReturnStdcall(state, memory, 0, result, error);
   }

   while (context.current < context.end)
   {
      uint32_t function;
      if (!memory->Read32(context.current, &function, error))
         return false;
      context.current += 4;
      if (!function)
         continue;
      if (state->reg[kEsp] < 4)
         return Fail(error, "guest CRT initializer stack underflow");
      state->reg[kEsp] -= 4;
      if (!memory->Write32(state->reg[kEsp], kInitializerTrap, error))
         return false;
      state->eip = function;
      return true;
   }

   m_initializers.pop_back();
   return ReturnStdcall(state, memory, 0, 0, error);
}

bool cEnvironment::InvokeImport(const sImport &item, sCpuState *state,
                                cMemory *memory, std::string *error)
{
   static const bool s_traceImports = getenv("DARK_OSM32_IMPORTS") != NULL;
   if (s_traceImports)
   {
      uint32_t a[3] = {0, 0, 0};
      for (unsigned i = 0; i < 3; ++i)
         ReadStackArgument(*state, *memory, i, &a[i], 0);
      fprintf(stderr, "OSM32 import %s!%s (%08x %08x %08x)\n", item.module.c_str(),
              item.name.c_str(), a[0], a[1], a[2]);
   }
   if (IsCrtImport(item, "toupper") || IsCrtImport(item, "isspace"))
   {
      uint32_t character;
      if (!ReadStackArgument(*state, *memory, 0, &character, error))
         return false;
      uint32_t result = 0;
      if (item.name == "toupper")
         result = character >= 'a' && character <= 'z' ?
            character - ('a' - 'A') : character;
      else
         result = character == ' ' || character == '\t' ||
                  character == '\n' || character == '\r' ||
                  character == '\f' || character == '\v';
      // CRT functions use cdecl; the guest caller removes their arguments.
      return ReturnStdcall(state, memory, 0, result, error);
   }
   if (IsCrtImport(item, "_stricmp") || IsCrtImport(item, "_strnicmp"))
   {
      uint32_t leftAddress, rightAddress, count = 0xffffffffu;
      std::string left, right;
      if (!ReadStackArgument(*state, *memory, 0, &leftAddress, error) ||
          !ReadStackArgument(*state, *memory, 1, &rightAddress, error) ||
          !ReadCString(*memory, leftAddress, &left, error) ||
          !ReadCString(*memory, rightAddress, &right, error) ||
          (item.name == "_strnicmp" &&
           !ReadStackArgument(*state, *memory, 2, &count, error)))
         return false;
      size_t i = 0;
      while (i < count && i < left.size() && i < right.size() &&
             AsciiLower(static_cast<uint8_t>(left[i])) ==
                AsciiLower(static_cast<uint8_t>(right[i])))
         ++i;
      if (i == count)
         return ReturnStdcall(state, memory, 0, 0, error);
      const int result =
         static_cast<int>(AsciiLower(i < left.size() ?
            static_cast<uint8_t>(left[i]) : 0)) -
         static_cast<int>(AsciiLower(i < right.size() ?
            static_cast<uint8_t>(right[i]) : 0));
      return ReturnStdcall(state, memory, 0,
                           static_cast<uint32_t>(result), error);
   }
   if (IsCrtImport(item, "strlen"))
   {
      uint32_t address;
      std::string value;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadCString(*memory, address, &value, error))
         return false;
      return ReturnStdcall(state, memory, 0,
                           static_cast<uint32_t>(value.size()), error);
   }
   if (IsCrtImport(item, "strcmp"))
   {
      uint32_t leftAddress, rightAddress;
      std::string left, right;
      if (!ReadStackArgument(*state, *memory, 0, &leftAddress, error) ||
          !ReadStackArgument(*state, *memory, 1, &rightAddress, error) ||
          !ReadCString(*memory, leftAddress, &left, error) ||
          !ReadCString(*memory, rightAddress, &right, error))
         return false;
      const int result = left.compare(right);
      return ReturnStdcall(state, memory, 0,
                           static_cast<uint32_t>(result), error);
   }
   if (IsCrtImport(item, "strcpy") || IsCrtImport(item, "strcat"))
   {
      uint32_t destination, source;
      std::string prefix, value;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &source, error) ||
          !ReadCString(*memory, source, &value, error) ||
          (item.name == "strcat" &&
           !ReadCString(*memory, destination, &prefix, error)))
         return false;
      const uint32_t offset = static_cast<uint32_t>(prefix.size());
      for (uint32_t i = 0; i <= value.size(); ++i)
         if (!memory->Write8(destination + offset + i,
                             i < value.size() ?
                                static_cast<uint8_t>(value[i]) : 0,
                             error))
            return false;
      return ReturnStdcall(state, memory, 0, destination, error);
   }
   if (IsCrtImport(item, "strchr") || IsCrtImport(item, "strrchr") ||
       IsCrtImport(item, "strstr"))
   {
      uint32_t address, argument;
      std::string value;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadStackArgument(*state, *memory, 1, &argument, error) ||
          !ReadCString(*memory, address, &value, error))
         return false;
      size_t offset = std::string::npos;
      if (item.name == "strstr")
      {
         std::string needle;
         if (!ReadCString(*memory, argument, &needle, error)) return false;
         offset = value.find(needle);
      }
      else if (item.name == "strchr")
         offset = static_cast<char>(argument) == '\0' ? value.size() :
            value.find(static_cast<char>(argument));
      else
         offset = static_cast<char>(argument) == '\0' ? value.size() :
            value.rfind(static_cast<char>(argument));
      return ReturnStdcall(state, memory, 0,
         offset == std::string::npos ? 0 :
            address + static_cast<uint32_t>(offset), error);
   }
   if (IsCrtImport(item, "strcspn") || IsCrtImport(item, "strpbrk") ||
       IsCrtImport(item, "strspn"))
   {
      uint32_t address, setAddress;
      std::string value, set;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadStackArgument(*state, *memory, 1, &setAddress, error) ||
          !ReadCString(*memory, address, &value, error) ||
          !ReadCString(*memory, setAddress, &set, error))
         return false;
      size_t offset;
      if (item.name == "strspn")
         offset = value.find_first_not_of(set);
      else
         offset = value.find_first_of(set);
      if (offset == std::string::npos)
         offset = value.size();
      const uint32_t result = item.name == "strpbrk" ?
         (offset == value.size() ? 0 :
            address + static_cast<uint32_t>(offset)) :
         static_cast<uint32_t>(offset);
      return ReturnStdcall(state, memory, 0, result, error);
   }
   // The old runtime's (msvcrt.dll: tnhScript.osm)
   if (IsCrtImport(item, "sprintf") || IsCrtImport(item, "_snprintf") ||
       IsCrtImport(item, "_vsnprintf") || IsCrtImport(item, "vsprintf"))
   {
      const bool counted = item.name == "_snprintf" || item.name == "_vsnprintf";
      const bool list = item.name == "_vsnprintf" || item.name == "vsprintf";
      uint32_t destination, count = 0xffffffffu, formatAddress, cursor;
      unsigned at = 0;
      std::string format, text;
      if (!ReadStackArgument(*state, *memory, at++, &destination, error) ||
          (counted && !ReadStackArgument(*state, *memory, at++, &count, error)) ||
          !ReadStackArgument(*state, *memory, at++, &formatAddress, error) ||
          !ReadCString(*memory, formatAddress, &format, error))
         return false;
      if (list)
      {
         if (!ReadStackArgument(*state, *memory, at, &cursor, error)) return false;
      }
      else
         cursor = state->reg[kEsp] + 4 + at * 4;
      if (!FormatGuest(*memory, format, cursor, &text, error))
         return false;
      // (_snprintf: no terminator when it fills the buffer, and -1)
      if (counted && text.size() >= count)
      {
         if (!WriteGuestText(memory, destination, text, count, error)) return false;
         return ReturnStdcall(state, memory, 0, text.size() == count ? count : 0xffffffffu, error);
      }
      if (!WriteGuestText(memory, destination, text, 0xffffffffu, error)) return false;
      return ReturnStdcall(state, memory, 0, static_cast<uint32_t>(text.size()), error);
   }
   if (IsCrtImport(item, "tolower"))
   {
      uint32_t ch;
      if (!ReadStackArgument(*state, *memory, 0, &ch, error)) return false;
      return ReturnStdcall(state, memory, 0, (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch, error);
   }
   if (IsCrtImport(item, "_isctype"))
   {
      uint32_t ch, mask;
      if (!ReadStackArgument(*state, *memory, 0, &ch, error) ||
          !ReadStackArgument(*state, *memory, 1, &mask, error)) return false;
      uint32_t type = 0;
      if (ch < 128)
      {
         if (ch >= 'A' && ch <= 'Z') type |= 0x101;
         if (ch >= 'a' && ch <= 'z') type |= 0x102;
         if (ch >= '0' && ch <= '9') type |= 0x84;
         if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) type |= 0x80;
         if (ch == ' ' || (ch >= 9 && ch <= 13)) type |= 0x08;
         if (ch == ' ') type |= 0x40;
         if (ch < 32 || ch == 127) type |= 0x20;
         if (ch > 32 && ch < 127 && !(type & 0x107)) type |= 0x10;
      }
      return ReturnStdcall(state, memory, 0, type & mask, error);
   }
   if (IsCrtImport(item, "_errno"))
      return ReturnStdcall(state, memory, 0, m_errno, error);
   if (IsCrtImport(item, "strtoul"))
   {
      uint32_t address, endAddress, base;
      std::string value;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadStackArgument(*state, *memory, 1, &endAddress, error) ||
          !ReadStackArgument(*state, *memory, 2, &base, error) ||
          !ReadCString(*memory, address, &value, error))
         return false;
      char *end = 0;
      const unsigned long result = strtoul(value.c_str(), &end, static_cast<int>(base));
      if (endAddress && !memory->Write32(endAddress, address + static_cast<uint32_t>(end - value.c_str()), error))
         return false;
      return ReturnStdcall(state, memory, 0, static_cast<uint32_t>(result), error);
   }
   if (IsCrtImport(item, "strtok"))
   {
      // (its place in the string kept here, as the runtime keeps it)
      uint32_t address, delimiters;
      std::string delims;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadStackArgument(*state, *memory, 1, &delimiters, error) ||
          !ReadCString(*memory, delimiters, &delims, error))
         return false;
      uint32_t at = address ? address : m_strtokNext;
      if (!at)
         return ReturnStdcall(state, memory, 0, 0, error);
      uint8_t ch;
      for (;;)
      {
         if (!memory->Read8(at, &ch, error)) return false;
         if (!ch || delims.find(static_cast<char>(ch)) == std::string::npos) break;
         ++at;
      }
      if (!ch)
      {
         m_strtokNext = 0;
         return ReturnStdcall(state, memory, 0, 0, error);
      }
      const uint32_t token = at;
      for (;;)
      {
         if (!memory->Read8(at, &ch, error)) return false;
         if (!ch) { m_strtokNext = 0; break; }
         if (delims.find(static_cast<char>(ch)) != std::string::npos)
         {
            if (!memory->Write8(at, 0, error)) return false;
            m_strtokNext = at + 1;
            break;
         }
         ++at;
      }
      return ReturnStdcall(state, memory, 0, token, error);
   }
   if (IsCrtImport(item, "sscanf"))
   {
      // %d %i %u %x %f %g %e %s %c %n and %[..], widths, %*: the parsing
      // the host's sscanf does, one conversion at a time
      uint32_t source, formatAddress;
      std::string input, format;
      if (!ReadStackArgument(*state, *memory, 0, &source, error) ||
          !ReadStackArgument(*state, *memory, 1, &formatAddress, error) ||
          !ReadCString(*memory, source, &input, error) ||
          !ReadCString(*memory, formatAddress, &format, error))
         return false;
      uint32_t cursor = state->reg[kEsp] + 4 + 2 * 4;
      size_t at = 0;
      int assigned = 0;
      bool failed = false;
      for (size_t i = 0; i < format.size() && !failed; ++i)
      {
         const char f = format[i];
         if (isspace(static_cast<unsigned char>(f)))
         {
            while (at < input.size() && isspace(static_cast<unsigned char>(input[at]))) ++at;
            continue;
         }
         if (f != '%' || (i + 1 < format.size() && format[i + 1] == '%'))
         {
            if (f == '%') ++i;
            if (at < input.size() && input[at] == format[i]) { ++at; continue; }
            break;
         }
         std::string spec = "%";
         ++i;
         const bool skip = i < format.size() && format[i] == '*';
         if (skip) { spec += '*'; ++i; }
         while (i < format.size() && isdigit(static_cast<unsigned char>(format[i]))) spec += format[i++];
         bool isLong = false;
         while (i < format.size() && strchr("hlL", format[i])) { isLong |= format[i] != 'h'; ++i; }
         if (i >= format.size()) break;
         char conv = format[i];
         if (conv == '[')
         {
            spec += '[';
            ++i;
            if (i < format.size() && format[i] == '^') spec += format[i++];
            if (i < format.size() && format[i] == ']') spec += format[i++];
            while (i < format.size() && format[i] != ']') spec += format[i++];
            spec += ']';
         }
         else
            spec += conv;
         spec += "%n";
         const char *rest = input.c_str() + at;
         int consumed = -1;
         uint32_t target = 0;
         if (!skip && conv != 'n')
         {
            if (!memory->Read32(cursor, &target, error)) return false;
            cursor += 4;
         }
         if (conv == 'n')
         {
            if (!memory->Read32(cursor, &target, error)) return false;
            cursor += 4;
            if (!memory->Write32(target, static_cast<uint32_t>(at), error)) return false;
            continue;
         }
         if (strchr("diuxXo", conv))
         {
            long long v = 0;
            std::string sp = spec;
            sp.insert(sp.size() - 3, "ll");
            if (skip) sscanf(rest, sp.c_str(), &consumed);
            else if (sscanf(rest, sp.c_str(), &v, &consumed) < 1) consumed = -1;
            if (consumed < 0) { failed = true; break; }
            if (!skip && !memory->Write32(target, static_cast<uint32_t>(v), error)) return false;
         }
         else if (strchr("eEfgG", conv))
         {
            double v = 0;
            std::string sp = spec;
            sp.insert(sp.size() - 3, "l");
            if (skip) sscanf(rest, sp.c_str(), &consumed);
            else if (sscanf(rest, sp.c_str(), &v, &consumed) < 1) consumed = -1;
            if (consumed < 0) { failed = true; break; }
            if (!skip)
            {
               uint64_t bits;
               if (isLong) memcpy(&bits, &v, 8);
               else { const float fv = static_cast<float>(v); uint32_t b32; memcpy(&b32, &fv, 4); bits = b32; }
               if (!memory->Write32(target, static_cast<uint32_t>(bits), error) ||
                   (isLong && !memory->Write32(target + 4, static_cast<uint32_t>(bits >> 32), error)))
                  return false;
            }
         }
         else
         {
            std::vector<char> v(input.size() + 2, 0);
            if (skip) sscanf(rest, spec.c_str(), &consumed);
            else if (sscanf(rest, spec.c_str(), v.data(), &consumed) < 1) consumed = -1;
            if (consumed < 0) { failed = true; break; }
            if (!skip)
            {
               const size_t n = conv == 'c' ? static_cast<size_t>(consumed) : strlen(v.data());
               for (size_t k = 0; k < n; ++k)
                  if (!memory->Write8(target + static_cast<uint32_t>(k), static_cast<uint8_t>(v[k]), error)) return false;
               if (conv != 'c' && !memory->Write8(target + static_cast<uint32_t>(n), 0, error)) return false;
            }
         }
         at += static_cast<size_t>(consumed);
         if (!skip) ++assigned;
      }
      if (!assigned && failed && at >= input.size())
         return ReturnStdcall(state, memory, 0, 0xffffffffu, error);   // EOF
      return ReturnStdcall(state, memory, 0, static_cast<uint32_t>(assigned), error);
   }
   if (IsCrtImport(item, "div"))
   {
      // (an 8-byte struct, returned in EDX:EAX)
      uint32_t n, d;
      if (!ReadStackArgument(*state, *memory, 0, &n, error) ||
          !ReadStackArgument(*state, *memory, 1, &d, error)) return false;
      const int32_t ni = static_cast<int32_t>(n), di = static_cast<int32_t>(d);
      if (!di) return Fail(error, "guest div by zero");
      state->reg[kEdx] = static_cast<uint32_t>(ni % di);
      return ReturnStdcall(state, memory, 0, static_cast<uint32_t>(ni / di), error);
   }
   if (IsCrtImport(item, "acos"))
   {
      double first;
      if (!ReadStackDouble(*state, *memory, 0, &first, error)) return false;
      return ReturnCrtDouble(state, memory, std::acos(first), error);
   }
   if (IsCrtImport(item, "_write"))
   {
      uint32_t handle, buffer, count;
      if (!ReadStackArgument(*state, *memory, 0, &handle, error) ||
          !ReadStackArgument(*state, *memory, 1, &buffer, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error)) return false;
      std::string text;
      for (uint32_t i = 0; i < count && i < 1024; ++i)
      {
         uint8_t b;
         if (!memory->Read8(buffer + i, &b, error)) return false;
         text += static_cast<char>(b);
      }
      DebugOutput(text);
      return ReturnStdcall(state, memory, 0, count, error);
   }
   if (IsCrtImport(item, "_assert") || IsCrtImport(item, "abort"))
   {
      std::string what = "guest runtime ";
      what += item.name;
      uint32_t text;
      std::string message;
      if (item.name == "_assert" && ReadStackArgument(*state, *memory, 0, &text, 0) &&
          ReadCString(*memory, text, &message, 0))
         what += ": " + message;
      return Fail(error, what.c_str());
   }
   if (IsCrtImport(item, "strtol") || IsCrtImport(item, "strtod"))
   {
      uint32_t address, endAddress;
      std::string value;
      if (!ReadStackArgument(*state, *memory, 0, &address, error) ||
          !ReadStackArgument(*state, *memory, 1, &endAddress, error) ||
          !ReadCString(*memory, address, &value, error))
         return false;
      char *end = 0;
      if (item.name == "strtol")
      {
         uint32_t base;
         if (!ReadStackArgument(*state, *memory, 2, &base, error))
            return false;
         const long result = strtol(value.c_str(), &end,
                                    static_cast<int>(base));
         if (endAddress && !memory->Write32(endAddress, address +
               static_cast<uint32_t>(end - value.c_str()), error))
            return false;
         return ReturnStdcall(state, memory, 0,
                              static_cast<uint32_t>(result), error);
      }
      const double result = strtod(value.c_str(), &end);
      if (endAddress && !memory->Write32(endAddress, address +
            static_cast<uint32_t>(end - value.c_str()), error))
         return false;
      return ReturnCrtDouble(state, memory, result, error);
   }
   if (IsCrtImport(item, "cos") || IsCrtImport(item, "exp") ||
       IsCrtImport(item, "log") || IsCrtImport(item, "log10") ||
       IsCrtImport(item, "sin") || IsCrtImport(item, "sqrt") ||
       IsCrtImport(item, "pow"))
   {
      double first;
      if (!ReadStackDouble(*state, *memory, 0, &first, error)) return false;
      double result;
      if (item.name == "cos") result = std::cos(first);
      else if (item.name == "exp") result = std::exp(first);
      else if (item.name == "log") result = std::log(first);
      else if (item.name == "log10") result = std::log10(first);
      else if (item.name == "sin") result = std::sin(first);
      else if (item.name == "sqrt") result = std::sqrt(first);
      else
      {
         double second;
         if (!ReadStackDouble(*state, *memory, 2, &second, error))
            return false;
         result = std::pow(first, second);
      }
      return ReturnCrtDouble(state, memory, result, error);
   }
   if (IsCrtImport(item, "fflush"))
      return ReturnStdcall(state, memory, 0, 0, error);
   if (IsCrtImport(item, "_itoa"))
   {
      uint32_t rawValue, destination, radix;
      if (!ReadStackArgument(*state, *memory, 0, &rawValue, error) ||
          !ReadStackArgument(*state, *memory, 1, &destination, error) ||
          !ReadStackArgument(*state, *memory, 2, &radix, error))
         return false;
      if (!destination || radix < 2 || radix > 36)
         return Fail(error, "invalid guest _itoa argument");

      char converted[40];
      size_t length = 0;
      uint32_t magnitude = rawValue;
      const bool negative = radix == 10 &&
                            static_cast<int32_t>(rawValue) < 0;
      if (negative)
         magnitude = 0u - rawValue;
      do
      {
         const uint32_t digit = magnitude % radix;
         converted[length++] = static_cast<char>(
            digit < 10 ? '0' + digit : 'a' + digit - 10);
         magnitude /= radix;
      } while (magnitude && length < sizeof(converted) - 1);
      if (negative)
         converted[length++] = '-';
      for (size_t i = 0; i < length; ++i)
         if (!memory->Write8(destination + static_cast<uint32_t>(i),
                             static_cast<uint8_t>(converted[length - i - 1]),
                             error))
            return false;
      if (!memory->Write8(destination + static_cast<uint32_t>(length), 0,
                          error))
         return false;
      return ReturnStdcall(state, memory, 0, destination, error);
   }
   if (IsCrtImport(item, "_CIsqrt") || IsCrtImport(item, "_CIsin") ||
       IsCrtImport(item, "_CIcos"))
   {
      if (!state->x87Depth)
         return Fail(error, "guest CRT intrinsic x87 stack underflow");
      if (item.name == "_CIsqrt")
         state->x87[0] = std::sqrt(state->x87[0]);
      else if (item.name == "_CIsin")
         state->x87[0] = std::sin(state->x87[0]);
      else
         state->x87[0] = std::cos(state->x87[0]);
      return ReturnStdcall(state, memory, 0, state->reg[kEax], error);
   }
   if (IsCrtImport(item, "_initterm_e"))
      return BeginInitializers(true, state, memory, error);
   if (IsCrtImport(item, "_initterm"))
      return BeginInitializers(false, state, memory, error);
   if (IsCrtImport(item, "_encode_pointer") ||
       IsCrtImport(item, "_decode_pointer"))
   {
      uint32_t pointer;
      if (!ReadStackArgument(*state, *memory, 0, &pointer, error))
         return false;
      return ReturnStdcall(state, memory, 0, pointer, error);
   }
   if (IsCrtImport(item, "_lock") || IsCrtImport(item, "_unlock") ||
       IsCrtImport(item, "_crt_debugger_hook"))
      return ReturnStdcall(state, memory, 0, 0, error);
   if (IsCrtImport(item, "__dllonexit") || IsCrtImport(item, "_onexit"))
   {
      uint32_t function;
      if (!ReadStackArgument(*state, *memory, 0, &function, error))
         return false;
      // Destructors are process-lifetime cleanup for these modules.  The
      // guest image is discarded as a unit, so retaining the registration is
      // unnecessary, but registration must report success to the CRT.
      return ReturnStdcall(state, memory, 0, function, error);
   }
   if (IsCrtImport(item, "malloc") || IsCrtImport(item, "_malloc_crt"))
   {
      uint32_t size;
      if (!ReadStackArgument(*state, *memory, 0, &size, error))
         return false;
      const uint32_t allocation = m_heap.Alloc(size, error);
      if (!allocation && size)
         return false;
      return ReturnStdcall(state, memory, 0, allocation, error);
   }
   if (IsCrtImport(item, "realloc"))
   {
      uint32_t allocation, size;
      if (!ReadStackArgument(*state, *memory, 0, &allocation, error) ||
          !ReadStackArgument(*state, *memory, 1, &size, error))
         return false;
      const uint32_t resized = m_heap.Realloc(allocation, size, error);
      if (!resized && size)
         return false;
      return ReturnStdcall(state, memory, 0, resized, error);
   }
   if (IsCrtImport(item, "free"))
   {
      uint32_t allocation;
      if (!ReadStackArgument(*state, *memory, 0, &allocation, error))
         return false;
      if (allocation && !m_heap.Free(allocation, error))
         return false;
      return ReturnStdcall(state, memory, 0, 0, error);
   }
   if (IsCrtImport(item, "memset"))
   {
      uint32_t destination, value, count;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &value, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error))
         return false;
      if (count > 0x02000000)
         return Fail(error, "guest memset is too large");
      for (uint32_t i = 0; i < count; ++i)
         if (!memory->Write8(destination + i, static_cast<uint8_t>(value),
                             error))
            return false;
      return ReturnStdcall(state, memory, 0, destination, error);
   }
   if (IsCrtImport(item, "memcpy") || IsCrtImport(item, "memmove"))
   {
      uint32_t destination, source, count;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &source, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error))
         return false;
      if (count > 0x02000000)
         return Fail(error, "guest memory copy is too large");
      std::vector<uint8_t> bytes(count);
      for (uint32_t i = 0; i < count; ++i)
         if (!memory->Read8(source + i, &bytes[i], error))
            return false;
      for (uint32_t i = 0; i < count; ++i)
         if (!memory->Write8(destination + i, bytes[i], error))
            return false;
      return ReturnStdcall(state, memory, 0, destination, error);
   }
   if (IsCrtImport(item, "memcmp") || IsCrtImport(item, "memchr"))
   {
      uint32_t left, rightOrValue, count;
      if (!ReadStackArgument(*state, *memory, 0, &left, error) ||
          !ReadStackArgument(*state, *memory, 1, &rightOrValue, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error))
         return false;
      for (uint32_t i = 0; i < count; ++i)
      {
         uint8_t a, b;
         if (!memory->Read8(left + i, &a, error)) return false;
         if (item.name == "memchr")
         {
            if (a == static_cast<uint8_t>(rightOrValue))
               return ReturnStdcall(state, memory, 0, left + i, error);
         }
         else
         {
            if (!memory->Read8(rightOrValue + i, &b, error)) return false;
            if (a != b)
               return ReturnStdcall(state, memory, 0,
                  static_cast<uint32_t>(static_cast<int>(a) - b), error);
         }
      }
      return ReturnStdcall(state, memory, 0, 0, error);
   }
   if (IsCrtImport(item, "strncpy"))
   {
      uint32_t destination, source, count;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &source, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error))
         return false;
      bool terminated = false;
      for (uint32_t i = 0; i < count; ++i)
      {
         uint8_t value = 0;
         if (!terminated && !memory->Read8(source + i, &value, error))
            return false;
         if (!value)
            terminated = true;
         if (!memory->Write8(destination + i, value, error))
            return false;
      }
      return ReturnStdcall(state, memory, 0, destination, error);
   }

   // The newer Visual C++ runtime's start-up (NVScript.osm): its security
   // cookie from the time, the process and a counter; its locks with a spin
   // count; its handlers for unhandled exceptions, which this host has none
   // of; the platform's procedures it looks up, found by none here.
   if (IsKernelImport(item, "GetSystemTimeAsFileTime") ||
       IsKernelImport(item, "QueryPerformanceCounter"))
   {
      uint32_t destination;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error))
         return false;
      const uint64_t ticks = static_cast<uint64_t>(
         std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) * 10u +
         116444736000000000ull;
      if (!memory->Write32(destination, static_cast<uint32_t>(ticks), error) ||
          !memory->Write32(destination + 4, static_cast<uint32_t>(ticks >> 32), error))
         return false;
      return ReturnStdcall(state, memory, 4, 1, error);
   }
   if (IsKernelImport(item, "QueryPerformanceFrequency"))
   {
      uint32_t destination;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !memory->Write32(destination, 10000000u, error) ||
          !memory->Write32(destination + 4, 0, error))
         return false;
      return ReturnStdcall(state, memory, 4, 1, error);
   }
   if (IsKernelImport(item, "GetCurrentProcessId"))
      return ReturnStdcall(state, memory, 0, 0x1000, error);
   if (IsKernelImport(item, "GetCurrentProcess"))
      return ReturnStdcall(state, memory, 0, 0xffffffffu, error);
   if (IsKernelImport(item, "InitializeCriticalSectionAndSpinCount"))
      return ReturnStdcall(state, memory, 8, 1, error);
   if (IsKernelImport(item, "SetUnhandledExceptionFilter"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "IsDebuggerPresent"))
      return ReturnStdcall(state, memory, 0, 0, error);
   if (IsKernelImport(item, "IsProcessorFeaturePresent"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "GetModuleHandleW") ||
       IsKernelImport(item, "LoadLibraryA") || IsKernelImport(item, "LoadLibraryW"))
   {
      // KERNEL32 itself is there (the runtime waits for it, then gives up
      // and exits); none other is. Its procedures: none found -- the
      // runtime has its fallbacks (thread storage for fibre storage...)
      uint32_t name;
      if (!ReadStackArgument(*state, *memory, 0, &name, error)) return false;
      std::string text;
      if (name)
      {
         const bool wide = item.name.back() == 'W';
         for (uint32_t i = 0; i < 260; ++i)
         {
            uint32_t ch = 0;
            if (wide)
            {
               uint16_t w;
               if (!memory->Read16(name + i * 2, &w, error)) return false;
               ch = w;
            }
            else
            {
               uint8_t b;
               if (!memory->Read8(name + i, &b, error)) return false;
               ch = b;
            }
            if (!ch) break;
            text += static_cast<char>(ch < 128 ? ch : '?');
         }
      }
      uint32_t handle = name ? 0 : m_moduleHandle;
      if (EqualAsciiNoCase(text, "kernel32.dll") || EqualAsciiNoCase(text, "kernel32"))
         handle = kKernel32Handle;
      return ReturnStdcall(state, memory, 4, handle, error);
   }
   if (IsKernelImport(item, "GetProcAddress"))
      return ReturnStdcall(state, memory, 8, 0, error);
   if (IsKernelImport(item, "Sleep"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "IsValidCodePage"))
      return ReturnStdcall(state, memory, 4, 1, error);
   if (IsKernelImport(item, "GetUserDefaultLCID"))
      return ReturnStdcall(state, memory, 0, 0x0409, error);
   if (IsKernelImport(item, "IsValidLocale"))
      return ReturnStdcall(state, memory, 8, 1, error);
   if (IsKernelImport(item, "VirtualAlloc"))
   {
      // (zeroed, as the system's pages are; never given back)
      uint32_t size;
      if (!ReadStackArgument(*state, *memory, 1, &size, error)) return false;
      const uint32_t allocation = size ? m_heap.Alloc(size, error) : 0;
      if (!allocation && size) return false;
      for (uint32_t i = 0; i < size; ++i)
         if (!memory->Write8(allocation + i, 0, error)) return false;
      return ReturnStdcall(state, memory, 16, allocation, error);
   }
   if (IsKernelImport(item, "VirtualFree"))
      return ReturnStdcall(state, memory, 12, 1, error);
   // Character types and case (the runtime's ctype tables and toupper):
   // ASCII as the system has it, every other character none of them
   if (IsKernelImport(item, "GetStringTypeW") || IsKernelImport(item, "GetStringTypeA") ||
       IsKernelImport(item, "GetStringTypeExA"))
   {
      const bool ansi = item.name != "GetStringTypeW";
      const unsigned first = ansi ? 1 : 0;   // (the A forms: a locale first)
      uint32_t infoType, source, count, destination;
      if (!ReadStackArgument(*state, *memory, first, &infoType, error) ||
          !ReadStackArgument(*state, *memory, first + 1, &source, error) ||
          !ReadStackArgument(*state, *memory, first + 2, &count, error) ||
          !ReadStackArgument(*state, *memory, first + 3, &destination, error))
         return false;
      const uint32_t argBytes = (first + 4) * 4;
      if (infoType != 1)
         return ReturnStdcall(state, memory, argBytes, 0, error);
      for (uint32_t i = 0; count == 0xffffffffu || i < count; ++i)
      {
         uint32_t ch = 0;
         if (ansi)
         {
            uint8_t b;
            if (!memory->Read8(source + i, &b, error)) return false;
            ch = b;
         }
         else
         {
            uint16_t w;
            if (!memory->Read16(source + i * 2, &w, error)) return false;
            ch = w;
         }
         uint16_t type = 0;
         if (ch < 128)
         {
            if (ch >= 'A' && ch <= 'Z') type |= 0x101;
            if (ch >= 'a' && ch <= 'z') type |= 0x102;
            if (ch >= '0' && ch <= '9') type |= 0x84;
            if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) type |= 0x80;
            if (ch == ' ' || (ch >= 9 && ch <= 13)) type |= 0x08;
            if (ch == ' ' || ch == 9) type |= 0x40;
            if (ch < 32 || ch == 127) type |= 0x20;
            if (ch > 32 && ch < 127 && !(type & 0x107)) type |= 0x10;
         }
         if (!memory->Write16(destination + i * 2, type, error)) return false;
         if (count == 0xffffffffu && !ch) break;
      }
      return ReturnStdcall(state, memory, argBytes, 1, error);
   }
   if (IsKernelImport(item, "LCMapStringW") || IsKernelImport(item, "LCMapStringA"))
   {
      const bool wide = item.name == "LCMapStringW";
      const uint32_t unit = wide ? 2 : 1;
      uint32_t flags, source, count, destination, capacity;
      if (!ReadStackArgument(*state, *memory, 1, &flags, error) ||
          !ReadStackArgument(*state, *memory, 2, &source, error) ||
          !ReadStackArgument(*state, *memory, 3, &count, error) ||
          !ReadStackArgument(*state, *memory, 4, &destination, error) ||
          !ReadStackArgument(*state, *memory, 5, &capacity, error))
         return false;
      std::vector<uint32_t> text;
      for (uint32_t i = 0; count == 0xffffffffu || i < count; ++i)
      {
         uint32_t ch = 0;
         if (wide)
         {
            uint16_t w;
            if (!memory->Read16(source + i * 2, &w, error)) return false;
            ch = w;
         }
         else
         {
            uint8_t b;
            if (!memory->Read8(source + i, &b, error)) return false;
            ch = b;
         }
         if (flags & 0x100) ch = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
         if (flags & 0x200) ch = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
         text.push_back(ch);
         if (count == 0xffffffffu && !ch) break;
         if (text.size() > 0x100000) break;
      }
      if (!capacity)
         return ReturnStdcall(state, memory, 24, static_cast<uint32_t>(text.size()), error);
      if (capacity < text.size())
         return ReturnStdcall(state, memory, 24, 0, error);
      for (size_t i = 0; i < text.size(); ++i)
         if (!(wide ? memory->Write16(destination + i * unit, static_cast<uint16_t>(text[i]), error)
                    : memory->Write8(destination + i, static_cast<uint8_t>(text[i]), error)))
            return false;
      return ReturnStdcall(state, memory, 24, static_cast<uint32_t>(text.size()), error);
   }
   if (IsKernelImport(item, "GetLocaleInfoA") || IsKernelImport(item, "GetLocaleInfoW"))
      return ReturnStdcall(state, memory, 16, 0, error);
   if (IsKernelImport(item, "EnumSystemLocalesA") || IsKernelImport(item, "EnumSystemLocalesW"))
      return ReturnStdcall(state, memory, 8, 1, error);
   // Files and the console: none (the runtime's start-up and its messages)
   if (IsKernelImport(item, "GetConsoleCP") || IsKernelImport(item, "GetConsoleOutputCP"))
      return ReturnStdcall(state, memory, 0, 437, error);
   if (IsKernelImport(item, "GetConsoleMode"))
      return ReturnStdcall(state, memory, 8, 0, error);
   if (IsKernelImport(item, "WriteFile") || IsKernelImport(item, "WriteConsoleA") ||
       IsKernelImport(item, "WriteConsoleW"))
   {
      // (taken: all of it said written, the debug text shown)
      uint32_t buffer, count, written;
      if (!ReadStackArgument(*state, *memory, 1, &buffer, error) ||
          !ReadStackArgument(*state, *memory, 2, &count, error) ||
          !ReadStackArgument(*state, *memory, 3, &written, error))
         return false;
      if (written && !memory->Write32(written, count, error)) return false;
      std::string text;
      for (uint32_t i = 0; i < count && i < 512 && item.name != "WriteConsoleW"; ++i)
      {
         uint8_t b;
         if (!memory->Read8(buffer + i, &b, error)) return false;
         text += static_cast<char>(b);
      }
      DebugOutput(text);
      return ReturnStdcall(state, memory, 20, 1, error);
   }
   if (IsKernelImport(item, "ReadFile"))
      return ReturnStdcall(state, memory, 20, 0, error);
   if (IsKernelImport(item, "SetFilePointer"))
      return ReturnStdcall(state, memory, 16, 0xffffffffu, error);
   if (IsKernelImport(item, "CreateFileA") || IsKernelImport(item, "CreateFileW"))
      return ReturnStdcall(state, memory, 28, 0xffffffffu, error);
   if (IsKernelImport(item, "CloseHandle") || IsKernelImport(item, "FlushFileBuffers"))
      return ReturnStdcall(state, memory, 4, 1, error);
   if (IsKernelImport(item, "UnhandledExceptionFilter"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (!item.byOrdinal && item.module == "USER32.dll" && item.name == "GetAsyncKeyState")
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "GetVersion"))
      return ReturnStdcall(state, memory, 0, 5, error);
   if (IsKernelImport(item, "DisableThreadLibraryCalls"))
      return ReturnStdcall(state, memory, 4, 1, error);
   if (IsKernelImport(item, "FindAtomA") ||
       IsKernelImport(item, "AddAtomA"))
   {
      uint32_t nameAddress;
      std::string name;
      if (!ReadStackArgument(*state, *memory, 0, &nameAddress, error))
         return false;
      // MAKEINTATOM values name a predefined atom directly.
      if (nameAddress && nameAddress < 0x10000)
         return ReturnStdcall(state, memory, 4, nameAddress, error);
      if (!ReadCString(*memory, nameAddress, &name, error)) return false;
      for (size_t i = 0; i < m_atoms.size(); ++i)
         if (EqualAsciiNoCase(m_atoms[i].name, name))
         {
            if (item.name == "AddAtomA") ++m_atoms[i].references;
            return ReturnStdcall(state, memory, 4,
               0xc000u + static_cast<uint32_t>(i), error);
         }
      if (item.name == "FindAtomA")
         return ReturnStdcall(state, memory, 4, 0, error);
      if (m_atoms.size() >= 0x4000)
         return ReturnStdcall(state, memory, 4, 0, error);
      sAtom atom;
      atom.name = name;
      atom.references = 1;
      m_atoms.push_back(atom);
      return ReturnStdcall(state, memory, 4,
         0xc000u + static_cast<uint32_t>(m_atoms.size() - 1), error);
   }
   if (IsKernelImport(item, "GetAtomNameA"))
   {
      uint32_t atom, destination, capacity;
      if (!ReadStackArgument(*state, *memory, 0, &atom, error) ||
          !ReadStackArgument(*state, *memory, 1, &destination, error) ||
          !ReadStackArgument(*state, *memory, 2, &capacity, error))
         return false;
      if (atom < 0xc000 || atom - 0xc000 >= m_atoms.size() || !capacity)
         return ReturnStdcall(state, memory, 12, 0, error);
      const std::string &name = m_atoms[atom - 0xc000].name;
      const uint32_t copied = static_cast<uint32_t>(
         name.size() < capacity - 1 ? name.size() : capacity - 1);
      for (uint32_t i = 0; i < copied; ++i)
         if (!memory->Write8(destination + i,
                             static_cast<uint8_t>(name[i]), error))
            return false;
      if (!memory->Write8(destination + copied, 0, error)) return false;
      return ReturnStdcall(state, memory, 12, copied, error);
   }
   if (IsKernelImport(item, "GetModuleHandleA"))
   {
      uint32_t name;
      if (!ReadStackArgument(*state, *memory, 0, &name, error)) return false;
      return ReturnStdcall(state, memory, 4, name ? 0 : m_moduleHandle, error);
   }
   if (IsKernelImport(item, "GetProcessHeap"))
      return ReturnStdcall(state, memory, 0, kGuestHeapHandle, error);
   if (IsKernelImport(item, "GlobalAlloc"))
   {
      uint32_t flags, size;
      if (!ReadStackArgument(*state, *memory, 0, &flags, error) ||
          !ReadStackArgument(*state, *memory, 1, &size, error)) return false;
      const uint32_t allocation = m_heap.Alloc(size, error);
      if (!allocation && size) return false;
      if (flags & 0x40)
         for (uint32_t i = 0; i < size; ++i)
            if (!memory->Write8(allocation + i, 0, error)) return false;
      return ReturnStdcall(state, memory, 8, allocation, error);
   }
   if (IsKernelImport(item, "HeapCreate"))
      return ReturnStdcall(state, memory, 12, kGuestHeapHandle, error);
   if (IsKernelImport(item, "HeapDestroy"))
   {
      uint32_t heap;
      if (!ReadStackArgument(*state, *memory, 0, &heap, error)) return false;
      return ReturnStdcall(state, memory, 4,
                           heap == kGuestHeapHandle ? 1 : 0, error);
   }
   if (IsKernelImport(item, "HeapAlloc"))
   {
      uint32_t heap, flags, size;
      if (!ReadStackArgument(*state, *memory, 0, &heap, error) ||
          !ReadStackArgument(*state, *memory, 1, &flags, error) ||
          !ReadStackArgument(*state, *memory, 2, &size, error)) return false;
      if (heap != kGuestHeapHandle)
         return ReturnStdcall(state, memory, 12, 0, error);
      const uint32_t allocation = m_heap.Alloc(size, error);
      if (!allocation && size) return false;
      if (flags & 8)
         for (uint32_t i = 0; i < size; ++i)
            if (!memory->Write8(allocation + i, 0, error)) return false;
      return ReturnStdcall(state, memory, 12, allocation, error);
   }
   if (IsKernelImport(item, "HeapReAlloc"))
   {
      uint32_t heap, flags, allocation, size;
      if (!ReadStackArgument(*state, *memory, 0, &heap, error) ||
          !ReadStackArgument(*state, *memory, 1, &flags, error) ||
          !ReadStackArgument(*state, *memory, 2, &allocation, error) ||
          !ReadStackArgument(*state, *memory, 3, &size, error)) return false;
      if (heap != kGuestHeapHandle)
         return ReturnStdcall(state, memory, 16, 0, error);
      const uint32_t oldSize = m_heap.GetSize(allocation);
      const uint32_t resized = m_heap.Realloc(allocation, size, error);
      if (!resized && size) return false;
      if ((flags & 8) && size > oldSize)
         for (uint32_t i = oldSize; i < size; ++i)
            if (!memory->Write8(resized + i, 0, error)) return false;
      return ReturnStdcall(state, memory, 16, resized, error);
   }
   if (IsKernelImport(item, "HeapFree"))
   {
      uint32_t heap, allocation;
      if (!ReadStackArgument(*state, *memory, 0, &heap, error) ||
          !ReadStackArgument(*state, *memory, 2, &allocation, error))
         return false;
      const bool freed = heap == kGuestHeapHandle &&
                         (!allocation || m_heap.Free(allocation, error));
      return ReturnStdcall(state, memory, 12, freed ? 1 : 0, error);
   }
   if (IsKernelImport(item, "HeapSize"))
   {
      uint32_t heap, allocation;
      if (!ReadStackArgument(*state, *memory, 0, &heap, error) ||
          !ReadStackArgument(*state, *memory, 2, &allocation, error))
         return false;
      const uint32_t size = heap == kGuestHeapHandle ?
         m_heap.GetSize(allocation) : 0xffffffffu;
      return ReturnStdcall(state, memory, 12,
                           size ? size : 0xffffffffu, error);
   }
   if (IsKernelImport(item, "InitializeCriticalSection") ||
       IsKernelImport(item, "DeleteCriticalSection") ||
       IsKernelImport(item, "EnterCriticalSection") ||
       IsKernelImport(item, "LeaveCriticalSection"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "InterlockedCompareExchange"))
   {
      uint32_t destination, exchange, comparand, previous;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &exchange, error) ||
          !ReadStackArgument(*state, *memory, 2, &comparand, error) ||
          !memory->Read32(destination, &previous, error))
         return false;
      if (previous == comparand &&
          !memory->Write32(destination, exchange, error))
         return false;
      return ReturnStdcall(state, memory, 12, previous, error);
   }
   if (IsKernelImport(item, "InterlockedExchange"))
   {
      uint32_t destination, exchange, previous;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !ReadStackArgument(*state, *memory, 1, &exchange, error) ||
          !memory->Read32(destination, &previous, error) ||
          !memory->Write32(destination, exchange, error))
         return false;
      return ReturnStdcall(state, memory, 8, previous, error);
   }
   if (IsKernelImport(item, "InterlockedIncrement") ||
       IsKernelImport(item, "InterlockedDecrement"))
   {
      uint32_t destination, previous;
      if (!ReadStackArgument(*state, *memory, 0, &destination, error) ||
          !memory->Read32(destination, &previous, error))
         return false;
      const uint32_t result = item.name == "InterlockedIncrement" ?
         previous + 1 : previous - 1;
      if (!memory->Write32(destination, result, error)) return false;
      return ReturnStdcall(state, memory, 4, result, error);
   }
   if (IsKernelImport(item, "TlsAlloc"))
   {
      for (size_t i = 0; i < m_tlsUsed.size(); ++i)
         if (!m_tlsUsed[i])
         {
            m_tlsUsed[i] = true;
            m_tlsValues[i] = 0;
            return ReturnStdcall(state, memory, 0,
                                 static_cast<uint32_t>(i), error);
         }
      m_tlsUsed.push_back(true);
      m_tlsValues.push_back(0);
      return ReturnStdcall(state, memory, 0,
         static_cast<uint32_t>(m_tlsUsed.size() - 1), error);
   }
   if (IsKernelImport(item, "TlsSetValue") ||
       IsKernelImport(item, "TlsGetValue") ||
       IsKernelImport(item, "TlsFree"))
   {
      uint32_t index, value = 0;
      if (!ReadStackArgument(*state, *memory, 0, &index, error)) return false;
      const bool valid = index < m_tlsUsed.size() && m_tlsUsed[index];
      if (item.name == "TlsSetValue")
      {
         if (!ReadStackArgument(*state, *memory, 1, &value, error)) return false;
         if (valid) m_tlsValues[index] = value;
         return ReturnStdcall(state, memory, 8, valid ? 1 : 0, error);
      }
      if (item.name == "TlsGetValue")
         return ReturnStdcall(state, memory, 4,
                              valid ? m_tlsValues[index] : 0, error);
      if (valid)
      {
         m_tlsUsed[index] = false;
         m_tlsValues[index] = 0;
      }
      return ReturnStdcall(state, memory, 4, valid ? 1 : 0, error);
   }
   if (IsKernelImport(item, "GetCurrentThreadId"))
      return ReturnStdcall(state, memory, 0, 1, error);
   if (IsKernelImport(item, "GetCurrentThread"))
      return ReturnStdcall(state, memory, 0, 0xfffffffeu, error);
   if (IsKernelImport(item, "GetThreadPriority"))
      return ReturnStdcall(state, memory, 4, 0, error);
   if (IsKernelImport(item, "SetThreadPriority"))
      return ReturnStdcall(state, memory, 8, 1, error);
   if (IsKernelImport(item, "GetTickCount"))
   {
      static const std::chrono::steady_clock::time_point start =
         std::chrono::steady_clock::now();
      const uint64_t milliseconds = static_cast<uint64_t>(
         std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
      return ReturnStdcall(state, memory, 0,
                           static_cast<uint32_t>(milliseconds), error);
   }
   if (IsKernelImport(item, "GetCommandLineA"))
      return ReturnStdcall(state, memory, 0, kCommandLine, error);
   if (IsKernelImport(item, "GetEnvironmentStrings") ||
       IsKernelImport(item, "GetEnvironmentStringsA") ||
       IsKernelImport(item, "GetEnvironmentStringsW"))
      return ReturnStdcall(state, memory, 0, kEnvironment, error);
   if (IsKernelImport(item, "FreeEnvironmentStringsA") ||
       IsKernelImport(item, "FreeEnvironmentStringsW"))
      return ReturnStdcall(state, memory, 4, 1, error);

   if (IsKernelImport(item, "WideCharToMultiByte"))
   {
      uint32_t source, sourceCount, destination, destinationCount, used;
      if (!ReadStackArgument(*state, *memory, 2, &source, error) ||
          !ReadStackArgument(*state, *memory, 3, &sourceCount, error) ||
          !ReadStackArgument(*state, *memory, 4, &destination, error) ||
          !ReadStackArgument(*state, *memory, 5, &destinationCount, error) ||
          !ReadStackArgument(*state, *memory, 7, &used, error)) return false;
      uint32_t count = sourceCount;
      if (count == 0xffffffffu)
      {
         count = 0;
         uint16_t value;
         do
         {
            if (count == 0x00100000u ||
                !memory->Read16(source + count * 2, &value, error)) return false;
            ++count;
         } while (value);
      }
      if (destination && destinationCount >= count)
      {
         for (uint32_t i = 0; i < count; ++i)
         {
            uint16_t value;
            if (!memory->Read16(source + i * 2, &value, error) ||
                !memory->Write8(destination + i,
                   value <= 0x7f ? static_cast<uint8_t>(value) : '?', error))
               return false;
         }
         if (used && !memory->Write32(used, 0, error)) return false;
      }
      return ReturnStdcall(state, memory, 32,
         !destination || destinationCount >= count ? count : 0, error);
   }
   if (IsKernelImport(item, "MultiByteToWideChar"))
   {
      uint32_t source, sourceCount, destination, destinationCount;
      if (!ReadStackArgument(*state, *memory, 2, &source, error) ||
          !ReadStackArgument(*state, *memory, 3, &sourceCount, error) ||
          !ReadStackArgument(*state, *memory, 4, &destination, error) ||
          !ReadStackArgument(*state, *memory, 5, &destinationCount, error))
         return false;
      uint32_t count = sourceCount;
      if (count == 0xffffffffu)
      {
         count = 0;
         uint8_t value;
         do
         {
            if (count == 0x00100000u ||
                !memory->Read8(source + count, &value, error)) return false;
            ++count;
         } while (value);
      }
      if (destination && destinationCount >= count)
         for (uint32_t i = 0; i < count; ++i)
         {
            uint8_t value;
            if (!memory->Read8(source + i, &value, error) ||
                !memory->Write16(destination + i * 2, value, error)) return false;
         }
      return ReturnStdcall(state, memory, 24,
         !destination || destinationCount >= count ? count : 0, error);
   }
   if (IsKernelImport(item, "GetStartupInfoA"))
   {
      uint32_t output;
      if (!ReadStackArgument(*state, *memory, 0, &output, error)) return false;
      for (uint32_t i = 0; i < 68; ++i)
         if (!memory->Write8(output + i, 0, error)) return false;
      if (!memory->Write32(output, 68, error)) return false;
      return ReturnStdcall(state, memory, 4, 0, error);
   }
   if (IsKernelImport(item, "GetStdHandle"))
   {
      uint32_t which;
      if (!ReadStackArgument(*state, *memory, 0, &which, error)) return false;
      return ReturnStdcall(state, memory, 4, 0x100u + (0u - which), error);
   }
   if (IsKernelImport(item, "GetFileType"))
      return ReturnStdcall(state, memory, 4, 2, error);
   if (IsKernelImport(item, "SetHandleCount"))
   {
      uint32_t count;
      if (!ReadStackArgument(*state, *memory, 0, &count, error)) return false;
      return ReturnStdcall(state, memory, 4, count, error);
   }
   if (IsKernelImport(item, "SetStdHandle"))
      return ReturnStdcall(state, memory, 8, 1, error);
   if (IsKernelImport(item, "GetACP"))
      return ReturnStdcall(state, memory, 0, 1252, error);
   if (IsKernelImport(item, "GetOEMCP"))
      return ReturnStdcall(state, memory, 0, 437, error);
   if (IsKernelImport(item, "GetCPInfo"))
   {
      uint32_t output;
      if (!ReadStackArgument(*state, *memory, 1, &output, error)) return false;
      for (uint32_t i = 0; i < 20; ++i)
         if (!memory->Write8(output + i, 0, error)) return false;
      if (!memory->Write32(output, 1, error) ||
          !memory->Write8(output + 4, '?', error)) return false;
      return ReturnStdcall(state, memory, 8, 1, error);
   }
   if (IsKernelImport(item, "GetModuleFileNameA"))
   {
      static const char path[] = "guest.osm";
      uint32_t output, capacity;
      if (!ReadStackArgument(*state, *memory, 1, &output, error) ||
          !ReadStackArgument(*state, *memory, 2, &capacity, error)) return false;
      const uint32_t length = static_cast<uint32_t>(sizeof(path) - 1);
      const uint32_t written = capacity <= length ? capacity : length;
      for (uint32_t i = 0; i < written; ++i)
         if (!memory->Write8(output + i, path[i], error)) return false;
      if (capacity > length && !memory->Write8(output + length, 0, error))
         return false;
      return ReturnStdcall(state, memory, 12,
                           capacity <= length ? capacity : length, error);
   }
   if (IsKernelImport(item, "GetFileAttributesA"))
   {
      m_lastError = 2;
      return ReturnStdcall(state, memory, 4, 0xffffffffu, error);
   }
   if (IsKernelImport(item, "GetLastError"))
      return ReturnStdcall(state, memory, 0, m_lastError, error);
   if (IsKernelImport(item, "SetLastError"))
   {
      if (!ReadStackArgument(*state, *memory, 0, &m_lastError, error))
         return false;
      return ReturnStdcall(state, memory, 4, 0, error);
   }
   if (IsKernelImport(item, "GetPrivateProfileIntA"))
   {
      uint32_t fallback;
      if (!ReadStackArgument(*state, *memory, 2, &fallback, error)) return false;
      return ReturnStdcall(state, memory, 16, fallback, error);
   }
   if (IsKernelImport(item, "GetPrivateProfileStringA"))
   {
      uint32_t fallback, output, capacity;
      if (!ReadStackArgument(*state, *memory, 2, &fallback, error) ||
          !ReadStackArgument(*state, *memory, 3, &output, error) ||
          !ReadStackArgument(*state, *memory, 4, &capacity, error)) return false;
      uint32_t copied = 0;
      if (capacity)
      {
         while (copied + 1 < capacity)
         {
            uint8_t value = 0;
            if (fallback && !memory->Read8(fallback + copied, &value, error))
               return false;
            if (!memory->Write8(output + copied, value, error)) return false;
            if (!value) break;
            ++copied;
         }
         if (!memory->Write8(output + copied, 0, error)) return false;
      }
      return ReturnStdcall(state, memory, 24, copied, error);
   }
   if (IsKernelImport(item, "OutputDebugStringA"))
   {
      uint32_t textAddress;
      if (!ReadStackArgument(*state, *memory, 0, &textAddress, error))
         return false;
      std::string text;
      for (unsigned i = 0; i < 4096; ++i)
      {
         uint8_t ch;
         if (!memory->Read8(textAddress + i, &ch, error)) return false;
         if (!ch) break;
         text.push_back(static_cast<char>(ch));
      }
      DebugOutput(text);
      return ReturnStdcall(state, memory, 4, 0, error);
   }

   std::string name = item.module + "!";
   if (item.byOrdinal)
   {
      char ordinal[24];
      snprintf(ordinal, sizeof(ordinal), "#%u",
               static_cast<unsigned>(item.ordinal));
      name += ordinal;
   }
   else
      name += item.name;
   return Fail(error, "unsupported guest import " + name);
}

} // namespace osm32
