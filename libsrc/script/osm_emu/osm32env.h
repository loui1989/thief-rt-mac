// Default portable host environment for legacy 32-bit OSM modules.

#ifndef __OSM32ENV_H
#define __OSM32ENV_H

#include <osm_emu/osm32rt.h>

#pragma pack(push, 8)
#include <stdint.h>
#include <string>
#include <vector>
#pragma pack(pop)

namespace osm32
{

// Implements the platform APIs needed by the VC6 runtime embedded in the
// original OSMs. Engine-facing script and service calls remain explicit
// virtual hooks, so no host pointer or platform ABI leaks into guest memory.
class cEnvironment : public iEnvironment
{
public:
   cEnvironment();
   virtual ~cEnvironment();

   virtual bool Prepare(cImage *image, cMemory *memory,
                        std::string *error);
   virtual uint32_t ScriptManagerAddress() const;
   virtual uint32_t PrintAddress() const;
   virtual uint32_t AllocatorAddress() const;
   virtual uint32_t AllocGuest(uint32_t size, std::string *error);
   virtual bool FreeGuest(uint32_t address, std::string *error);

   virtual bool Handles(uint32_t address) const;
   virtual bool Invoke(uint32_t address, sCpuState *state, cMemory *memory,
                       std::string *error);

   // Engine interfaces a guest asks the script manager for (QueryInterface:
   // in the game it is part of the application's aggregate), handed over as
   // proxies like the services': their id kEngineInterface | the LG
   // interface's number, or a proxy of an object one of them returned (an
   // object query), its native object kept with it.
   enum { kEngineInterface = 0x8000, kEngineObject = 0xc000 };

protected:
   bool GetScriptServiceId(uint32_t object, uint16_t *id) const;
   void *GetProxyNative(uint32_t object) const;
   uint32_t CreateInterfaceProxy(uint16_t id, void *native, std::string *error);
   bool QueryAggregate(sCpuState *state, cMemory *memory, std::string *error);
   // the engine interfaces there are (by their LG number), and the native
   // object of a proxy let go of by the guest
   virtual bool EngineInterfaceAvailable(uint32_t /*lgNumber*/) { return false; }
   virtual void ReleaseProxyNative(uint16_t /*id*/, void * /*native*/) {}
   virtual bool InvokeScriptManager(unsigned slot, sCpuState *state,
                                    cMemory *memory, std::string *error);
   virtual bool InvokeScriptService(unsigned slot, sCpuState *state,
                                    cMemory *memory, std::string *error);
   virtual void DebugOutput(const std::string &text);

private:
   struct sServiceProxy
   {
      uint16_t id;
      uint32_t object;
      uint32_t vtable;
      uint32_t references;
      void *native;
   };

   struct sInitializerContext
   {
      uint32_t current;
      uint32_t end;
      bool stopOnError;
   };

   struct sAtom
   {
      std::string name;
      uint32_t references;
   };

   bool InvokeAllocator(unsigned slot, sCpuState *state, cMemory *memory,
                        std::string *error);
   bool BeginInitializers(bool stopOnError, sCpuState *state,
                          cMemory *memory, std::string *error);
   bool ContinueInitializers(sCpuState *state, cMemory *memory,
                             std::string *error);
   bool InvokeImport(const sImport &item, sCpuState *state, cMemory *memory,
                     std::string *error);

   const std::vector<sImport> *m_imports;
   cMemory *m_memory;
   cGuestHeap m_heap;
   uint32_t m_moduleHandle;
   uint32_t m_errno;        // the guest's errno (_errno)
   uint32_t m_strtokNext;   // where strtok goes on
   uint32_t m_lastError;
   std::vector<bool> m_tlsUsed;
   std::vector<uint32_t> m_tlsValues;
   std::vector<sAtom> m_atoms;
   std::vector<sServiceProxy> m_services;
   std::vector<sInitializerContext> m_initializers;
};

} // namespace osm32

#endif // __OSM32ENV_H
