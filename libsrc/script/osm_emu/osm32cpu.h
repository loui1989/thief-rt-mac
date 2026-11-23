// Portable IA-32 interpreter for legacy OSM modules.

#ifndef __OSM32CPU_H
#define __OSM32CPU_H

#pragma pack(push, 8)
#include <stdint.h>
#include <string>
#include <vector>
#pragma pack(pop)

namespace osm32
{

enum eRegister
{
   kEax,
   kEcx,
   kEdx,
   kEbx,
   kEsp,
   kEbp,
   kEsi,
   kEdi,
   kRegisterCount
};

enum eMemoryAccess
{
   kMemRead = 1,
   kMemWrite = 2,
   kMemExecute = 4
};

struct sCpuState
{
   uint32_t reg[kRegisterCount];
   uint32_t eip;
   uint32_t eflags;
   double x87[8];
   unsigned x87Depth;
   uint16_t x87Status;
   uint16_t x87Control;
   // SSE/SSE2 (a newer Visual C++'s code uses them whatever CPUID says):
   // the eight XMM registers, little-endian bytes, and MXCSR
   uint8_t xmm[8][16];
   uint32_t mxcsr;

   sCpuState();
};

extern uint64_t g_osm32Instructions;
void DumpGuestProfile();

class cMemory
{
public:
   bool Map(uint32_t base, uint32_t size, unsigned access, std::string *error);
   bool Load(uint32_t address, const void *source, size_t size, std::string *error);

   bool Read8(uint32_t address, uint8_t *value, std::string *error) const;
   bool Read16(uint32_t address, uint16_t *value, std::string *error) const;
   bool Read32(uint32_t address, uint32_t *value, std::string *error) const;
   bool Read64(uint32_t address, uint64_t *value, std::string *error) const;
   bool Write8(uint32_t address, uint8_t value, std::string *error);
   bool Write16(uint32_t address, uint16_t value, std::string *error);
   bool Write32(uint32_t address, uint32_t value, std::string *error);
   bool Write64(uint32_t address, uint64_t value, std::string *error);
   bool Fetch8(uint32_t address, uint8_t *value, std::string *error) const;
   // the bytes at an address, size of them in one region (to patch), or null
   uint8_t *Bytes(uint32_t address, size_t size);
   // the executable region an address is in: its bytes, its first address and
   // its end; good until Generation() changes (a region mapped)
   const uint8_t *Code(uint32_t address, uint32_t *base, uint64_t *end) const;
   unsigned Generation() const { return m_generation; }

private:
   struct sRegion
   {
      uint32_t base;
      uint64_t end;
      unsigned access;
      std::vector<uint8_t> bytes;
   };

   const sRegion *Find(uint32_t address, size_t size, unsigned access) const;
   sRegion *Find(uint32_t address, size_t size, unsigned access);
   std::vector<sRegion> m_regions;
   // the region last found, for each kind of access (its index: the regions
   // move as more are mapped): every byte the guest fetched or touched looked
   // through all of them
   mutable size_t m_last[8] = {0, 0, 0, 0, 0, 0, 0, 0};
   unsigned m_generation = 1;
};

class iHostCalls
{
public:
   virtual ~iHostCalls() {}
   virtual bool Handles(uint32_t address) const = 0;
   virtual bool Invoke(uint32_t address, sCpuState *state, cMemory *memory,
                       std::string *error) = 0;
};

class cCpu
{
public:
   enum eRunResult
   {
      kRunStopped,
      kRunFault,
      kRunLimit
   };

   cCpu();

   cMemory &Memory() { return m_memory; }
   const cMemory &Memory() const { return m_memory; }
   sCpuState &State() { return m_state; }
   const sCpuState &State() const { return m_state; }
   void SetHostCalls(iHostCalls *host) { m_host = host; }

   bool Push32(uint32_t value, std::string *error);
   bool Pop32(uint32_t *value, std::string *error);
   bool Step(std::string *error);
   eRunResult Run(uint32_t stopAddress, uint64_t instructionLimit,
                  std::string *error);
   // the C runtime's functions the guest spends its time in, recognised in
   // its code (base, size) and run natively from then on: how many
   unsigned InstallNativeHooks(uint32_t base, uint32_t size);

private:
   bool RunNative(unsigned kind, std::string *error);
   // the code being run (Fetch8 without looking for its region each byte)
   const uint8_t *m_code = 0;
   uint32_t m_codeBase = 0;
   uint64_t m_codeEnd = 0;
   unsigned m_codeGeneration = 0;
   std::vector<unsigned> m_natives;   // each hook's kind, by its trap's number
   struct sDecodedRM
   {
      bool isRegister;
      unsigned reg;
      uint32_t address;
   };

   bool Fetch8(uint8_t *value, std::string *error);
   bool Fetch16(uint16_t *value, std::string *error);
   bool Fetch32(uint32_t *value, std::string *error);
   bool DecodeRM(uint8_t modrm, sDecodedRM *operand, unsigned *regField,
                 std::string *error);
   bool ReadRM32(const sDecodedRM &operand, uint32_t *value,
                 std::string *error) const;
   bool WriteRM32(const sDecodedRM &operand, uint32_t value,
                  std::string *error);
   bool ReadRM16(const sDecodedRM &operand, uint16_t *value,
                 std::string *error) const;
   bool WriteRM16(const sDecodedRM &operand, uint16_t value,
                  std::string *error);
   bool ReadRM8(const sDecodedRM &operand, uint8_t *value,
                std::string *error) const;
   bool WriteRM8(const sDecodedRM &operand, uint8_t value,
                 std::string *error);
   uint8_t ReadReg8(unsigned reg) const;
   void WriteReg8(unsigned reg, uint8_t value);

   void SetLogicFlags(uint32_t result);
   void SetLogicFlags16(uint16_t result);
   void SetLogicFlags8(uint8_t result);
   void SetAddFlags(uint32_t left, uint32_t right, uint32_t result);
   void SetAdcFlags(uint32_t left, uint32_t right, bool carry,
                    uint32_t result);
   void SetAddFlags16(uint16_t left, uint16_t right, uint16_t result);
   void SetAddFlags8(uint8_t left, uint8_t right, uint8_t result);
   void SetAdcFlags8(uint8_t left, uint8_t right, bool carry,
                     uint8_t result);
   void SetSubFlags(uint32_t left, uint32_t right, uint32_t result);
   void SetSbbFlags(uint32_t left, uint32_t right, bool borrow,
                    uint32_t result);
   void SetSubFlags16(uint16_t left, uint16_t right, uint16_t result);
   void SetSubFlags8(uint8_t left, uint8_t right, uint8_t result);
   void SetSbbFlags8(uint8_t left, uint8_t right, bool borrow,
                     uint8_t result);
   void SetIncFlags(uint32_t before, uint32_t result);
   void SetDecFlags(uint32_t before, uint32_t result);
   bool Condition(unsigned condition) const;
   // an 0F instruction it knows (SSE/SSE2, CMOVcc, SHLD/SHRD, multi-byte
   // NOP): done, *handled set; else nothing fetched (osm32cpu.cpp)
   bool ExecuteSse(unsigned prefix, uint32_t instructionAddress, bool *handled,
                   std::string *error);
   // the x87 forms the main decoder lacks (transcendentals, FCOMI, FCMOV,
   // FISTTP, register FST/FSTP/FUCOM, m80, FNSTENV/FLDENV, FNSAVE/FRSTOR):
   // done, *handled set; else nothing fetched past the D8-DF byte
   bool ExecuteX87(uint8_t opcode, uint32_t instructionAddress, bool *handled,
                   std::string *error);

   cMemory m_memory;
   sCpuState m_state;
   iHostCalls *m_host;
};

} // namespace osm32

#endif // __OSM32CPU_H
