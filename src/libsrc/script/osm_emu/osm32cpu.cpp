// Portable IA-32 interpreter for legacy OSM modules.

#include <osm_emu/osm32cpu.h>

#pragma pack(push, 8)
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string.h>
#pragma pack(pop)

namespace
{

const uint32_t kFlagCarry = 0x00000001;
const uint32_t kFlagParity = 0x00000004;
const uint32_t kFlagAdjust = 0x00000010;
const uint32_t kFlagZero = 0x00000040;
const uint32_t kFlagSign = 0x00000080;
const uint32_t kFlagOverflow = 0x00000800;
const uint32_t kFlagDirection = 0x00000400;
const uint32_t kArithmeticFlags = kFlagCarry | kFlagParity | kFlagAdjust |
                                  kFlagZero | kFlagSign | kFlagOverflow;

bool Fail(std::string *error, const char *message)
{
   if (error)
      *error = message;
   return false;
}

bool FailAddress(std::string *error, const char *message, uint32_t address)
{
   if (error)
   {
      std::ostringstream text;
      text << message << " 0x" << std::hex << address;
      *error = text.str();
   }
   return false;
}

bool FailOpcode(std::string *error, const char *message, uint32_t address,
                uint8_t opcode)
{
   if (error)
   {
      std::ostringstream text;
      text << message << " 0x" << std::hex << static_cast<unsigned>(opcode)
           << " at 0x" << address;
      *error = text.str();
   }
   return false;
}

bool EvenParity(uint8_t value)
{
   value ^= value >> 4;
   value &= 0x0f;
   return ((0x9669u >> value) & 1) != 0;
}

int32_t Sign8(uint8_t value)
{
   return static_cast<int8_t>(value);
}

} // namespace

namespace osm32
{

sCpuState::sCpuState()
 : eip(0),
   eflags(0x00000002),
   x87Depth(0),
   x87Status(0),
   x87Control(0x037f)
{
   memset(reg, 0, sizeof(reg));
   memset(x87, 0, sizeof(x87));
   memset(xmm, 0, sizeof(xmm));
   mxcsr = 0x1f80;
}

const cMemory::sRegion *cMemory::Find(uint32_t address, size_t size,
                                      unsigned access) const
{
   const uint64_t end = static_cast<uint64_t>(address) + size;
   if (end > 0x100000000ull)
      return 0;
   for (size_t i = 0; i < m_regions.size(); ++i)
   {
      const sRegion &region = m_regions[i];
      if (address >= region.base && end <= region.end &&
          (region.access & access) == access)
         return &region;
   }
   return 0;
}

cMemory::sRegion *cMemory::Find(uint32_t address, size_t size, unsigned access)
{
   return const_cast<sRegion *>(
      static_cast<const cMemory *>(this)->Find(address, size, access));
}

bool cMemory::Map(uint32_t base, uint32_t size, unsigned access,
                  std::string *error)
{
   if (!size)
      return Fail(error, "cannot map an empty guest region");
   const uint64_t end = static_cast<uint64_t>(base) + size;
   if (end > 0x100000000ull)
      return Fail(error, "guest region wraps the 32-bit address space");

   for (size_t i = 0; i < m_regions.size(); ++i)
      if (base < m_regions[i].end && end > m_regions[i].base)
         return Fail(error, "guest memory regions overlap");

   sRegion region;
   region.base = base;
   region.end = end;
   region.access = access;
   try
   {
      region.bytes.assign(size, 0);
      m_regions.push_back(region);
   }
   catch (...)
   {
      return Fail(error, "cannot allocate guest memory");
   }

   std::sort(m_regions.begin(), m_regions.end(),
      [](const sRegion &left, const sRegion &right) {
         return left.base < right.base;
      });
   if (error)
      error->clear();
   return true;
}

bool cMemory::Load(uint32_t address, const void *source, size_t size,
                   std::string *error)
{
   sRegion *region = Find(address, size, 0);
   if (!region)
      return FailAddress(error, "guest load targets unmapped memory at", address);
   memcpy(&region->bytes[address - region->base], source, size);
   return true;
}

bool cMemory::Read8(uint32_t address, uint8_t *value, std::string *error) const
{
   const sRegion *region = Find(address, 1, kMemRead);
   if (!region)
      return FailAddress(error, "guest read fault at", address);
   *value = region->bytes[address - region->base];
   return true;
}

bool cMemory::Read16(uint32_t address, uint16_t *value, std::string *error) const
{
   const sRegion *region = Find(address, 2, kMemRead);
   if (!region)
      return FailAddress(error, "guest read fault at", address);
   const uint8_t *p = &region->bytes[address - region->base];
   *value = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
   return true;
}

bool cMemory::Read32(uint32_t address, uint32_t *value, std::string *error) const
{
   const sRegion *region = Find(address, 4, kMemRead);
   if (!region)
      return FailAddress(error, "guest read fault at", address);
   const uint8_t *p = &region->bytes[address - region->base];
   *value = static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
   return true;
}

bool cMemory::Read64(uint32_t address, uint64_t *value, std::string *error) const
{
   const sRegion *region = Find(address, 8, kMemRead);
   if (!region)
      return FailAddress(error, "guest read fault at", address);
   const uint8_t *p = &region->bytes[address - region->base];
   *value = static_cast<uint64_t>(p[0]) |
            (static_cast<uint64_t>(p[1]) << 8) |
            (static_cast<uint64_t>(p[2]) << 16) |
            (static_cast<uint64_t>(p[3]) << 24) |
            (static_cast<uint64_t>(p[4]) << 32) |
            (static_cast<uint64_t>(p[5]) << 40) |
            (static_cast<uint64_t>(p[6]) << 48) |
            (static_cast<uint64_t>(p[7]) << 56);
   return true;
}

bool cMemory::Write8(uint32_t address, uint8_t value, std::string *error)
{
   sRegion *region = Find(address, 1, kMemWrite);
   if (!region)
      return FailAddress(error, "guest write fault at", address);
   region->bytes[address - region->base] = value;
   return true;
}

bool cMemory::Write16(uint32_t address, uint16_t value, std::string *error)
{
   sRegion *region = Find(address, 2, kMemWrite);
   if (!region)
      return FailAddress(error, "guest write fault at", address);
   uint8_t *p = &region->bytes[address - region->base];
   p[0] = static_cast<uint8_t>(value);
   p[1] = static_cast<uint8_t>(value >> 8);
   return true;
}

bool cMemory::Write32(uint32_t address, uint32_t value, std::string *error)
{
   sRegion *region = Find(address, 4, kMemWrite);
   if (!region)
      return FailAddress(error, "guest write fault at", address);
   uint8_t *p = &region->bytes[address - region->base];
   p[0] = static_cast<uint8_t>(value);
   p[1] = static_cast<uint8_t>(value >> 8);
   p[2] = static_cast<uint8_t>(value >> 16);
   p[3] = static_cast<uint8_t>(value >> 24);
   return true;
}

bool cMemory::Write64(uint32_t address, uint64_t value, std::string *error)
{
   sRegion *region = Find(address, 8, kMemWrite);
   if (!region)
      return FailAddress(error, "guest write fault at", address);
   uint8_t *p = &region->bytes[address - region->base];
   for (unsigned i = 0; i < 8; ++i)
      p[i] = static_cast<uint8_t>(value >> (i * 8));
   return true;
}

bool cMemory::Fetch8(uint32_t address, uint8_t *value, std::string *error) const
{
   const sRegion *region = Find(address, 1, kMemExecute);
   if (!region)
      return FailAddress(error, "guest execute fault at", address);
   *value = region->bytes[address - region->base];
   return true;
}

cCpu::cCpu()
 : m_host(0)
{
}

bool cCpu::Push32(uint32_t value, std::string *error)
{
   m_state.reg[kEsp] -= 4;
   if (!m_memory.Write32(m_state.reg[kEsp], value, error))
   {
      m_state.reg[kEsp] += 4;
      return false;
   }
   return true;
}

bool cCpu::Pop32(uint32_t *value, std::string *error)
{
   uint32_t popped;
   if (!m_memory.Read32(m_state.reg[kEsp], &popped, error))
      return false;
   m_state.reg[kEsp] += 4;
   *value = popped;
   return true;
}

bool cCpu::Fetch8(uint8_t *value, std::string *error)
{
   if (!m_memory.Fetch8(m_state.eip, value, error))
      return false;
   ++m_state.eip;
   return true;
}

bool cCpu::Fetch16(uint16_t *value, std::string *error)
{
   uint8_t low, high;
   if (!Fetch8(&low, error) || !Fetch8(&high, error))
      return false;
   *value = static_cast<uint16_t>(low | (static_cast<uint16_t>(high) << 8));
   return true;
}

bool cCpu::Fetch32(uint32_t *value, std::string *error)
{
   uint16_t low, high;
   if (!Fetch16(&low, error) || !Fetch16(&high, error))
      return false;
   *value = static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16);
   return true;
}

bool cCpu::DecodeRM(uint8_t modrm, sDecodedRM *operand, unsigned *regField,
                    std::string *error)
{
   const unsigned mod = modrm >> 6;
   const unsigned rm = modrm & 7;
   *regField = (modrm >> 3) & 7;
   if (mod == 3)
   {
      operand->isRegister = true;
      operand->reg = rm;
      operand->address = 0;
      return true;
   }

   operand->isRegister = false;
   operand->reg = 0;
   uint32_t address = 0;
   bool needsDisp32 = false;
   if (rm == 4)
   {
      uint8_t sib;
      if (!Fetch8(&sib, error))
         return false;
      const unsigned scale = sib >> 6;
      const unsigned index = (sib >> 3) & 7;
      const unsigned base = sib & 7;
      if (index != 4)
         address += m_state.reg[index] << scale;
      if (base == 5 && mod == 0)
         needsDisp32 = true;
      else
         address += m_state.reg[base];
   }
   else if (rm == 5 && mod == 0)
      needsDisp32 = true;
   else
      address = m_state.reg[rm];

   if (mod == 1)
   {
      uint8_t displacement;
      if (!Fetch8(&displacement, error))
         return false;
      address += Sign8(displacement);
   }
   else if (mod == 2 || needsDisp32)
   {
      uint32_t displacement;
      if (!Fetch32(&displacement, error))
         return false;
      address += displacement;
   }
   operand->address = address;
   return true;
}

bool cCpu::ReadRM32(const sDecodedRM &operand, uint32_t *value,
                    std::string *error) const
{
   if (operand.isRegister)
   {
      *value = m_state.reg[operand.reg];
      return true;
   }
   return m_memory.Read32(operand.address, value, error);
}

bool cCpu::WriteRM32(const sDecodedRM &operand, uint32_t value,
                     std::string *error)
{
   if (operand.isRegister)
   {
      m_state.reg[operand.reg] = value;
      return true;
   }
   return m_memory.Write32(operand.address, value, error);
}

bool cCpu::ReadRM16(const sDecodedRM &operand, uint16_t *value,
                    std::string *error) const
{
   if (operand.isRegister)
   {
      *value = static_cast<uint16_t>(m_state.reg[operand.reg]);
      return true;
   }
   return m_memory.Read16(operand.address, value, error);
}

bool cCpu::WriteRM16(const sDecodedRM &operand, uint16_t value,
                     std::string *error)
{
   if (operand.isRegister)
   {
      m_state.reg[operand.reg] = (m_state.reg[operand.reg] & 0xffff0000u) |
                                 value;
      return true;
   }
   return m_memory.Write16(operand.address, value, error);
}

bool cCpu::ReadRM8(const sDecodedRM &operand, uint8_t *value,
                   std::string *error) const
{
   if (operand.isRegister)
   {
      *value = ReadReg8(operand.reg);
      return true;
   }
   return m_memory.Read8(operand.address, value, error);
}

bool cCpu::WriteRM8(const sDecodedRM &operand, uint8_t value,
                    std::string *error)
{
   if (operand.isRegister)
   {
      WriteReg8(operand.reg, value);
      return true;
   }
   return m_memory.Write8(operand.address, value, error);
}

uint8_t cCpu::ReadReg8(unsigned reg) const
{
   return reg < 4 ? static_cast<uint8_t>(m_state.reg[reg])
                  : static_cast<uint8_t>(m_state.reg[reg - 4] >> 8);
}

void cCpu::WriteReg8(unsigned reg, uint8_t value)
{
   if (reg < 4)
      m_state.reg[reg] = (m_state.reg[reg] & 0xffffff00u) | value;
   else
   {
      const unsigned base = reg - 4;
      m_state.reg[base] = (m_state.reg[base] & 0xffff00ffu) |
                          (static_cast<uint32_t>(value) << 8);
   }
}

void cCpu::SetLogicFlags(uint32_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80000000u)
      m_state.eflags |= kFlagSign;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetLogicFlags16(uint16_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x8000)
      m_state.eflags |= kFlagSign;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetLogicFlags8(uint8_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80)
      m_state.eflags |= kFlagSign;
   if (EvenParity(result))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetAddFlags(uint32_t left, uint32_t right, uint32_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (static_cast<uint64_t>(left) + right > 0xffffffffull)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80000000u)
      m_state.eflags |= kFlagSign;
   if ((~(left ^ right) & (left ^ result) & 0x80000000u) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetAdcFlags(uint32_t left, uint32_t right, bool carry,
                       uint32_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   const uint64_t unsignedResult = static_cast<uint64_t>(left) + right + carry;
   const int64_t signedResult = static_cast<int64_t>(static_cast<int32_t>(left)) +
      static_cast<int32_t>(right) + carry;
   if (unsignedResult > 0xffffffffull)
      m_state.eflags |= kFlagCarry;
   if ((left & 0x0f) + (right & 0x0f) + carry > 0x0f)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80000000u)
      m_state.eflags |= kFlagSign;
   if (signedResult < -0x80000000ll || signedResult > 0x7fffffffll)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetAddFlags16(uint16_t left, uint16_t right, uint16_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (static_cast<unsigned>(left) + right > 0xffff)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x8000)
      m_state.eflags |= kFlagSign;
   if ((~(left ^ right) & (left ^ result) & 0x8000) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetAddFlags8(uint8_t left, uint8_t right, uint8_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (static_cast<unsigned>(left) + right > 0xff)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80)
      m_state.eflags |= kFlagSign;
   if ((~(left ^ right) & (left ^ result) & 0x80) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(result))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetAdcFlags8(uint8_t left, uint8_t right, bool carry,
                        uint8_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   const unsigned unsignedResult =
      static_cast<unsigned>(left) + right + carry;
   const int signedResult = static_cast<int8_t>(left) +
                            static_cast<int8_t>(right) + carry;
   if (unsignedResult > 0xff)
      m_state.eflags |= kFlagCarry;
   if ((left & 0x0f) + (right & 0x0f) + carry > 0x0f)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80)
      m_state.eflags |= kFlagSign;
   if (signedResult < -128 || signedResult > 127)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(result))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetSubFlags(uint32_t left, uint32_t right, uint32_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (left < right)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80000000u)
      m_state.eflags |= kFlagSign;
   if (((left ^ right) & (left ^ result) & 0x80000000u) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetSbbFlags(uint32_t left, uint32_t right, bool borrow,
                       uint32_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   const uint64_t subtrahend = static_cast<uint64_t>(right) + borrow;
   const int64_t signedResult = static_cast<int64_t>(static_cast<int32_t>(left)) -
      static_cast<int32_t>(right) - borrow;
   if (static_cast<uint64_t>(left) < subtrahend)
      m_state.eflags |= kFlagCarry;
   if (static_cast<unsigned>(left & 0x0f) <
       static_cast<unsigned>(right & 0x0f) + borrow)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80000000u)
      m_state.eflags |= kFlagSign;
   if (signedResult < -0x80000000ll || signedResult > 0x7fffffffll)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetSubFlags16(uint16_t left, uint16_t right, uint16_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (left < right)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x8000)
      m_state.eflags |= kFlagSign;
   if (((left ^ right) & (left ^ result) & 0x8000) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(static_cast<uint8_t>(result)))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetSubFlags8(uint8_t left, uint8_t right, uint8_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   if (left < right)
      m_state.eflags |= kFlagCarry;
   if (((left ^ right ^ result) & 0x10) != 0)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80)
      m_state.eflags |= kFlagSign;
   if (((left ^ right) & (left ^ result) & 0x80) != 0)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(result))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetSbbFlags8(uint8_t left, uint8_t right, bool borrow,
                        uint8_t result)
{
   m_state.eflags &= ~kArithmeticFlags;
   const unsigned subtrahend = static_cast<unsigned>(right) + borrow;
   const int signedResult = static_cast<int8_t>(left) -
                            static_cast<int8_t>(right) - borrow;
   if (static_cast<unsigned>(left) < subtrahend)
      m_state.eflags |= kFlagCarry;
   if (static_cast<unsigned>(left & 0x0f) <
       static_cast<unsigned>(right & 0x0f) + borrow)
      m_state.eflags |= kFlagAdjust;
   if (!result)
      m_state.eflags |= kFlagZero;
   if (result & 0x80)
      m_state.eflags |= kFlagSign;
   if (signedResult < -128 || signedResult > 127)
      m_state.eflags |= kFlagOverflow;
   if (EvenParity(result))
      m_state.eflags |= kFlagParity;
}

void cCpu::SetIncFlags(uint32_t before, uint32_t result)
{
   const uint32_t carry = m_state.eflags & kFlagCarry;
   SetAddFlags(before, 1, result);
   m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
}

void cCpu::SetDecFlags(uint32_t before, uint32_t result)
{
   const uint32_t carry = m_state.eflags & kFlagCarry;
   SetSubFlags(before, 1, result);
   m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
}

bool cCpu::Condition(unsigned condition) const
{
   const bool carry = (m_state.eflags & kFlagCarry) != 0;
   const bool parity = (m_state.eflags & kFlagParity) != 0;
   const bool zero = (m_state.eflags & kFlagZero) != 0;
   const bool sign = (m_state.eflags & kFlagSign) != 0;
   const bool overflow = (m_state.eflags & kFlagOverflow) != 0;
   switch (condition & 15)
   {
      case 0: return overflow;
      case 1: return !overflow;
      case 2: return carry;
      case 3: return !carry;
      case 4: return zero;
      case 5: return !zero;
      case 6: return carry || zero;
      case 7: return !carry && !zero;
      case 8: return sign;
      case 9: return !sign;
      case 10: return parity;
      case 11: return !parity;
      case 12: return sign != overflow;
      case 13: return sign == overflow;
      case 14: return zero || sign != overflow;
      case 15: return !zero && sign == overflow;
   }
   return false;
}

bool cCpu::Step(std::string *error)
{
   if (m_host && m_host->Handles(m_state.eip))
      return m_host->Invoke(m_state.eip, &m_state, &m_memory, error);

   const uint32_t instructionAddress = m_state.eip;
   uint8_t opcode;
   if (!Fetch8(&opcode, error))
      return false;

   bool repeat = false;
   bool repeatNotEqual = false;
   bool operand16 = false;
   // (LOCK, 0xf0: one thread here, every access atomic -- GCC's runtime
   // counts references with lock xadd and lock add)
   while (opcode == 0xf2 || opcode == 0xf3 || opcode == 0x66 || opcode == 0xf0)
   {
      if (opcode == 0xf0)
         ;
      else if (opcode == 0xf2 || opcode == 0xf3)
      {
         repeat = true;
         repeatNotEqual = opcode == 0xf2;
      }
      else
         operand16 = true;
      if (!Fetch8(&opcode, error))
         return false;
   }

   if (opcode == 0x0f)
   {
      bool handled = false;
      const unsigned prefix = operand16 ? 0x66u : repeat ? (repeatNotEqual ? 0xf2u : 0xf3u) : 0u;
      if (!ExecuteSse(prefix, instructionAddress, &handled, error))
         return false;
      if (handled)
         return true;
   }
   if (opcode == 0x8c)
   {
      // MOV r/m16, Sreg: the C runtime's crash reporting saves the selectors
      // (flat 32-bit Windows values)
      static const uint16_t kSelectors[8] = { 0x2b, 0x23, 0x2b, 0x2b, 0x53, 0x2b, 0, 0 };
      uint8_t modrm;
      sDecodedRM operand;
      unsigned segment;
      if (!Fetch8(&modrm, error) || !DecodeRM(modrm, &operand, &segment, error))
         return false;
      if (segment > 5)
         return FailAddress(error, "invalid segment register at", instructionAddress);
      if (!operand.isRegister)
         return m_memory.Write16(operand.address, kSelectors[segment], error);
      m_state.reg[operand.reg] = operand16 ?
         (m_state.reg[operand.reg] & 0xffff0000u) | kSelectors[segment] :
         kSelectors[segment];
      return true;
   }
   if (opcode >= 0xd8 && opcode <= 0xdf)
   {
      bool handled = false;
      if (!ExecuteX87(opcode, instructionAddress, &handled, error))
         return false;
      if (handled)
         return true;
   }

   if (opcode == 0xa4 || opcode == 0xa5 ||
       opcode == 0xa6 || opcode == 0xa7 ||
       opcode == 0xaa || opcode == 0xab ||
       opcode == 0xae || opcode == 0xaf)
   {
      const unsigned width = (opcode & 1) ? (operand16 ? 2 : 4) : 1;
      const int32_t delta = (m_state.eflags & kFlagDirection)
         ? -static_cast<int32_t>(width) : static_cast<int32_t>(width);
      uint32_t count = repeat ? m_state.reg[kEcx] : 1;
      while (count)
      {
         --count;
         if (opcode == 0xa4)
         {
            uint8_t value;
            if (!m_memory.Read8(m_state.reg[kEsi], &value, error) ||
                !m_memory.Write8(m_state.reg[kEdi], value, error))
               return false;
         }
         else if (opcode == 0xa5)
         {
            if (operand16)
            {
               uint16_t value;
               if (!m_memory.Read16(m_state.reg[kEsi], &value, error) ||
                   !m_memory.Write16(m_state.reg[kEdi], value, error))
                  return false;
            }
            else
            {
               uint32_t value;
               if (!m_memory.Read32(m_state.reg[kEsi], &value, error) ||
                   !m_memory.Write32(m_state.reg[kEdi], value, error))
                  return false;
            }
         }
         else if (opcode == 0xa6)
         {
            uint8_t left, right;
            if (!m_memory.Read8(m_state.reg[kEsi], &left, error) ||
                !m_memory.Read8(m_state.reg[kEdi], &right, error))
               return false;
            SetSubFlags8(left, right, static_cast<uint8_t>(left - right));
         }
         else if (opcode == 0xa7)
         {
            if (operand16)
            {
               uint16_t left, right;
               if (!m_memory.Read16(m_state.reg[kEsi], &left, error) ||
                   !m_memory.Read16(m_state.reg[kEdi], &right, error))
                  return false;
               SetSubFlags16(left, right,
                             static_cast<uint16_t>(left - right));
            }
            else
            {
               uint32_t left, right;
               if (!m_memory.Read32(m_state.reg[kEsi], &left, error) ||
                   !m_memory.Read32(m_state.reg[kEdi], &right, error))
                  return false;
               SetSubFlags(left, right, left - right);
            }
         }
         else if (opcode == 0xaa)
         {
            if (!m_memory.Write8(m_state.reg[kEdi], ReadReg8(0), error))
               return false;
         }
         else if (operand16)
         {
            if (opcode == 0xaf)
            {
               uint16_t value;
               const uint16_t accumulator =
                  static_cast<uint16_t>(m_state.reg[kEax]);
               if (!m_memory.Read16(m_state.reg[kEdi], &value, error))
                  return false;
               SetSubFlags16(accumulator, value,
                             static_cast<uint16_t>(accumulator - value));
            }
            else if (!m_memory.Write16(m_state.reg[kEdi],
                                       static_cast<uint16_t>(m_state.reg[kEax]),
                                       error))
               return false;
         }
         else if (opcode == 0xae)
         {
            uint8_t value;
            const uint8_t accumulator = ReadReg8(0);
            if (!m_memory.Read8(m_state.reg[kEdi], &value, error))
               return false;
            SetSubFlags8(accumulator, value,
                         static_cast<uint8_t>(accumulator - value));
         }
         else if (opcode == 0xaf)
         {
            uint32_t value;
            const uint32_t accumulator = m_state.reg[kEax];
            if (!m_memory.Read32(m_state.reg[kEdi], &value, error))
               return false;
            SetSubFlags(accumulator, value, accumulator - value);
         }
         else if (!m_memory.Write32(m_state.reg[kEdi], m_state.reg[kEax],
                                    error))
            return false;

         if (opcode >= 0xa4 && opcode <= 0xa7)
            m_state.reg[kEsi] += delta;
         m_state.reg[kEdi] += delta;

         if (repeat && (opcode == 0xa6 || opcode == 0xa7 ||
                        opcode == 0xae || opcode == 0xaf))
         {
            const bool equal = (m_state.eflags & kFlagZero) != 0;
            if ((repeatNotEqual && equal) || (!repeatNotEqual && !equal))
               break;
         }
      }
      if (repeat)
         m_state.reg[kEcx] = count;
      return true;
   }

   if (operand16)
   {
      if (opcode >= 0x40 && opcode <= 0x4f)
      {
         const unsigned reg = opcode & 7;
         const uint16_t before = static_cast<uint16_t>(m_state.reg[reg]);
         const bool decrement = opcode >= 0x48;
         const uint16_t result = static_cast<uint16_t>(
            decrement ? before - 1 : before + 1);
         const uint32_t carry = m_state.eflags & kFlagCarry;
         if (decrement)
            SetSubFlags16(before, 1, result);
         else
            SetAddFlags16(before, 1, result);
         m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
         m_state.reg[reg] = (m_state.reg[reg] & 0xffff0000u) | result;
         return true;
      }
      if (opcode == 0x05 || opcode == 0x0d || opcode == 0x25 ||
          opcode == 0x2d || opcode == 0x35 || opcode == 0x3d)
      {
         uint16_t immediate;
         if (!Fetch16(&immediate, error)) return false;
         const uint16_t left = static_cast<uint16_t>(m_state.reg[kEax]);
         uint16_t result;
         switch (opcode)
         {
            case 0x05: result = left + immediate;
                       SetAddFlags16(left, immediate, result); break;
            case 0x0d: result = left | immediate;
                       SetLogicFlags16(result); break;
            case 0x25: result = left & immediate;
                       SetLogicFlags16(result); break;
            case 0x2d: result = left - immediate;
                       SetSubFlags16(left, immediate, result); break;
            case 0x35: result = left ^ immediate;
                       SetLogicFlags16(result); break;
            default:   result = left - immediate;
                       SetSubFlags16(left, immediate, result); return true;
         }
         m_state.reg[kEax] = (m_state.reg[kEax] & 0xffff0000u) | result;
         return true;
      }
      if (opcode == 0xa1 || opcode == 0xa3)
      {
         uint32_t address;
         if (!Fetch32(&address, error))
            return false;
         if (opcode == 0xa3)
            return m_memory.Write16(address,
               static_cast<uint16_t>(m_state.reg[kEax]), error);
         uint16_t value;
         if (!m_memory.Read16(address, &value, error))
            return false;
         m_state.reg[kEax] = (m_state.reg[kEax] & 0xffff0000u) | value;
         return true;
      }
      if (opcode >= 0xb8 && opcode <= 0xbf)
      {
         uint16_t value;
         if (!Fetch16(&value, error))
            return false;
         const unsigned reg = opcode - 0xb8;
         m_state.reg[reg] = (m_state.reg[reg] & 0xffff0000u) | value;
         return true;
      }
      if (opcode == 0x01 || opcode == 0x03 ||
          opcode == 0x09 || opcode == 0x0b ||
          opcode == 0x21 || opcode == 0x23 ||
          opcode == 0x29 || opcode == 0x2b ||
          opcode == 0x31 || opcode == 0x33 ||
          opcode == 0x39 || opcode == 0x3b)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         uint16_t memoryValue;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error) ||
             !ReadRM16(operand, &memoryValue, error))
            return false;
         const bool registerDestination = (opcode & 2) != 0;
         const uint16_t registerValue =
            static_cast<uint16_t>(m_state.reg[regField]);
         const uint16_t left = registerDestination
            ? registerValue : memoryValue;
         const uint16_t right = registerDestination
            ? memoryValue : registerValue;
         uint16_t result = 0;
         switch (opcode & 0xfc)
         {
            case 0x00: result = left + right; SetAddFlags16(left, right, result); break;
            case 0x08: result = left | right; SetLogicFlags16(result); break;
            case 0x20: result = left & right; SetLogicFlags16(result); break;
            case 0x28: result = left - right; SetSubFlags16(left, right, result); break;
            case 0x30: result = left ^ right; SetLogicFlags16(result); break;
            case 0x38: result = left - right; SetSubFlags16(left, right, result); return true;
         }
         if (registerDestination)
            m_state.reg[regField] = (m_state.reg[regField] & 0xffff0000u) |
                                    result;
         else
            return WriteRM16(operand, result, error);
         return true;
      }
      if (opcode == 0x89 || opcode == 0x8b)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (opcode == 0x89)
            return WriteRM16(operand,
               static_cast<uint16_t>(m_state.reg[regField]), error);
         uint16_t value;
         if (!ReadRM16(operand, &value, error))
            return false;
         m_state.reg[regField] = (m_state.reg[regField] & 0xffff0000u) | value;
         return true;
      }
      if (opcode == 0x85)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         uint16_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error) ||
             !ReadRM16(operand, &value, error))
            return false;
         SetLogicFlags16(value &
                         static_cast<uint16_t>(m_state.reg[regField]));
         return true;
      }
      if (opcode == 0xc7)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint16_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error))
            return false;
         if (operation != 0)
            return Fail(error, "invalid 16-bit MOV immediate encoding");
         if (!Fetch16(&value, error)) return false;
         return WriteRM16(operand, value, error);
      }
      if (opcode == 0xff)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint16_t before;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM16(operand, &before, error))
            return false;
         if (operation != 0 && operation != 1)
            return Fail(error, "unsupported IA-32 16-bit FF operation");
         const uint16_t result = static_cast<uint16_t>(
            operation == 0 ? before + 1 : before - 1);
         const uint32_t carry = m_state.eflags & kFlagCarry;
         if (operation == 0)
            SetAddFlags16(before, 1, result);
         else
            SetSubFlags16(before, 1, result);
         m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
         return WriteRM16(operand, result, error);
      }
      if (opcode == 0xf7)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint16_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM16(operand, &value, error))
            return false;
         if (operation == 0)
         {
            uint16_t immediate;
            if (!Fetch16(&immediate, error)) return false;
            SetLogicFlags16(value & immediate);
            return true;
         }
         if (operation == 2)
            return WriteRM16(operand, static_cast<uint16_t>(~value), error);
         if (operation == 3)
         {
            const uint16_t result = static_cast<uint16_t>(0 - value);
            SetSubFlags16(0, value, result);
            return WriteRM16(operand, result, error);
         }
         return Fail(error, "unsupported IA-32 16-bit F7 operation");
      }
      if (opcode == 0x81 || opcode == 0x83)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint16_t left, immediate;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM16(operand, &left, error))
            return false;
         if (opcode == 0x81)
         {
            if (!Fetch16(&immediate, error)) return false;
         }
         else
         {
            uint8_t value;
            if (!Fetch8(&value, error)) return false;
            immediate = static_cast<uint16_t>(Sign8(value));
         }
         uint16_t result;
         switch (operation)
         {
            case 0: result = left + immediate; SetAddFlags16(left, immediate, result); break;
            case 1: result = left | immediate; SetLogicFlags16(result); break;
            case 4: result = left & immediate; SetLogicFlags16(result); break;
            case 5: result = left - immediate; SetSubFlags16(left, immediate, result); break;
            case 6: result = left ^ immediate; SetLogicFlags16(result); break;
            case 7: result = left - immediate; SetSubFlags16(left, immediate, result); return true;
            default: return Fail(error, "unsupported IA-32 16-bit immediate arithmetic operation");
         }
         return WriteRM16(operand, result, error);
      }
      if (opcode == 0xc1 || opcode == 0xd1 || opcode == 0xd3)
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint16_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM16(operand, &value, error))
            return false;
         uint8_t rawCount = 1;
         if (opcode == 0xc1 && !Fetch8(&rawCount, error)) return false;
         if (opcode == 0xd3) rawCount = ReadReg8(1);
         unsigned count = rawCount & 31;
         if (!count) return true;

         uint16_t result = value;
         bool carry = false;
         switch (operation)
         {
            case 0: // ROL
               count %= 16;
               if (!count) return true;
               result = static_cast<uint16_t>((value << count) |
                                              (value >> (16 - count)));
               carry = (result & 1) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 15) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 1: // ROR
               count %= 16;
               if (!count) return true;
               result = static_cast<uint16_t>((value >> count) |
                                              (value << (16 - count)));
               carry = (result & 0x8000u) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 15) ^ (result >> 14)) & 1))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 4: case 6: // SHL/SAL
               carry = count <= 16 && ((value >> (16 - count)) & 1) != 0;
               result = count < 16 ? static_cast<uint16_t>(value << count) : 0;
               SetLogicFlags16(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 15) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 5: // SHR
               carry = count <= 16 && ((value >> (count - 1)) & 1) != 0;
               result = count < 16 ? static_cast<uint16_t>(value >> count) : 0;
               SetLogicFlags16(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (value & 0x8000u))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 7: // SAR
               carry = count <= 16 && ((value >> (count - 1)) & 1) != 0;
               result = count < 16 ? static_cast<uint16_t>(
                  static_cast<int16_t>(value) >> count) :
                  static_cast<uint16_t>((value & 0x8000u) ? 0xffffu : 0);
               SetLogicFlags16(result);
               if (carry) m_state.eflags |= kFlagCarry;
               break;
            default:
               return Fail(error, "unsupported IA-32 16-bit shift operation");
         }
         return WriteRM16(operand, result, error);
      }
      if (opcode == 0x90)
         return true;   // (66 90: a two-byte NOP, GCC's padding)
      return FailAddress(error, "unsupported IA-32 operand-size override at",
                         instructionAddress);
   }

   if (opcode >= 0x40 && opcode <= 0x47)
   {
      const unsigned reg = opcode - 0x40;
      const uint32_t before = m_state.reg[reg];
      const uint32_t result = before + 1;
      m_state.reg[reg] = result;
      SetIncFlags(before, result);
      return true;
   }
   if (opcode >= 0x48 && opcode <= 0x4f)
   {
      const unsigned reg = opcode - 0x48;
      const uint32_t before = m_state.reg[reg];
      const uint32_t result = before - 1;
      m_state.reg[reg] = result;
      SetDecFlags(before, result);
      return true;
   }
   if (opcode >= 0x50 && opcode <= 0x57)
      return Push32(m_state.reg[opcode - 0x50], error);
   if (opcode >= 0x58 && opcode <= 0x5f)
      return Pop32(&m_state.reg[opcode - 0x58], error);
   if (opcode >= 0x70 && opcode <= 0x7f)
   {
      uint8_t displacement;
      if (!Fetch8(&displacement, error))
         return false;
      if (Condition(opcode - 0x70))
         m_state.eip += Sign8(displacement);
      return true;
   }
   if (opcode >= 0xb0 && opcode <= 0xb7)
   {
      uint8_t value;
      if (!Fetch8(&value, error))
         return false;
      WriteReg8(opcode - 0xb0, value);
      return true;
   }
   if (opcode >= 0xb8 && opcode <= 0xbf)
      return Fetch32(&m_state.reg[opcode - 0xb8], error);

   switch (opcode)
   {
      case 0x00: case 0x02:
      case 0x08: case 0x0a:
      case 0x10: case 0x12:
      case 0x18: case 0x1a:
      case 0x20: case 0x22:
      case 0x28: case 0x2a:
      case 0x30: case 0x32:
      case 0x38: case 0x3a:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         uint8_t memoryValue;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error) ||
             !ReadRM8(operand, &memoryValue, error))
            return false;
         const bool registerDestination = (opcode & 2) != 0;
         const uint8_t left = registerDestination
            ? ReadReg8(regField) : memoryValue;
         const uint8_t right = registerDestination
            ? memoryValue : ReadReg8(regField);
         uint8_t result = 0;
         const bool carry = (m_state.eflags & kFlagCarry) != 0;
         switch (opcode & 0xfc)
         {
            case 0x00: result = left + right; SetAddFlags8(left, right, result); break;
            case 0x08: result = left | right; SetLogicFlags8(result); break;
            case 0x10:
               result = left + right + carry;
               SetAdcFlags8(left, right, carry, result);
               break;
            case 0x18:
               result = left - right - carry;
               SetSbbFlags8(left, right, carry, result);
               break;
            case 0x20: result = left & right; SetLogicFlags8(result); break;
            case 0x28: result = left - right; SetSubFlags8(left, right, result); break;
            case 0x30: result = left ^ right; SetLogicFlags8(result); break;
            case 0x38: result = left - right; SetSubFlags8(left, right, result); return true;
         }
         if (registerDestination)
            WriteReg8(regField, result);
         else
            return WriteRM8(operand, result, error);
         return true;
      }

      case 0x01: case 0x03:
      case 0x09: case 0x0b:
      case 0x11: case 0x13:
      case 0x19: case 0x1b:
      case 0x21: case 0x23:
      case 0x29: case 0x2b:
      case 0x31: case 0x33:
      case 0x39: case 0x3b:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         uint32_t memoryValue;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error) ||
             !ReadRM32(operand, &memoryValue, error))
            return false;
         const bool registerDestination = (opcode & 2) != 0;
         const uint32_t left = registerDestination
            ? m_state.reg[regField] : memoryValue;
         const uint32_t right = registerDestination
            ? memoryValue : m_state.reg[regField];
         uint32_t result = 0;
         const bool carry = (m_state.eflags & kFlagCarry) != 0;
         switch (opcode & 0xfc)
         {
            case 0x00: result = left + right; SetAddFlags(left, right, result); break;
            case 0x08: result = left | right; SetLogicFlags(result); break;
            case 0x10:
               result = left + right + carry;
               SetAdcFlags(left, right, carry, result);
               break;
            case 0x18:
               result = left - right - carry;
               SetSbbFlags(left, right, carry, result);
               break;
            case 0x20: result = left & right; SetLogicFlags(result); break;
            case 0x28: result = left - right; SetSubFlags(left, right, result); break;
            case 0x30: result = left ^ right; SetLogicFlags(result); break;
            case 0x38: result = left - right; SetSubFlags(left, right, result); return true;
         }
         if (registerDestination)
            m_state.reg[regField] = result;
         else
            return WriteRM32(operand, result, error);
         return true;
      }

      case 0x04: case 0x0c: case 0x14: case 0x1c:
      case 0x24: case 0x2c: case 0x34: case 0x3c:
      {
         uint8_t immediate;
         if (!Fetch8(&immediate, error))
            return false;
         const uint8_t left = ReadReg8(0);
         uint8_t result;
         const bool carry = (m_state.eflags & kFlagCarry) != 0;
         switch (opcode)
         {
            case 0x04: result = left + immediate; SetAddFlags8(left, immediate, result); break;
            case 0x0c: result = left | immediate; SetLogicFlags8(result); break;
            case 0x14:
               result = left + immediate + carry;
               SetAdcFlags8(left, immediate, carry, result);
               break;
            case 0x1c:
               result = left - immediate - carry;
               SetSbbFlags8(left, immediate, carry, result);
               break;
            case 0x24: result = left & immediate; SetLogicFlags8(result); break;
            case 0x2c: result = left - immediate; SetSubFlags8(left, immediate, result); break;
            case 0x34: result = left ^ immediate; SetLogicFlags8(result); break;
            default: result = left - immediate; SetSubFlags8(left, immediate, result); return true;
         }
         WriteReg8(0, result);
         return true;
      }

      case 0x05: case 0x0d: case 0x25: case 0x2d: case 0x35: case 0x3d:
      {
         uint32_t immediate;
         if (!Fetch32(&immediate, error))
            return false;
         const uint32_t left = m_state.reg[kEax];
         uint32_t result;
         switch (opcode)
         {
            case 0x05: result = left + immediate; SetAddFlags(left, immediate, result); break;
            case 0x0d: result = left | immediate; SetLogicFlags(result); break;
            case 0x25: result = left & immediate; SetLogicFlags(result); break;
            case 0x2d: result = left - immediate; SetSubFlags(left, immediate, result); break;
            case 0x35: result = left ^ immediate; SetLogicFlags(result); break;
            default: result = left - immediate; SetSubFlags(left, immediate, result); return true;
         }
         m_state.reg[kEax] = result;
         return true;
      }

      case 0x64:
      {
         // VC9's DLL startup reads the thread environment block through an
         // FS-segment MOV (notably fs:[18h], the TEB self pointer).  The
         // emulator exposes the small TEB image in its proxy page.
         const uint32_t kGuestThreadEnvironmentBlock = 0x60000a00;
         uint8_t nestedOpcode;
         if (!Fetch8(&nestedOpcode, error))
            return false;
         if (nestedOpcode == 0x8b)
         {
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            uint32_t value;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error))
               return false;
            if (!operand.isRegister)
               operand.address += kGuestThreadEnvironmentBlock;
            if (!ReadRM32(operand, &value, error))
               return false;
            m_state.reg[regField] = value;
            return true;
         }
         if (nestedOpcode == 0x89)
         {
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error))
               return false;
            if (!operand.isRegister)
               operand.address += kGuestThreadEnvironmentBlock;
            return WriteRM32(operand, m_state.reg[regField], error);
         }
         if (nestedOpcode == 0xff)
         {
            uint8_t modrm;
            sDecodedRM operand;
            unsigned operation;
            uint32_t value;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &operation, error))
               return false;
            if (operation != 6)
               return Fail(error, "unsupported IA-32 FS FF operation");
            if (!operand.isRegister)
               operand.address += kGuestThreadEnvironmentBlock;
            return ReadRM32(operand, &value, error) && Push32(value, error);
         }
         if (nestedOpcode == 0xa1 || nestedOpcode == 0xa3)
         {
            uint32_t offset;
            if (!Fetch32(&offset, error))
               return false;
            const uint32_t address = kGuestThreadEnvironmentBlock + offset;
            if (nestedOpcode == 0xa1)
               return m_memory.Read32(address, &m_state.reg[kEax], error);
            return m_memory.Write32(address, m_state.reg[kEax], error);
         }
         return FailOpcode(error, "unsupported IA-32 FS opcode",
                           instructionAddress, nestedOpcode);
      }

      case 0x68:
      {
         uint32_t value;
         return Fetch32(&value, error) && Push32(value, error);
      }

      case 0x6a:
      {
         uint8_t value;
         return Fetch8(&value, error) &&
                Push32(static_cast<uint32_t>(Sign8(value)), error);
      }

      case 0x69: case 0x6b:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         uint32_t source, immediate;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error) ||
             !ReadRM32(operand, &source, error))
            return false;
         if (opcode == 0x69)
         {
            if (!Fetch32(&immediate, error)) return false;
         }
         else
         {
            uint8_t value;
            if (!Fetch8(&value, error)) return false;
            immediate = static_cast<uint32_t>(Sign8(value));
         }
         const int64_t product = static_cast<int64_t>(static_cast<int32_t>(source)) *
                                 static_cast<int32_t>(immediate);
         m_state.reg[regField] = static_cast<uint32_t>(product);
         m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
         if (product != static_cast<int32_t>(product))
            m_state.eflags |= kFlagCarry | kFlagOverflow;
         return true;
      }

      case 0x80:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint8_t left, immediate;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM8(operand, &left, error) ||
             !Fetch8(&immediate, error))
            return false;
         uint8_t result;
         const bool carry = (m_state.eflags & kFlagCarry) != 0;
         switch (operation)
         {
            case 0: result = left + immediate; SetAddFlags8(left, immediate, result); break;
            case 1: result = left | immediate; SetLogicFlags8(result); break;
            case 2:
               result = left + immediate + carry;
               SetAdcFlags8(left, immediate, carry, result);
               break;
            case 3:
               result = left - immediate - carry;
               SetSbbFlags8(left, immediate, carry, result);
               break;
            case 4: result = left & immediate; SetLogicFlags8(result); break;
            case 5: result = left - immediate; SetSubFlags8(left, immediate, result); break;
            case 6: result = left ^ immediate; SetLogicFlags8(result); break;
            case 7: result = left - immediate; SetSubFlags8(left, immediate, result); return true;
            default: return Fail(error, "unsupported IA-32 byte immediate arithmetic operation");
         }
         return WriteRM8(operand, result, error);
      }

      case 0x81: case 0x83:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint32_t left, immediate;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM32(operand, &left, error))
            return false;
         if (opcode == 0x81)
         {
            if (!Fetch32(&immediate, error)) return false;
         }
         else
         {
            uint8_t value;
            if (!Fetch8(&value, error)) return false;
            immediate = static_cast<uint32_t>(Sign8(value));
         }
         uint32_t result;
         const bool carry = (m_state.eflags & kFlagCarry) != 0;
         switch (operation)
         {
            case 0: result = left + immediate; SetAddFlags(left, immediate, result); break;
            case 1: result = left | immediate; SetLogicFlags(result); break;
            case 2:
               result = left + immediate + carry;
               SetAdcFlags(left, immediate, carry, result);
               break;
            case 3:
               result = left - immediate - carry;
               SetSbbFlags(left, immediate, carry, result);
               break;
            case 4: result = left & immediate; SetLogicFlags(result); break;
            case 5: result = left - immediate; SetSubFlags(left, immediate, result); break;
            case 6: result = left ^ immediate; SetLogicFlags(result); break;
            case 7: result = left - immediate; SetSubFlags(left, immediate, result); return true;
            default: return Fail(error, "unsupported IA-32 immediate arithmetic operation");
         }
         return WriteRM32(operand, result, error);
      }

      case 0x84: case 0x85:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (opcode == 0x84)
         {
            uint8_t value;
            if (!ReadRM8(operand, &value, error)) return false;
            SetLogicFlags8(value & ReadReg8(regField));
         }
         else
         {
            uint32_t value;
            if (!ReadRM32(operand, &value, error)) return false;
            SetLogicFlags(value & m_state.reg[regField]);
         }
         return true;
      }

      case 0x86: case 0x87:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (opcode == 0x86)
         {
            uint8_t value;
            if (!ReadRM8(operand, &value, error) ||
                !WriteRM8(operand, ReadReg8(regField), error))
               return false;
            WriteReg8(regField, value);
            return true;
         }
         uint32_t value;
         if (!ReadRM32(operand, &value, error) ||
             !WriteRM32(operand, m_state.reg[regField], error))
            return false;
         m_state.reg[regField] = value;
         return true;
      }

      case 0x88: case 0x8a:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (opcode == 0x88)
            return WriteRM8(operand, ReadReg8(regField), error);
         uint8_t value;
         if (!ReadRM8(operand, &value, error))
            return false;
         WriteReg8(regField, value);
         return true;
      }

      case 0x89: case 0x8b:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (opcode == 0x89)
            return WriteRM32(operand, m_state.reg[regField], error);
         return ReadRM32(operand, &m_state.reg[regField], error);
      }

      case 0x8d:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned regField;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &regField, error))
            return false;
         if (operand.isRegister)
            return Fail(error, "LEA requires a memory operand");
         m_state.reg[regField] = operand.address;
         return true;
      }

      case 0x90:
         return true;

      case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
      {
         // XCHG EAX, r32 (the newer runtime's stack probe: xchg eax, esp)
         const unsigned other = opcode - 0x90;
         const uint32_t value = m_state.reg[kEax];
         m_state.reg[kEax] = m_state.reg[other];
         m_state.reg[other] = value;
         return true;
      }

      case 0x99: // CDQ
         m_state.reg[kEdx] = (m_state.reg[kEax] & 0x80000000u) ?
                             0xffffffffu : 0;
         return true;

      case 0x9b: // WAIT/FWAIT; floating-point execution is synchronous here.
         return true;

      case 0x9c: // PUSHFD
         return Push32(m_state.eflags, error);

      case 0x9d: // POPFD
         if (!Pop32(&m_state.eflags, error))
            return false;
         m_state.eflags |= 0x00000002u;
         return true;

      case 0x9e: // SAHF: SF ZF AF PF CF from AH
         m_state.eflags = (m_state.eflags & ~0xd5u) |
                          ((m_state.reg[kEax] >> 8) & 0xd5u) | 0x2u;
         return true;

      case 0x9f: // LAHF: AH = SF ZF 0 AF 0 PF 1 CF
         m_state.reg[kEax] = (m_state.reg[kEax] & 0xffff00ffu) |
                             (((m_state.eflags & 0xd5u) | 0x2u) << 8);
         return true;

      case 0xa9:
      {
         uint32_t immediate;
         if (!Fetch32(&immediate, error))
            return false;
         SetLogicFlags(m_state.reg[kEax] & immediate);
         return true;
      }

      case 0xa8:
      {
         uint8_t immediate;
         if (!Fetch8(&immediate, error))
            return false;
         SetLogicFlags8(ReadReg8(0) & immediate);
         return true;
      }

      case 0xa0:
      {
         uint32_t address;
         uint8_t value;
         if (!Fetch32(&address, error) ||
             !m_memory.Read8(address, &value, error))
            return false;
         WriteReg8(0, value);
         return true;
      }

      case 0xa1:
      {
         uint32_t address;
         return Fetch32(&address, error) &&
                m_memory.Read32(address, &m_state.reg[kEax], error);
      }

      case 0xa3:
      {
         uint32_t address;
         return Fetch32(&address, error) &&
                m_memory.Write32(address, m_state.reg[kEax], error);
      }

      case 0xa2:
      {
         uint32_t address;
         return Fetch32(&address, error) &&
                m_memory.Write8(address, ReadReg8(0), error);
      }

      case 0xc2:
      {
         uint16_t bytes;
         uint32_t target;
         if (!Fetch16(&bytes, error) || !Pop32(&target, error))
            return false;
         m_state.reg[kEsp] += bytes;
         m_state.eip = target;
         return true;
      }

      case 0xc1: case 0xd1: case 0xd3:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint32_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM32(operand, &value, error))
            return false;

         uint8_t rawCount = 1;
         if (opcode == 0xc1 && !Fetch8(&rawCount, error))
            return false;
         if (opcode == 0xd3)
            rawCount = ReadReg8(1);
         const unsigned count = rawCount & 31;
         if (!count)
            return true;

         uint32_t result;
         bool carry;
         switch (operation)
         {
            case 0: // ROL
               result = (value << count) | (value >> (32 - count));
               carry = (result & 1) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 31) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 1: // ROR
               result = (value >> count) | (value << (32 - count));
               carry = (result & 0x80000000u) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 31) ^ (result >> 30)) & 1))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 4: case 6: // SHL/SAL
               carry = ((value >> (32 - count)) & 1) != 0;
               result = value << count;
               SetLogicFlags(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 31) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 5: // SHR
               carry = ((value >> (count - 1)) & 1) != 0;
               result = value >> count;
               SetLogicFlags(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (value & 0x80000000u))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 7: // SAR
               carry = ((value >> (count - 1)) & 1) != 0;
               result = static_cast<uint32_t>(static_cast<int32_t>(value) >> count);
               SetLogicFlags(result);
               if (carry) m_state.eflags |= kFlagCarry;
               break;
            default:
               return Fail(error, "unsupported IA-32 shift operation");
         }
         return WriteRM32(operand, result, error);
      }

      case 0xc0: case 0xd0: case 0xd2:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint8_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM8(operand, &value, error))
            return false;

         uint8_t rawCount = 1;
         if (opcode == 0xc0 && !Fetch8(&rawCount, error)) return false;
         if (opcode == 0xd2) rawCount = ReadReg8(1);
         unsigned count = rawCount & 31;
         if (!count) return true;

         uint8_t result = value;
         bool carry = false;
         switch (operation)
         {
            case 0: // ROL
               count %= 8;
               if (!count) return true;
               result = static_cast<uint8_t>((value << count) |
                                             (value >> (8 - count)));
               carry = (result & 1) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 7) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 1: // ROR
               count %= 8;
               if (!count) return true;
               result = static_cast<uint8_t>((value >> count) |
                                             (value << (8 - count)));
               carry = (result & 0x80) != 0;
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 7) ^ (result >> 6)) & 1))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 4: case 6: // SHL/SAL
               carry = count <= 8 && ((value >> (8 - count)) & 1) != 0;
               result = count < 8 ? static_cast<uint8_t>(value << count) : 0;
               SetLogicFlags8(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (((result >> 7) & 1) !=
                                  static_cast<unsigned>(carry)))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 5: // SHR
               carry = count <= 8 && ((value >> (count - 1)) & 1) != 0;
               result = count < 8 ? static_cast<uint8_t>(value >> count) : 0;
               SetLogicFlags8(result);
               if (carry) m_state.eflags |= kFlagCarry;
               if (count == 1 && (value & 0x80))
                  m_state.eflags |= kFlagOverflow;
               break;
            case 7: // SAR
               carry = count <= 8 && ((value >> (count - 1)) & 1) != 0;
               result = count < 8 ? static_cast<uint8_t>(
                  static_cast<int8_t>(value) >> count) :
                  static_cast<uint8_t>((value & 0x80) ? 0xff : 0);
               SetLogicFlags8(result);
               if (carry) m_state.eflags |= kFlagCarry;
               break;
            default:
               return Fail(error, "unsupported IA-32 byte shift operation");
         }
         return WriteRM8(operand, result, error);
      }

      case 0xc3:
      {
         uint32_t target;
         if (!Pop32(&target, error))
            return false;
         m_state.eip = target;
         return true;
      }

      case 0xc6:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint8_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !Fetch8(&value, error))
            return false;
         if (operation != 0)
            return Fail(error, "invalid byte MOV immediate encoding");
         return WriteRM8(operand, value, error);
      }

      case 0xc7:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint32_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !Fetch32(&value, error))
            return false;
         if (operation != 0)
            return Fail(error, "invalid MOV immediate encoding");
         return WriteRM32(operand, value, error);
      }

      case 0xc9:
      {
         m_state.reg[kEsp] = m_state.reg[kEbp];
         return Pop32(&m_state.reg[kEbp], error);
      }

      case 0xd8:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error))
            return false;
         if (!m_state.x87Depth)
            return Fail(error, "x87 stack underflow");

         double value;
         if (operand.isRegister)
         {
            const unsigned index = modrm & 7;
            if (index >= m_state.x87Depth)
               return Fail(error, "x87 stack register is empty");
            value = m_state.x87[index];
         }
         else
         {
            uint32_t bits;
            float single;
            if (!m_memory.Read32(operand.address, &bits, error))
               return false;
            memcpy(&single, &bits, sizeof(single));
            value = single;
         }

         if (operation == 2 || operation == 3) // FCOM/FCOMP
         {
            const double top = m_state.x87[0];
            m_state.x87Status &= ~0x4500u;
            if (std::isnan(top) || std::isnan(value))
               m_state.x87Status |= 0x4500u;
            else if (top < value)
               m_state.x87Status |= 0x0100u;
            else if (top == value)
               m_state.x87Status |= 0x4000u;
            if (operation == 3)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }

         switch (operation)
         {
            case 0: m_state.x87[0] += value; break;
            case 1: m_state.x87[0] *= value; break;
            case 4: m_state.x87[0] -= value; break;
            case 5: m_state.x87[0] = value - m_state.x87[0]; break;
            case 6: m_state.x87[0] /= value; break;
            case 7: m_state.x87[0] = value / m_state.x87[0]; break;
         }
         return true;
      }

      case 0xd7: // XLAT byte ptr [EBX + AL]
      {
         uint8_t value;
         const uint32_t address = m_state.reg[kEbx] +
            (m_state.reg[kEax] & 0xffu);
         if (!m_memory.Read8(address, &value, error))
            return false;
         WriteReg8(0, value);
         return true;
      }

      case 0xd9:
      {
         uint8_t second;
         if (!Fetch8(&second, error))
            return false;
         if ((second & 0xf8u) == 0xc0u) // FLD ST(i)
         {
            const unsigned index = second & 7u;
            if (index >= m_state.x87Depth)
               return Fail(error, "x87 stack register is empty");
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            const double value = m_state.x87[index];
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = value;
            ++m_state.x87Depth;
            return true;
         }
         if ((second & 0xf8u) == 0xc8u) // FXCH ST(i)
         {
            const unsigned index = second & 7u;
            if (index >= m_state.x87Depth)
               return Fail(error, "x87 stack register is empty");
            std::swap(m_state.x87[0], m_state.x87[index]);
            return true;
         }
         if (second == 0xe0) // FCHS
         {
            if (!m_state.x87Depth) return Fail(error, "x87 stack underflow");
            m_state.x87[0] = -m_state.x87[0];
            return true;
         }
         if (second == 0xe1) // FABS
         {
            if (!m_state.x87Depth) return Fail(error, "x87 stack underflow");
            m_state.x87[0] = std::fabs(m_state.x87[0]);
            return true;
         }
         if (second == 0xe4) // FTST
         {
            if (!m_state.x87Depth) return Fail(error, "x87 stack underflow");
            const double value = m_state.x87[0];
            m_state.x87Status &= ~0x4500u;
            if (std::isnan(value))
               m_state.x87Status |= 0x4500u;
            else if (value < 0.0)
               m_state.x87Status |= 0x0100u;
            else if (value == 0.0)
               m_state.x87Status |= 0x4000u;
            return true;
         }
         if (second == 0xe5) // FXAM
         {
            m_state.x87Status &= ~0x4700u;
            if (!m_state.x87Depth)
            {
               m_state.x87Status |= 0x4100u;
               return true;
            }
            const double value = m_state.x87[0];
            if (std::signbit(value))
               m_state.x87Status |= 0x0200u;
            switch (std::fpclassify(value))
            {
               case FP_NAN:       m_state.x87Status |= 0x0100u; break;
               case FP_INFINITE:  m_state.x87Status |= 0x0500u; break;
               case FP_ZERO:      m_state.x87Status |= 0x4000u; break;
               case FP_SUBNORMAL: m_state.x87Status |= 0x4400u; break;
               default:           m_state.x87Status |= 0x0400u; break;
            }
            return true;
         }
         if (second == 0xe8)
         {
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = 1.0;
            ++m_state.x87Depth;
            return true;
         }
         if (second == 0xee) // FLDZ
         {
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = 0.0;
            ++m_state.x87Depth;
            return true;
         }
         if (second == 0xfa) // FSQRT
         {
            if (!m_state.x87Depth) return Fail(error, "x87 stack underflow");
            m_state.x87[0] = std::sqrt(m_state.x87[0]);
            return true;
         }
         sDecodedRM operand;
         unsigned operation;
         if (!DecodeRM(second, &operand, &operation, error))
            return false;
         if (operand.isRegister)
            return FailAddress(error, "unsupported IA-32 D9 opcode at",
                               instructionAddress);
         if (operation == 0) // FLD m32real
         {
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            uint32_t bits;
            float value;
            if (!m_memory.Read32(operand.address, &bits, error))
               return false;
            memcpy(&value, &bits, sizeof(value));
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = value;
            ++m_state.x87Depth;
            return true;
         }
         if (operation == 2 || operation == 3) // FST/FSTP m32real
         {
            if (!m_state.x87Depth)
               return Fail(error, "x87 stack underflow");
            const float value = static_cast<float>(m_state.x87[0]);
            uint32_t bits;
            memcpy(&bits, &value, sizeof(bits));
            if (!m_memory.Write32(operand.address, bits, error))
               return false;
            if (operation == 3)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }
         if (operation == 5)
         {
            uint16_t value;
            if (!m_memory.Read16(operand.address, &value, error))
               return false;
            m_state.x87Control = value;
            return true;
         }
         if (operation == 7)
            return m_memory.Write16(operand.address, m_state.x87Control, error);
         return FailAddress(error, "unsupported IA-32 D9 memory operation at",
                            instructionAddress);
      }

      case 0xda:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error))
            return false;
         if (operand.isRegister)
            return FailAddress(error, "unsupported IA-32 DA opcode at",
                               instructionAddress);
         if (!m_state.x87Depth)
            return Fail(error, "x87 stack underflow");
         uint32_t bits;
         if (!m_memory.Read32(operand.address, &bits, error))
            return false;
         const double value = static_cast<int32_t>(bits);
         if (operation == 2 || operation == 3) // FICOM/FICOMP m32int
         {
            const double top = m_state.x87[0];
            m_state.x87Status &= ~0x4500u;
            if (std::isnan(top))
               m_state.x87Status |= 0x4500u;
            else if (top < value)
               m_state.x87Status |= 0x0100u;
            else if (top == value)
               m_state.x87Status |= 0x4000u;
            if (operation == 3)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }
         switch (operation)
         {
            case 0: m_state.x87[0] += value; break;
            case 1: m_state.x87[0] *= value; break;
            case 4: m_state.x87[0] -= value; break;
            case 5: m_state.x87[0] = value - m_state.x87[0]; break;
            case 6: m_state.x87[0] /= value; break;
            case 7: m_state.x87[0] = value / m_state.x87[0]; break;
         }
         return true;
      }

      case 0xdb:
      {
         uint8_t second;
         if (!Fetch8(&second, error))
            return false;
         if (second == 0xe2) // FNCLEX
         {
            m_state.x87Status &= 0x7f00u;
            return true;
         }
         if (second == 0xe3) // FNINIT
         {
            m_state.x87Depth = 0;
            m_state.x87Status = 0;
            m_state.x87Control = 0x037f;
            return true;
         }
         sDecodedRM operand;
         unsigned operation;
         if (!DecodeRM(second, &operand, &operation, error))
            return false;
         if (!operand.isRegister && operation == 0) // FILD m32int
         {
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            uint32_t value;
            if (!m_memory.Read32(operand.address, &value, error))
               return false;
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = static_cast<int32_t>(value);
            ++m_state.x87Depth;
            return true;
         }
         if (!operand.isRegister && (operation == 2 || operation == 3))
         {
            // FIST/FISTP m32int.  MSVC's default x87 control word rounds to
            // nearest; the unusual modes are retained in bits 10-11.
            if (!m_state.x87Depth)
               return Fail(error, "x87 stack underflow");
            double rounded;
            switch ((m_state.x87Control >> 10) & 3)
            {
               case 1: rounded = std::floor(m_state.x87[0]); break;
               case 2: rounded = std::ceil(m_state.x87[0]); break;
               case 3: rounded = std::trunc(m_state.x87[0]); break;
               default: rounded = std::nearbyint(m_state.x87[0]); break;
            }
            const int32_t value =
               rounded < -2147483648.0 || rounded > 2147483647.0 ||
               std::isnan(rounded) ? static_cast<int32_t>(0x80000000u) :
               static_cast<int32_t>(rounded);
            if (!m_memory.Write32(operand.address,
                                  static_cast<uint32_t>(value), error))
               return false;
            if (operation == 3)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }
         return FailAddress(error, "unsupported IA-32 DB opcode at",
                            instructionAddress);
      }

      case 0xdc:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint64_t bits;
         double value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error))
            return false;
         if (operand.isRegister)
         {
            const unsigned index = modrm & 7;
            if (index >= m_state.x87Depth)
               return Fail(error, "x87 stack register is empty");
            const double top = m_state.x87[0];
            switch (operation)
            {
               case 0: m_state.x87[index] += top; break;
               case 1: m_state.x87[index] *= top; break;
               case 4: m_state.x87[index] = top - m_state.x87[index]; break;
               case 5: m_state.x87[index] -= top; break;
               case 6: m_state.x87[index] = top / m_state.x87[index]; break;
               case 7: m_state.x87[index] /= top; break;
               default:
                  return Fail(error, "unsupported x87 register comparison");
            }
            return true;
         }
         if (!m_state.x87Depth)
            return Fail(error, "x87 stack underflow");
         if (!m_memory.Read64(operand.address, &bits, error))
            return false;
         memcpy(&value, &bits, sizeof(value));
         switch (operation)
         {
            case 0: m_state.x87[0] += value; break;
            case 1: m_state.x87[0] *= value; break;
            case 2: case 3:
            {
               const double top = m_state.x87[0];
               m_state.x87Status &= ~0x4500u;
               if (std::isnan(top) || std::isnan(value))
                  m_state.x87Status |= 0x4500u;
               else if (top < value)
                  m_state.x87Status |= 0x0100u;
               else if (top == value)
                  m_state.x87Status |= 0x4000u;
               if (operation == 3)
               {
                  for (unsigned i = 1; i < m_state.x87Depth; ++i)
                     m_state.x87[i - 1] = m_state.x87[i];
                  --m_state.x87Depth;
               }
               break;
            }
            case 4: m_state.x87[0] -= value; break;
            case 5: m_state.x87[0] = value - m_state.x87[0]; break;
            case 6: m_state.x87[0] /= value; break;
            case 7: m_state.x87[0] = value / m_state.x87[0]; break;
         }
         return true;
      }

      case 0xdd:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error))
            return false;
         if (operand.isRegister)
            return Fail(error, "unsupported x87 register operation");
         if (operation == 0)
         {
            uint64_t bits;
            double value;
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            if (!m_memory.Read64(operand.address, &bits, error))
               return false;
            memcpy(&value, &bits, sizeof(value));
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = value;
            ++m_state.x87Depth;
            return true;
         }
         if (operation == 2 || operation == 3)
         {
            uint64_t bits;
            if (!m_state.x87Depth)
               return Fail(error, "x87 stack underflow");
            memcpy(&bits, &m_state.x87[0], sizeof(bits));
            if (!m_memory.Write64(operand.address, bits, error))
               return false;
            if (operation == 3)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }
         if (operation == 7) // FNSTSW m16
            return m_memory.Write16(operand.address, m_state.x87Status,
                                    error);
         return Fail(error, "unsupported IA-32 DD operation");
      }

      case 0xde:
      {
         uint8_t second;
         if (!Fetch8(&second, error)) return false;
         const unsigned operation = second & 0xf8u;
         const unsigned stackIndex = second & 7u;
         if (operation == 0xc0u || operation == 0xc8u ||
             operation == 0xe0u || operation == 0xe8u ||
             operation == 0xf0u || operation == 0xf8u)
         {
            if (!stackIndex || m_state.x87Depth <= stackIndex)
               return Fail(error, "x87 stack underflow");
            const double top = m_state.x87[0];
            const double other = m_state.x87[stackIndex];
            double result;
            switch (operation)
            {
               case 0xc0u: result = other + top; break;
               case 0xc8u: result = other * top; break;
               case 0xe0u: result = top - other; break;
               case 0xe8u: result = other - top; break;
               case 0xf0u: result = top / other; break;
               default:    result = other / top; break;
            }
            m_state.x87[stackIndex] = result;
            for (unsigned i = 1; i < m_state.x87Depth; ++i)
               m_state.x87[i - 1] = m_state.x87[i];
            --m_state.x87Depth;
            return true;
         }
         if (second != 0xd9)
            return FailAddress(error, "unsupported IA-32 DE opcode at",
                               instructionAddress);
         if (m_state.x87Depth < 2)
            return Fail(error, "x87 stack underflow");
         const double left = m_state.x87[0];
         const double right = m_state.x87[1];
         m_state.x87Status &= ~0x4500u;
         if (std::isnan(left) || std::isnan(right))
            m_state.x87Status |= 0x4500u;
         else if (left < right)
            m_state.x87Status |= 0x0100u;
         else if (left == right)
            m_state.x87Status |= 0x4000u;
         for (unsigned i = 2; i < m_state.x87Depth; ++i)
            m_state.x87[i - 2] = m_state.x87[i];
         m_state.x87Depth -= 2;
         return true;
      }

      case 0xdf:
      {
         uint8_t second;
         if (!Fetch8(&second, error))
            return false;
         if (second == 0xe0)
         {
            m_state.reg[kEax] = (m_state.reg[kEax] & 0xffff0000u) |
                                m_state.x87Status;
            return true;
         }
         sDecodedRM operand;
         unsigned operation;
         if (!DecodeRM(second, &operand, &operation, error))
            return false;
         if (operand.isRegister)
            return FailAddress(error, "unsupported IA-32 DF opcode at",
                               instructionAddress);
         if (operation == 0 || operation == 5) // FILD m16int/m64int
         {
            if (m_state.x87Depth == 8)
               return Fail(error, "x87 stack overflow");
            double value;
            if (operation == 0)
            {
               uint16_t bits;
               if (!m_memory.Read16(operand.address, &bits, error))
                  return false;
               value = static_cast<int16_t>(bits);
            }
            else
            {
               uint64_t bits;
               if (!m_memory.Read64(operand.address, &bits, error))
                  return false;
               value = static_cast<double>(static_cast<int64_t>(bits));
            }
            for (unsigned i = m_state.x87Depth; i != 0; --i)
               m_state.x87[i] = m_state.x87[i - 1];
            m_state.x87[0] = value;
            ++m_state.x87Depth;
            return true;
         }
         if (operation == 2 || operation == 3 || operation == 7)
         {
            // FIST/FISTP m16int and FISTP m64int.
            if (!m_state.x87Depth)
               return Fail(error, "x87 stack underflow");
            double rounded;
            switch ((m_state.x87Control >> 10) & 3)
            {
               case 1: rounded = std::floor(m_state.x87[0]); break;
               case 2: rounded = std::ceil(m_state.x87[0]); break;
               case 3: rounded = std::trunc(m_state.x87[0]); break;
               default: rounded = std::nearbyint(m_state.x87[0]); break;
            }
            bool written;
            if (operation == 7)
            {
               const uint64_t value =
                  rounded < -9223372036854775808.0 ||
                  rounded >= 9223372036854775808.0 ||
                  std::isnan(rounded) ? 0x8000000000000000ULL :
                  static_cast<uint64_t>(static_cast<int64_t>(rounded));
               written = m_memory.Write64(operand.address, value, error);
            }
            else
            {
               const uint16_t value =
                  rounded < -32768.0 || rounded > 32767.0 ||
                  std::isnan(rounded) ? 0x8000u :
                  static_cast<uint16_t>(static_cast<int16_t>(rounded));
               written = m_memory.Write16(operand.address, value, error);
            }
            if (!written) return false;
            if (operation == 3 || operation == 7)
            {
               for (unsigned i = 1; i < m_state.x87Depth; ++i)
                  m_state.x87[i - 1] = m_state.x87[i];
               --m_state.x87Depth;
            }
            return true;
         }
         return FailAddress(error, "unsupported IA-32 DF opcode at",
                            instructionAddress);
      }

      case 0xe8:
      {
         uint32_t displacement;
         if (!Fetch32(&displacement, error))
            return false;
         const uint32_t returnAddress = m_state.eip;
         if (!Push32(returnAddress, error))
            return false;
         m_state.eip += displacement;
         return true;
      }

      case 0xe9:
      {
         uint32_t displacement;
         if (!Fetch32(&displacement, error))
            return false;
         m_state.eip += displacement;
         return true;
      }

      case 0xeb:
      {
         uint8_t displacement;
         if (!Fetch8(&displacement, error))
            return false;
         m_state.eip += Sign8(displacement);
         return true;
      }

      case 0xf6:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint8_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM8(operand, &value, error))
            return false;
         if (operation == 0)
         {
            uint8_t immediate;
            if (!Fetch8(&immediate, error)) return false;
            SetLogicFlags8(value & immediate);
            return true;
         }
         if (operation == 2)
            return WriteRM8(operand, static_cast<uint8_t>(~value), error);
         if (operation == 3)
         {
            const uint8_t result = static_cast<uint8_t>(0 - value);
            SetSubFlags8(0, value, result);
            return WriteRM8(operand, result, error);
         }
         return Fail(error, "unsupported IA-32 F6 operation");
      }

      case 0xf7:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint32_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM32(operand, &value, error))
            return false;
         if (operation == 0)
         {
            uint32_t immediate;
            if (!Fetch32(&immediate, error)) return false;
            SetLogicFlags(value & immediate);
            return true;
         }
         if (operation == 2)
            return WriteRM32(operand, ~value, error);
         if (operation == 3)
         {
            const uint32_t result = 0 - value;
            SetSubFlags(0, value, result);
            return WriteRM32(operand, result, error);
         }
         if (operation == 4)
         {
            const uint64_t product =
               static_cast<uint64_t>(m_state.reg[kEax]) * value;
            m_state.reg[kEax] = static_cast<uint32_t>(product);
            m_state.reg[kEdx] = static_cast<uint32_t>(product >> 32);
            m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
            if (m_state.reg[kEdx])
               m_state.eflags |= kFlagCarry | kFlagOverflow;
            return true;
         }
         if (operation == 5)
         {
            const int64_t product =
               static_cast<int64_t>(static_cast<int32_t>(m_state.reg[kEax])) *
               static_cast<int32_t>(value);
            const uint64_t bits = static_cast<uint64_t>(product);
            m_state.reg[kEax] = static_cast<uint32_t>(bits);
            m_state.reg[kEdx] = static_cast<uint32_t>(bits >> 32);
            m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
            if (product != static_cast<int32_t>(product))
               m_state.eflags |= kFlagCarry | kFlagOverflow;
            return true;
         }
         if (operation == 6)
         {
            if (!value)
               return Fail(error, "IA-32 unsigned division by zero");
            const uint64_t dividend =
               (static_cast<uint64_t>(m_state.reg[kEdx]) << 32) |
               m_state.reg[kEax];
            const uint64_t quotient = dividend / value;
            if (quotient > 0xffffffffull)
               return Fail(error, "IA-32 unsigned division overflow");
            m_state.reg[kEax] = static_cast<uint32_t>(quotient);
            m_state.reg[kEdx] = static_cast<uint32_t>(dividend % value);
            return true;
         }
         if (operation == 7)
         {
            const int32_t divisor = static_cast<int32_t>(value);
            if (!divisor)
               return Fail(error, "IA-32 signed division by zero");
            const uint64_t dividendBits =
               (static_cast<uint64_t>(m_state.reg[kEdx]) << 32) |
               m_state.reg[kEax];
            int64_t dividend;
            memcpy(&dividend, &dividendBits, sizeof(dividend));
            if (dividend == (std::numeric_limits<int64_t>::min)() &&
                divisor == -1)
               return Fail(error, "IA-32 signed division overflow");
            const int64_t quotient = dividend / divisor;
            if (quotient < (std::numeric_limits<int32_t>::min)() ||
                quotient > (std::numeric_limits<int32_t>::max)())
               return Fail(error, "IA-32 signed division overflow");
            m_state.reg[kEax] = static_cast<uint32_t>(quotient);
            m_state.reg[kEdx] = static_cast<uint32_t>(dividend % divisor);
            return true;
         }
         return Fail(error, "unsupported IA-32 F7 operation");
      }

      case 0xfc:
         m_state.eflags &= ~kFlagDirection;
         return true;

      case 0xfd:
         m_state.eflags |= kFlagDirection;
         return true;

      case 0xfe:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint8_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM8(operand, &value, error))
            return false;
         const uint32_t carry = m_state.eflags & kFlagCarry;
         if (operation == 0)
         {
            const uint8_t result = static_cast<uint8_t>(value + 1);
            SetAddFlags8(value, 1, result);
            m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
            return WriteRM8(operand, result, error);
         }
         if (operation == 1)
         {
            const uint8_t result = static_cast<uint8_t>(value - 1);
            SetSubFlags8(value, 1, result);
            m_state.eflags = (m_state.eflags & ~kFlagCarry) | carry;
            return WriteRM8(operand, result, error);
         }
         return Fail(error, "unsupported IA-32 FE operation");
      }

      case 0xff:
      {
         uint8_t modrm;
         sDecodedRM operand;
         unsigned operation;
         uint32_t value;
         if (!Fetch8(&modrm, error) ||
             !DecodeRM(modrm, &operand, &operation, error) ||
             !ReadRM32(operand, &value, error))
            return false;
         switch (operation)
         {
            case 0:
            {
               const uint32_t result = value + 1;
               SetIncFlags(value, result);
               return WriteRM32(operand, result, error);
            }
            case 1:
            {
               const uint32_t result = value - 1;
               SetDecFlags(value, result);
               return WriteRM32(operand, result, error);
            }
            case 2:
            {
               const uint32_t returnAddress = m_state.eip;
               if (!Push32(returnAddress, error)) return false;
               m_state.eip = value;
               return true;
            }
            case 4: m_state.eip = value; return true;
            case 6: return Push32(value, error);
            default: return Fail(error, "unsupported IA-32 FF operation");
         }
      }

      case 0x0f:
      {
         uint8_t second;
         if (!Fetch8(&second, error))
            return false;
         if (second == 0xa2) // CPUID
         {
            if (m_state.reg[kEax] == 0)
            {
               m_state.reg[kEax] = 1;
               m_state.reg[kEbx] = 0x756e6547u; // "Genu"
               m_state.reg[kEdx] = 0x49656e69u; // "ineI"
               m_state.reg[kEcx] = 0x6c65746eu; // "ntel"
            }
            else if (m_state.reg[kEax] == 1)
            {
               // Report an i686 baseline without optional SIMD extensions;
               // the interpreter implements the scalar CRT path.
               m_state.reg[kEax] = 0x00000600u;
               m_state.reg[kEbx] = 0;
               m_state.reg[kEcx] = 0;
               m_state.reg[kEdx] = 0;
            }
            else
               m_state.reg[kEax] = m_state.reg[kEbx] =
                  m_state.reg[kEcx] = m_state.reg[kEdx] = 0;
            return true;
         }
         if (second == 0xc1 || second == 0xb1)
         {
            // XADD r/m32, r32; CMPXCHG r/m32, r32
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            uint32_t value;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error) ||
                !ReadRM32(operand, &value, error))
               return false;
            if (second == 0xc1)
            {
               const uint32_t sum = value + m_state.reg[regField];
               SetAddFlags(value, m_state.reg[regField], sum);
               if (!WriteRM32(operand, sum, error)) return false;
               m_state.reg[regField] = value;
               return true;
            }
            SetSubFlags(m_state.reg[kEax], value, m_state.reg[kEax] - value);
            if (m_state.reg[kEax] == value)
               return WriteRM32(operand, m_state.reg[regField], error);
            m_state.reg[kEax] = value;
            return true;
         }
         if (second == 0xba || second == 0xa3 || second == 0xab || second == 0xb3 || second == 0xbb)
         {
            // BT, BTS, BTR, BTC: r/m32 by an immediate (0F BA /4-/7, a newer
            // Visual C++ runtime's test of what the processor has) or by a
            // register (a bit string from memory: the register's offset,
            // signed, past the dword at the operand)
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error))
               return false;
            unsigned operation;
            uint32_t bit;
            if (second == 0xba)
            {
               uint8_t immediate;
               if (!Fetch8(&immediate, error)) return false;
               if (regField < 4)
                  return FailOpcode(error, "unsupported IA-32 0F BA form", instructionAddress, regField);
               operation = regField - 4;
               bit = immediate & 31;
            }
            else
            {
               operation = second == 0xa3 ? 0 : second == 0xab ? 1 : second == 0xb3 ? 2 : 3;
               const int32_t offset = static_cast<int32_t>(m_state.reg[regField]);
               if (operand.isRegister)
                  bit = static_cast<uint32_t>(offset) & 31;
               else
               {
                  operand.address += static_cast<uint32_t>((offset >> 5) * 4);
                  bit = static_cast<uint32_t>(offset) & 31;
               }
            }
            uint32_t value;
            if (!ReadRM32(operand, &value, error)) return false;
            const uint32_t mask = 1u << bit;
            m_state.eflags = (m_state.eflags & ~kFlagCarry) | ((value & mask) ? kFlagCarry : 0);
            if (operation == 0)
               return true;
            const uint32_t result = operation == 1 ? (value | mask) : operation == 2 ? (value & ~mask) : (value ^ mask);
            return WriteRM32(operand, result, error);
         }
         if (second == 0xbc || second == 0xbd)
         {
            // BSF, BSR: the lowest, the highest bit set (none: ZF, the
            // destination left)
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            uint32_t value;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error) ||
                !ReadRM32(operand, &value, error))
               return false;
            if (!value)
            {
               m_state.eflags |= kFlagZero;
               return true;
            }
            m_state.eflags &= ~kFlagZero;
            unsigned index = 0;
            if (second == 0xbc)
               while (!(value & (1u << index))) ++index;
            else
            {
               index = 31;
               while (!(value & (1u << index))) --index;
            }
            m_state.reg[regField] = index;
            return true;
         }
         if (second >= 0x80 && second <= 0x8f)
         {
            uint32_t displacement;
            if (!Fetch32(&displacement, error)) return false;
            if (Condition(second - 0x80)) m_state.eip += displacement;
            return true;
         }
         if (second >= 0x90 && second <= 0x9f)
         {
            uint8_t modrm;
            sDecodedRM operand;
            unsigned ignored;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &ignored, error))
               return false;
            return WriteRM8(operand, Condition(second - 0x90) ? 1 : 0,
                            error);
         }
         if (second == 0xaf || second == 0xb6 || second == 0xb7 ||
             second == 0xbe || second == 0xbf)
         {
            uint8_t modrm;
            sDecodedRM operand;
            unsigned regField;
            if (!Fetch8(&modrm, error) ||
                !DecodeRM(modrm, &operand, &regField, error))
               return false;
            if (second == 0xaf)
            {
               uint32_t source;
               if (!ReadRM32(operand, &source, error)) return false;
               const int64_t product =
                  static_cast<int64_t>(static_cast<int32_t>(m_state.reg[regField])) *
                  static_cast<int32_t>(source);
               m_state.reg[regField] = static_cast<uint32_t>(product);
               m_state.eflags &= ~(kFlagCarry | kFlagOverflow);
               if (product != static_cast<int32_t>(product))
                  m_state.eflags |= kFlagCarry | kFlagOverflow;
               return true;
            }
            if (second == 0xb6 || second == 0xbe)
            {
               uint8_t value;
               if (!ReadRM8(operand, &value, error)) return false;
               m_state.reg[regField] = second == 0xbe
                  ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value)))
                  : value;
               return true;
            }
            uint16_t value;
            if (!ReadRM16(operand, &value, error)) return false;
            m_state.reg[regField] = second == 0xbf
               ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)))
               : value;
            return true;
         }
         return FailOpcode(error, "unsupported IA-32 0F opcode", instructionAddress,
                           second);
      }
   }

   return FailOpcode(error, "unsupported IA-32 opcode", instructionAddress,
                     opcode);
}

cCpu::eRunResult cCpu::Run(uint32_t stopAddress, uint64_t instructionLimit,
                           std::string *error)
{
   for (uint64_t count = 0; count < instructionLimit; ++count)
   {
      if (m_state.eip == stopAddress)
      {
         if (error) error->clear();
         return kRunStopped;
      }
      const uint32_t instructionAddress = m_state.eip;
      if (!Step(error))
      {
         if (error && error->find("guest eip") == std::string::npos)
         {
            char context[192];
            uint32_t frame = m_state.reg[kEbp];
            uint32_t callers[4] = { 0, 0, 0, 0 };
            unsigned callerCount = 0;
            while (frame && callerCount < 4)
            {
               uint32_t next, caller;
               if (!m_memory.Read32(frame, &next, 0) ||
                   !m_memory.Read32(frame + 4, &caller, 0))
                  break;
               callers[callerCount++] = caller;
               if (next <= frame) break;
               frame = next;
            }
            snprintf(context, sizeof(context),
                     " at guest eip 0x%08x (eax=%08x ecx=%08x "
                     "ebp=%08x callers=%08x,%08x,%08x,%08x)",
                     instructionAddress, m_state.reg[kEax],
                     m_state.reg[kEcx], m_state.reg[kEbp], callers[0],
                     callers[1], callers[2], callers[3]);
            *error += context;
         }
         return kRunFault;
      }
   }
   if (error)
      *error = "guest instruction limit exceeded";
   return kRunLimit;
}

} // namespace osm32

// ---------------------------------------------------------------------------
// SSE and SSE2. A newer Visual C++ (NVScript's later builds: Reflections in
// Red's) compiles for SSE2 whatever CPUID says: its floating point is scalar
// SSE, its copies of 16 bytes MOVUPS. Its runtime's own SIMD paths (string
// functions, maths) are chosen by CPUID, which still says none: those stay
// the scalar ones. Floats are worked in double and rounded to float (exact
// for + - * / and the square root); a conversion out of range or of a NaN
// gives the integer indefinite, as the processor does.

namespace
{
inline float XmmF(const uint8_t *x, int i) { float f; memcpy(&f, x + 4 * i, 4); return f; }
inline void SetXmmF(uint8_t *x, int i, float f) { memcpy(x + 4 * i, &f, 4); }
inline double XmmD(const uint8_t *x, int i) { double d; memcpy(&d, x + 8 * i, 8); return d; }
inline void SetXmmD(uint8_t *x, int i, double d) { memcpy(x + 8 * i, &d, 8); }
inline uint16_t XmmU16(const uint8_t *x, int i) { uint16_t v; memcpy(&v, x + 2 * i, 2); return v; }
inline void SetXmmU16(uint8_t *x, int i, uint16_t v) { memcpy(x + 2 * i, &v, 2); }
inline uint32_t XmmU32(const uint8_t *x, int i) { uint32_t v; memcpy(&v, x + 4 * i, 4); return v; }
inline void SetXmmU32(uint8_t *x, int i, uint32_t v) { memcpy(x + 4 * i, &v, 4); }
inline uint64_t XmmU64(const uint8_t *x, int i) { uint64_t v; memcpy(&v, x + 8 * i, 8); return v; }
inline void SetXmmU64(uint8_t *x, int i, uint64_t v) { memcpy(x + 8 * i, &v, 8); }

int32_t SseToInt32(double value, bool truncate, uint32_t mxcsr)
{
   if (value != value)
      return std::numeric_limits<int32_t>::min();
   double rounded;
   if (truncate)
      rounded = std::trunc(value);
   else
      switch ((mxcsr >> 13) & 3)
      {
         case 0: rounded = std::nearbyint(value); break;   // (the host's: to nearest, even)
         case 1: rounded = std::floor(value); break;
         case 2: rounded = std::ceil(value); break;
         default: rounded = std::trunc(value); break;
      }
   if (rounded < -2147483648.0 || rounded > 2147483647.0)
      return std::numeric_limits<int32_t>::min();
   return static_cast<int32_t>(rounded);
}

double SseArith(uint8_t op, double a, double b)
{
   switch (op)
   {
      case 0x51: return std::sqrt(b);
      case 0x58: return a + b;
      case 0x59: return a * b;
      case 0x5c: return a - b;
      case 0x5d: return a < b ? a : b;   // (MIN: the second unless the first is less)
      case 0x5e: return a / b;
      default:   return a > b ? a : b;   // 0x5f MAX
   }
}

bool SseCompare(unsigned predicate, double a, double b)
{
   const bool unordered = a != a || b != b;
   switch (predicate & 7)
   {
      case 0: return !unordered && a == b;
      case 1: return !unordered && a < b;
      case 2: return !unordered && a <= b;
      case 3: return unordered;
      case 4: return unordered || a != b;
      case 5: return unordered || !(a < b);
      case 6: return unordered || !(a <= b);
      default: return !unordered;
   }
}
} // namespace

namespace osm32
{

bool cCpu::ExecuteSse(unsigned prefix, uint32_t instructionAddress, bool *handled,
                      std::string *error)
{
   const uint32_t start = m_state.eip;   // after the 0F
   uint8_t op;
   if (!Fetch8(&op, error))
      return false;
   *handled = true;
   uint8_t (*X)[16] = m_state.xmm;

   auto read = [&](uint32_t address, uint8_t *out, unsigned bytes) -> bool {
      for (unsigned i = 0; i < bytes; i += 4)
      {
         uint32_t v;
         if (bytes - i >= 4)
         {
            if (!m_memory.Read32(address + i, &v, error)) return false;
            memcpy(out + i, &v, 4);
         }
         else
         {
            uint16_t w;
            if (!m_memory.Read16(address + i, &w, error)) return false;
            memcpy(out + i, &w, 2);
         }
      }
      return true;
   };
   auto write = [&](uint32_t address, const uint8_t *in, unsigned bytes) -> bool {
      for (unsigned i = 0; i < bytes; i += 4)
      {
         uint32_t v;
         memcpy(&v, in + i, 4);
         if (!m_memory.Write32(address + i, v, error)) return false;
      }
      return true;
   };
   // the source operand: an XMM register (all of it), or 'bytes' from memory
   // (the rest zero)
   auto source = [&](const sDecodedRM &o, uint8_t *out, unsigned bytes) -> bool {
      if (o.isRegister)
      {
         memcpy(out, X[o.reg], 16);
         return true;
      }
      memset(out, 0, 16);
      return read(o.address, out, bytes);
   };
   auto modrm = [&](sDecodedRM *o, unsigned *r) -> bool {
      uint8_t m;
      return Fetch8(&m, error) && DecodeRM(m, o, r, error);
   };
   auto notMine = [&]() -> bool {
      m_state.eip = start;
      *handled = false;
      return true;
   };
   auto fail = [&]() -> bool {
      return FailOpcode(error, prefix == 0x66 ? "unsupported SSE opcode (66 0F)" :
                               prefix == 0xf3 ? "unsupported SSE opcode (F3 0F)" :
                               prefix == 0xf2 ? "unsupported SSE opcode (F2 0F)" :
                                                "unsupported SSE opcode (0F)",
                        instructionAddress, op);
   };

   sDecodedRM o;
   unsigned r;
   uint8_t s[16];

   // (not SSE, but in the same code: a multi-byte NOP, prefetches, CMOVcc,
   // SHLD/SHRD)
   if (op == 0x1f || op == 0x18)
      return modrm(&o, &r);
   if (op >= 0x40 && op <= 0x4f)
   {
      if (prefix) return notMine();
      uint32_t value;
      if (!modrm(&o, &r) || !ReadRM32(o, &value, error)) return false;
      if (Condition(op - 0x40)) m_state.reg[r] = value;
      return true;
   }
   if (op == 0xa4 || op == 0xa5 || op == 0xac || op == 0xad)
   {
      if (prefix) return notMine();
      uint32_t value;
      if (!modrm(&o, &r) || !ReadRM32(o, &value, error)) return false;
      uint8_t count8 = 0;
      if ((op == 0xa4 || op == 0xac) && !Fetch8(&count8, error)) return false;
      if (op == 0xa5 || op == 0xad) count8 = ReadReg8(1);
      const unsigned count = count8 & 31;
      if (!count) return true;
      const uint32_t other = m_state.reg[r];
      uint32_t result;
      bool carry;
      if (op == 0xa4 || op == 0xa5)
      {
         result = (value << count) | (other >> (32 - count));
         carry = ((value >> (32 - count)) & 1) != 0;
      }
      else
      {
         result = (value >> count) | (other << (32 - count));
         carry = ((value >> (count - 1)) & 1) != 0;
      }
      SetLogicFlags(result);
      if (carry) m_state.eflags |= kFlagCarry;
      return WriteRM32(o, result, error);
   }

   switch (op)
   {
      case 0x10: case 0x11:   // MOVUPS/MOVUPD; MOVSS; MOVSD
      {
         if (!modrm(&o, &r)) return false;
         const unsigned bytes = prefix == 0xf3 ? 4 : prefix == 0xf2 ? 8 : 16;
         if (op == 0x10)
         {
            if (!source(o, s, bytes)) return false;
            if (bytes == 16 || !o.isRegister)
               memcpy(X[r], s, 16);   // (from memory, a scalar zeroes the rest)
            else
               memcpy(X[r], s, bytes);
            return true;
         }
         if (o.isRegister)
         {
            memcpy(X[o.reg], X[r], bytes);
            return true;
         }
         return write(o.address, X[r], bytes);
      }
      case 0x12: case 0x13: case 0x16: case 0x17:
      {
         // MOVLPS/MOVLPD (12, 13), MOVHPS/MOVHPD (16, 17); MOVHLPS, MOVLHPS
         // (12, 16 from a register)
         if (prefix != 0 && prefix != 0x66) return fail();
         if (!modrm(&o, &r)) return false;
         const int half = (op == 0x12 || op == 0x13) ? 0 : 1;
         if (op == 0x12 || op == 0x16)
         {
            if (o.isRegister)
            {
               SetXmmU64(X[r], half, XmmU64(X[o.reg], 1 - half));
               return true;
            }
            uint8_t q[8];
            if (!read(o.address, q, 8)) return false;
            memcpy(X[r] + 8 * half, q, 8);
            return true;
         }
         if (o.isRegister) return fail();
         return write(o.address, X[r] + 8 * half, 8);
      }
      case 0x14: case 0x15:   // UNPCKLPS/UNPCKHPS, UNPCKLPD/UNPCKHPD
      {
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         uint8_t d[16];
         memcpy(d, X[r], 16);
         const int h = op == 0x15;
         if (prefix == 0x66)
         {
            SetXmmU64(X[r], 0, XmmU64(d, h));
            SetXmmU64(X[r], 1, XmmU64(s, h));
         }
         else
         {
            SetXmmU32(X[r], 0, XmmU32(d, 2 * h));
            SetXmmU32(X[r], 1, XmmU32(s, 2 * h));
            SetXmmU32(X[r], 2, XmmU32(d, 2 * h + 1));
            SetXmmU32(X[r], 3, XmmU32(s, 2 * h + 1));
         }
         return true;
      }
      case 0x28: case 0x29: case 0x2b:   // MOVAPS/MOVAPD, MOVNTPS/MOVNTPD
      {
         if (!modrm(&o, &r)) return false;
         if (op == 0x28)
         {
            if (!source(o, s, 16)) return false;
            memcpy(X[r], s, 16);
            return true;
         }
         if (o.isRegister)
         {
            memcpy(X[o.reg], X[r], 16);
            return true;
         }
         return write(o.address, X[r], 16);
      }
      case 0x2a:   // CVTSI2SS, CVTSI2SD
      {
         if (prefix != 0xf3 && prefix != 0xf2) return fail();
         uint32_t value;
         if (!modrm(&o, &r) || !ReadRM32(o, &value, error)) return false;
         const int32_t v = static_cast<int32_t>(value);
         if (prefix == 0xf3) SetXmmF(X[r], 0, static_cast<float>(v));
         else SetXmmD(X[r], 0, static_cast<double>(v));
         return true;
      }
      case 0x2c: case 0x2d:   // CVTTSS2SI, CVTSS2SI, CVTTSD2SI, CVTSD2SI
      {
         if (prefix != 0xf3 && prefix != 0xf2) return fail();
         if (!modrm(&o, &r) || !source(o, s, prefix == 0xf3 ? 4 : 8)) return false;
         const double v = prefix == 0xf3 ? static_cast<double>(XmmF(s, 0)) : XmmD(s, 0);
         m_state.reg[r] = static_cast<uint32_t>(SseToInt32(v, op == 0x2c, m_state.mxcsr));
         return true;
      }
      case 0x2e: case 0x2f:   // UCOMISS/COMISS, UCOMISD/COMISD
      {
         if (prefix != 0 && prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, prefix == 0x66 ? 8 : 4)) return false;
         const double a = prefix == 0x66 ? XmmD(X[r], 0) : static_cast<double>(XmmF(X[r], 0));
         const double b = prefix == 0x66 ? XmmD(s, 0) : static_cast<double>(XmmF(s, 0));
         m_state.eflags &= ~(kFlagZero | kFlagParity | kFlagCarry | kFlagOverflow | kFlagSign | kFlagAdjust);
         if (a != a || b != b)
            m_state.eflags |= kFlagZero | kFlagParity | kFlagCarry;
         else if (a < b)
            m_state.eflags |= kFlagCarry;
         else if (a == b)
            m_state.eflags |= kFlagZero;
         return true;
      }
      case 0x50:   // MOVMSKPS, MOVMSKPD
      {
         if (!modrm(&o, &r)) return false;
         if (!o.isRegister) return fail();
         uint32_t mask = 0;
         if (prefix == 0x66)
            for (int i = 0; i < 2; ++i) mask |= ((X[o.reg][8 * i + 7] >> 7) & 1u) << i;
         else
            for (int i = 0; i < 4; ++i) mask |= ((X[o.reg][4 * i + 3] >> 7) & 1u) << i;
         m_state.reg[r] = mask;
         return true;
      }
      case 0x51: case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
      {
         const unsigned bytes = prefix == 0xf3 ? 4 : prefix == 0xf2 ? 8 : 16;
         if (!modrm(&o, &r) || !source(o, s, bytes)) return false;
         uint8_t *d = X[r];
         if (prefix == 0xf3)
            SetXmmF(d, 0, static_cast<float>(SseArith(op, XmmF(d, 0), XmmF(s, 0))));
         else if (prefix == 0xf2)
            SetXmmD(d, 0, SseArith(op, XmmD(d, 0), XmmD(s, 0)));
         else if (prefix == 0x66)
            for (int i = 0; i < 2; ++i) SetXmmD(d, i, SseArith(op, XmmD(d, i), XmmD(s, i)));
         else
            for (int i = 0; i < 4; ++i)
               SetXmmF(d, i, static_cast<float>(SseArith(op, XmmF(d, i), XmmF(s, i))));
         return true;
      }
      case 0x54: case 0x55: case 0x56: case 0x57:   // AND, ANDN, OR, XOR (PS/PD)
      {
         if (prefix != 0 && prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         for (int i = 0; i < 16; ++i)
         {
            uint8_t &d = X[r][i];
            d = op == 0x54 ? (d & s[i]) : op == 0x55 ? static_cast<uint8_t>(~d & s[i]) :
                op == 0x56 ? (d | s[i]) : (d ^ s[i]);
         }
         return true;
      }
      case 0x5a:   // CVTPS2PD, CVTPD2PS, CVTSS2SD, CVTSD2SS
      {
         const unsigned bytes = prefix == 0xf3 ? 4 : prefix == 0xf2 || prefix == 0 ? 8 : 16;
         if (!modrm(&o, &r) || !source(o, s, bytes)) return false;
         uint8_t *d = X[r];
         if (prefix == 0)
         {
            const float a = XmmF(s, 0), b = XmmF(s, 1);
            SetXmmD(d, 0, a);
            SetXmmD(d, 1, b);
         }
         else if (prefix == 0x66)
         {
            const double a = XmmD(s, 0), b = XmmD(s, 1);
            SetXmmF(d, 0, static_cast<float>(a));
            SetXmmF(d, 1, static_cast<float>(b));
            SetXmmU64(d, 1, 0);
         }
         else if (prefix == 0xf3)
            SetXmmD(d, 0, static_cast<double>(XmmF(s, 0)));
         else
            SetXmmF(d, 0, static_cast<float>(XmmD(s, 0)));
         return true;
      }
      case 0x5b:   // CVTDQ2PS, CVTPS2DQ (66), CVTTPS2DQ (F3)
      {
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         for (int i = 0; i < 4; ++i)
         {
            if (prefix == 0)
               SetXmmF(X[r], i, static_cast<float>(static_cast<int32_t>(XmmU32(s, i))));
            else
               SetXmmU32(X[r], i, static_cast<uint32_t>(SseToInt32(XmmF(s, i), prefix == 0xf3, m_state.mxcsr)));
         }
         return true;
      }
      case 0xe6:   // CVTDQ2PD (F3), CVTTPD2DQ (66), CVTPD2DQ (F2)
      {
         if (prefix == 0) return fail();
         if (!modrm(&o, &r) || !source(o, s, prefix == 0xf3 ? 8 : 16)) return false;
         if (prefix == 0xf3)
         {
            const int32_t a = static_cast<int32_t>(XmmU32(s, 0)), b = static_cast<int32_t>(XmmU32(s, 1));
            SetXmmD(X[r], 0, a);
            SetXmmD(X[r], 1, b);
         }
         else
         {
            for (int i = 0; i < 2; ++i)
               SetXmmU32(X[r], i, static_cast<uint32_t>(SseToInt32(XmmD(s, i), prefix == 0x66, m_state.mxcsr)));
            SetXmmU64(X[r], 1, 0);
         }
         return true;
      }
      case 0x6e:   // MOVD xmm, r/m32
      {
         if (prefix != 0x66) return fail();
         uint32_t value;
         if (!modrm(&o, &r) || !ReadRM32(o, &value, error)) return false;
         memset(X[r], 0, 16);
         SetXmmU32(X[r], 0, value);
         return true;
      }
      case 0x7e:   // MOVD r/m32, xmm (66); MOVQ xmm, xmm/m64 (F3)
      {
         if (!modrm(&o, &r)) return false;
         if (prefix == 0x66)
            return WriteRM32(o, XmmU32(X[r], 0), error);
         if (prefix == 0xf3)
         {
            if (!source(o, s, 8)) return false;
            SetXmmU64(X[r], 0, XmmU64(s, 0));
            SetXmmU64(X[r], 1, 0);
            return true;
         }
         return fail();
      }
      case 0xd6:   // MOVQ xmm/m64, xmm
      {
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r)) return false;
         if (o.isRegister)
         {
            SetXmmU64(X[o.reg], 0, XmmU64(X[r], 0));
            SetXmmU64(X[o.reg], 1, 0);
            return true;
         }
         return write(o.address, X[r], 8);
      }
      case 0x6f: case 0x7f:   // MOVDQA (66), MOVDQU (F3)
      {
         if (prefix != 0x66 && prefix != 0xf3) return fail();
         if (!modrm(&o, &r)) return false;
         if (op == 0x6f)
         {
            if (!source(o, s, 16)) return false;
            memcpy(X[r], s, 16);
            return true;
         }
         if (o.isRegister)
         {
            memcpy(X[o.reg], X[r], 16);
            return true;
         }
         return write(o.address, X[r], 16);
      }
      case 0x70:   // PSHUFD (66), PSHUFLW (F2), PSHUFHW (F3)
      {
         if (prefix == 0) return fail();
         uint8_t imm;
         if (!modrm(&o, &r) || !source(o, s, 16) || !Fetch8(&imm, error)) return false;
         uint8_t d[16];
         memcpy(d, s, 16);
         if (prefix == 0x66)
            for (int i = 0; i < 4; ++i) SetXmmU32(d, i, XmmU32(s, (imm >> (2 * i)) & 3));
         else
         {
            const int base = prefix == 0xf2 ? 0 : 4;
            for (int i = 0; i < 4; ++i) SetXmmU16(d, base + i, XmmU16(s, base + ((imm >> (2 * i)) & 3)));
         }
         memcpy(X[r], d, 16);
         return true;
      }
      case 0x71: case 0x72: case 0x73:   // shifts by an immediate
      {
         if (prefix != 0x66) return fail();
         uint8_t imm;
         if (!modrm(&o, &r)) return false;
         if (!o.isRegister) return fail();
         if (!Fetch8(&imm, error)) return false;
         uint8_t *d = X[o.reg];
         const unsigned n = imm;
         if (op == 0x73 && (r == 3 || r == 7))
         {
            // PSRLDQ, PSLLDQ: bytes
            uint8_t t[16] = {};
            for (int i = 0; i < 16; ++i)
            {
               const int from = r == 3 ? i + static_cast<int>(n) : i - static_cast<int>(n);
               if (from >= 0 && from < 16) t[i] = d[from];
            }
            memcpy(d, t, 16);
            return true;
         }
         const int width = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
         for (int i = 0; i < 128 / width; ++i)
         {
            uint64_t v = width == 16 ? XmmU16(d, i) : width == 32 ? XmmU32(d, i) : XmmU64(d, i);
            uint64_t result;
            if (r == 2)
               result = n >= static_cast<unsigned>(width) ? 0 : v >> n;
            else if (r == 6)
               result = n >= static_cast<unsigned>(width) ? 0 : v << n;
            else if (r == 4 && width != 64)
            {
               const int64_t signedValue = width == 16 ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
               result = static_cast<uint64_t>(signedValue >> (n >= static_cast<unsigned>(width) ? width - 1 : n));
            }
            else
               return fail();
            if (width == 16) SetXmmU16(d, i, static_cast<uint16_t>(result));
            else if (width == 32) SetXmmU32(d, i, static_cast<uint32_t>(result));
            else SetXmmU64(d, i, result);
         }
         return true;
      }
      case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2: case 0xf1: case 0xf2: case 0xf3:
      {
         // shifts by a register's count
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         const uint64_t n = XmmU64(s, 0);
         const int width = (op & 3) == 1 ? 16 : (op & 3) == 2 ? 32 : 64;
         uint8_t *d = X[r];
         for (int i = 0; i < 128 / width; ++i)
         {
            uint64_t v = width == 16 ? XmmU16(d, i) : width == 32 ? XmmU32(d, i) : XmmU64(d, i);
            uint64_t result;
            if ((op & 0xf0) == 0xd0)
               result = n >= static_cast<uint64_t>(width) ? 0 : v >> n;
            else if ((op & 0xf0) == 0xf0)
               result = n >= static_cast<uint64_t>(width) ? 0 : v << n;
            else
            {
               const int64_t signedValue = width == 16 ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
               result = static_cast<uint64_t>(signedValue >> (n >= static_cast<uint64_t>(width) ? width - 1 : n));
            }
            if (width == 16) SetXmmU16(d, i, static_cast<uint16_t>(result));
            else if (width == 32) SetXmmU32(d, i, static_cast<uint32_t>(result));
            else SetXmmU64(d, i, result);
         }
         return true;
      }
      case 0x60: case 0x61: case 0x62: case 0x6c: case 0x68: case 0x69: case 0x6a: case 0x6d:
      {
         // PUNPCKL/H BW, WD, DQ, QDQ
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         const int size = (op == 0x60 || op == 0x68) ? 1 : (op == 0x61 || op == 0x69) ? 2 :
                          (op == 0x62 || op == 0x6a) ? 4 : 8;
         const int high = op >= 0x68 && op != 0x6c;
         uint8_t d[16];
         const uint8_t *a = X[r];
         for (int i = 0; i < 16 / (2 * size); ++i)
         {
            memcpy(d + (2 * i) * size, a + (high * 8) + i * size, size);
            memcpy(d + (2 * i + 1) * size, s + (high * 8) + i * size, size);
         }
         memcpy(X[r], d, 16);
         return true;
      }
      case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76:
      {
         // PCMPGT B/W/D, PCMPEQ B/W/D
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         const int size = 1 << ((op & 3) == 0 ? 0 : (op & 3) == 1 ? 1 : 2);
         const bool equal = op >= 0x74;
         uint8_t *d = X[r];
         for (int i = 0; i < 16 / size; ++i)
         {
            int64_t a = 0, b = 0;
            if (size == 1) a = static_cast<int8_t>(d[i]), b = static_cast<int8_t>(s[i]);
            else if (size == 2) a = static_cast<int16_t>(XmmU16(d, i)), b = static_cast<int16_t>(XmmU16(s, i));
            else a = static_cast<int32_t>(XmmU32(d, i)), b = static_cast<int32_t>(XmmU32(s, i));
            const bool yes = equal ? a == b : a > b;
            memset(d + i * size, yes ? 0xff : 0, size);
         }
         return true;
      }
      case 0xc2:   // CMPPS, CMPPD, CMPSS, CMPSD
      {
         const unsigned bytes = prefix == 0xf3 ? 4 : prefix == 0xf2 ? 8 : 16;
         uint8_t predicate;
         if (!modrm(&o, &r) || !source(o, s, bytes) || !Fetch8(&predicate, error)) return false;
         uint8_t *d = X[r];
         if (prefix == 0xf3)
            SetXmmU32(d, 0, SseCompare(predicate, XmmF(d, 0), XmmF(s, 0)) ? 0xffffffffu : 0);
         else if (prefix == 0xf2)
            SetXmmU64(d, 0, SseCompare(predicate, XmmD(d, 0), XmmD(s, 0)) ? ~0ull : 0);
         else if (prefix == 0x66)
            for (int i = 0; i < 2; ++i) SetXmmU64(d, i, SseCompare(predicate, XmmD(d, i), XmmD(s, i)) ? ~0ull : 0);
         else
            for (int i = 0; i < 4; ++i) SetXmmU32(d, i, SseCompare(predicate, XmmF(d, i), XmmF(s, i)) ? 0xffffffffu : 0);
         return true;
      }
      case 0xc4:   // PINSRW xmm, r32/m16, imm8
      {
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r)) return false;
         uint16_t value;
         if (o.isRegister) value = static_cast<uint16_t>(m_state.reg[o.reg]);
         else if (!m_memory.Read16(o.address, &value, error)) return false;
         uint8_t imm;
         if (!Fetch8(&imm, error)) return false;
         SetXmmU16(X[r], imm & 7, value);
         return true;
      }
      case 0xc5:   // PEXTRW r32, xmm, imm8
      {
         if (prefix != 0x66) return fail();
         uint8_t imm;
         if (!modrm(&o, &r)) return false;
         if (!o.isRegister) return fail();
         if (!Fetch8(&imm, error)) return false;
         m_state.reg[r] = XmmU16(X[o.reg], imm & 7);
         return true;
      }
      case 0xc6:   // SHUFPS, SHUFPD
      {
         uint8_t imm;
         if (!modrm(&o, &r) || !source(o, s, 16) || !Fetch8(&imm, error)) return false;
         uint8_t d[16];
         const uint8_t *a = X[r];
         if (prefix == 0x66)
         {
            SetXmmU64(d, 0, XmmU64(a, imm & 1));
            SetXmmU64(d, 1, XmmU64(s, (imm >> 1) & 1));
         }
         else
         {
            SetXmmU32(d, 0, XmmU32(a, imm & 3));
            SetXmmU32(d, 1, XmmU32(a, (imm >> 2) & 3));
            SetXmmU32(d, 2, XmmU32(s, (imm >> 4) & 3));
            SetXmmU32(d, 3, XmmU32(s, (imm >> 6) & 3));
         }
         memcpy(X[r], d, 16);
         return true;
      }
      case 0xd7:   // PMOVMSKB r32, xmm
      {
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r)) return false;
         if (!o.isRegister) return fail();
         uint32_t mask = 0;
         for (int i = 0; i < 16; ++i) mask |= ((X[o.reg][i] >> 7) & 1u) << i;
         m_state.reg[r] = mask;
         return true;
      }
      case 0xdb: case 0xdf: case 0xeb: case 0xef:   // PAND, PANDN, POR, PXOR
      case 0xd4: case 0xfb: case 0xfe: case 0xfa: case 0xfc: case 0xfd: case 0xf8: case 0xf9:
      case 0xea: case 0xee: case 0xda: case 0xde:
      {
         if (prefix != 0x66) return fail();
         if (!modrm(&o, &r) || !source(o, s, 16)) return false;
         uint8_t *d = X[r];
         switch (op)
         {
            case 0xdb: for (int i = 0; i < 16; ++i) d[i] &= s[i]; break;
            case 0xdf: for (int i = 0; i < 16; ++i) d[i] = static_cast<uint8_t>(~d[i] & s[i]); break;
            case 0xeb: for (int i = 0; i < 16; ++i) d[i] |= s[i]; break;
            case 0xef: for (int i = 0; i < 16; ++i) d[i] ^= s[i]; break;
            case 0xd4: for (int i = 0; i < 2; ++i) SetXmmU64(d, i, XmmU64(d, i) + XmmU64(s, i)); break;
            case 0xfb: for (int i = 0; i < 2; ++i) SetXmmU64(d, i, XmmU64(d, i) - XmmU64(s, i)); break;
            case 0xfe: for (int i = 0; i < 4; ++i) SetXmmU32(d, i, XmmU32(d, i) + XmmU32(s, i)); break;
            case 0xfa: for (int i = 0; i < 4; ++i) SetXmmU32(d, i, XmmU32(d, i) - XmmU32(s, i)); break;
            case 0xfd: for (int i = 0; i < 8; ++i) SetXmmU16(d, i, static_cast<uint16_t>(XmmU16(d, i) + XmmU16(s, i))); break;
            case 0xf9: for (int i = 0; i < 8; ++i) SetXmmU16(d, i, static_cast<uint16_t>(XmmU16(d, i) - XmmU16(s, i))); break;
            case 0xfc: for (int i = 0; i < 16; ++i) d[i] = static_cast<uint8_t>(d[i] + s[i]); break;
            case 0xf8: for (int i = 0; i < 16; ++i) d[i] = static_cast<uint8_t>(d[i] - s[i]); break;
            case 0xea: case 0xee:
               for (int i = 0; i < 8; ++i)
               {
                  const int16_t a = static_cast<int16_t>(XmmU16(d, i)), b = static_cast<int16_t>(XmmU16(s, i));
                  SetXmmU16(d, i, static_cast<uint16_t>(op == 0xea ? std::min(a, b) : std::max(a, b)));
               }
               break;
            default:   // 0xda PMINUB, 0xde PMAXUB
               for (int i = 0; i < 16; ++i) d[i] = op == 0xda ? std::min(d[i], s[i]) : std::max(d[i], s[i]);
               break;
         }
         return true;
      }
      case 0xae:   // LDMXCSR, STMXCSR; the fences
      {
         if (prefix) return notMine();
         if (!modrm(&o, &r)) return false;
         if (o.isRegister)
            return r >= 5 ? true : fail();
         if (r == 2)
            return m_memory.Read32(o.address, &m_state.mxcsr, error);
         if (r == 3)
            return m_memory.Write32(o.address, m_state.mxcsr, error);
         return fail();
      }
      default:
         return notMine();
   }
}

} // namespace osm32

namespace
{
// 80-bit extended <-> double (FLD/FSTP m80, FSAVE/FRSTOR)
void DoubleToExtended(double value, uint8_t out[10])
{
   uint64_t bits;
   memcpy(&bits, &value, 8);
   const unsigned sign = static_cast<unsigned>(bits >> 63);
   const unsigned exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
   uint64_t fraction = bits & 0x000fffffffffffffULL;
   uint64_t mantissa = 0;
   unsigned e = 0;
   if (exponent == 0x7ff)
   {
      e = 0x7fff;
      mantissa = 0x8000000000000000ULL | (fraction << 11);
   }
   else if (exponent == 0 && fraction == 0)
      ;
   else if (exponent == 0)   // subnormal double: normal as extended
   {
      int shift = 0;
      mantissa = fraction << 11;
      while (!(mantissa & 0x8000000000000000ULL))
      {
         mantissa <<= 1;
         ++shift;
      }
      e = static_cast<unsigned>(16383 - 1022 - shift);
   }
   else
   {
      e = exponent - 1023 + 16383;
      mantissa = 0x8000000000000000ULL | (fraction << 11);
   }
   memcpy(out, &mantissa, 8);
   const uint16_t top = static_cast<uint16_t>(e | (sign << 15));
   memcpy(out + 8, &top, 2);
}

double ExtendedToDouble(const uint8_t in[10])
{
   uint64_t mantissa;
   uint16_t top;
   memcpy(&mantissa, in, 8);
   memcpy(&top, in + 8, 2);
   const bool negative = (top & 0x8000) != 0;
   const int e = top & 0x7fff;
   double value;
   if (e == 0x7fff)
      value = (mantissa << 1) ? std::numeric_limits<double>::quiet_NaN() :
                                std::numeric_limits<double>::infinity();
   else
      value = std::ldexp(static_cast<double>(mantissa), e - 16383 - 63);
   return negative ? -value : value;
}

double X87Round(double value, uint16_t control)
{
   switch ((control >> 10) & 3)
   {
      case 1: return std::floor(value);
      case 2: return std::ceil(value);
      case 3: return std::trunc(value);
      default: return std::nearbyint(value);
   }
}
} // namespace

namespace osm32
{

bool cCpu::ExecuteX87(uint8_t opcode, uint32_t instructionAddress, bool *handled,
                      std::string *error)
{
   const uint32_t start = m_state.eip;   // after the D8-DF byte
   uint8_t second;
   if (!Fetch8(&second, error))
      return false;
   *handled = true;
   double *st = m_state.x87;
   unsigned &depth = m_state.x87Depth;

   auto notMine = [&]() -> bool {
      m_state.eip = start;
      *handled = false;
      return true;
   };
   auto need = [&](unsigned count) -> bool {
      if (depth < count)
      {
         FailAddress(error, "x87 stack underflow at", instructionAddress);
         return false;
      }
      return true;
   };
   auto push = [&](double value) -> bool {
      if (depth == 8)
      {
         FailAddress(error, "x87 stack overflow at", instructionAddress);
         return false;
      }
      for (unsigned i = depth; i != 0; --i)
         st[i] = st[i - 1];
      st[0] = value;
      ++depth;
      return true;
   };
   auto pop = [&]() {
      for (unsigned i = 1; i < depth; ++i)
         st[i - 1] = st[i];
      --depth;
   };
   // FCOM-style condition codes in the status word
   auto compareStatus = [&](double a, double b) {
      m_state.x87Status &= ~0x4700u;
      if (std::isnan(a) || std::isnan(b))
         m_state.x87Status |= 0x4500u;
      else if (a < b)
         m_state.x87Status |= 0x0100u;
      else if (a == b)
         m_state.x87Status |= 0x4000u;
   };
   // FCOMI-style: ZF PF CF in EFLAGS
   auto compareFlags = [&](double a, double b) {
      m_state.eflags &= ~(kFlagZero | kFlagParity | kFlagCarry |
                          kFlagOverflow | kFlagSign | kFlagAdjust);
      if (std::isnan(a) || std::isnan(b))
         m_state.eflags |= kFlagZero | kFlagParity | kFlagCarry;
      else if (a < b)
         m_state.eflags |= kFlagCarry;
      else if (a == b)
         m_state.eflags |= kFlagZero;
   };
   auto clearC2 = [&]() { m_state.x87Status &= ~0x0400u; };
   // FISTTP: truncate whatever the control word says
   auto fisttp = [&](uint32_t address, unsigned bytes) -> bool {
      if (!need(1)) return false;
      const double t = std::trunc(st[0]);
      bool ok;
      if (bytes == 2)
      {
         const uint16_t v = std::isnan(t) || t < -32768.0 || t > 32767.0 ?
            0x8000u : static_cast<uint16_t>(static_cast<int16_t>(t));
         ok = m_memory.Write16(address, v, error);
      }
      else if (bytes == 4)
      {
         const uint32_t v = std::isnan(t) || t < -2147483648.0 || t > 2147483647.0 ?
            0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(t));
         ok = m_memory.Write32(address, v, error);
      }
      else
      {
         const uint64_t v = std::isnan(t) || t < -9223372036854775808.0 ||
                            t >= 9223372036854775808.0 ? 0x8000000000000000ULL :
            static_cast<uint64_t>(static_cast<int64_t>(t));
         ok = m_memory.Write64(address, v, error);
      }
      if (!ok) return false;
      pop();
      return true;
   };
   // the 28-byte protected-mode environment: control, status (with TOP),
   // tag word; the instruction and operand pointers are zero
   auto storeEnvironment = [&](uint32_t address) -> bool {
      const unsigned top = (8 - depth) & 7;
      uint16_t tags = 0;
      for (unsigned physical = 0; physical < 8; ++physical)
      {
         const unsigned i = (physical - top) & 7;   // its ST(i)
         if (i >= depth)
            tags |= static_cast<uint16_t>(3u << (physical * 2));
      }
      const uint32_t words[7] = {
         0xffff0000u | m_state.x87Control,
         0xffff0000u | static_cast<uint32_t>((m_state.x87Status & ~0x3800u) | (top << 11)),
         0xffff0000u | tags, 0, 0, 0, 0 };
      for (unsigned i = 0; i < 7; ++i)
         if (!m_memory.Write32(address + i * 4, words[i], error))
            return false;
      return true;
   };
   auto loadEnvironment = [&](uint32_t address, bool takeDepth) -> bool {
      uint32_t control, status, tags;
      if (!m_memory.Read32(address, &control, error) ||
          !m_memory.Read32(address + 4, &status, error) ||
          !m_memory.Read32(address + 8, &tags, error))
         return false;
      m_state.x87Control = static_cast<uint16_t>(control);
      m_state.x87Status = static_cast<uint16_t>(status & ~0x3800u);
      if (takeDepth)
      {
         const unsigned top = (status >> 11) & 7;
         unsigned count = 0;
         while (count < 8 &&
                ((tags >> (((top + count) & 7) * 2)) & 3) != 3)
            ++count;
         depth = count;
      }
      return true;
   };

   if (second >= 0xc0)
   {
      const unsigned i = second & 7u;
      switch (opcode)
      {
         case 0xd9:
         {
            static const double kLog2Of10 = 3.321928094887362347870;
            static const double kLog2OfE = 1.442695040888963407360;
            static const double kPi = 3.141592653589793238463;
            static const double kLog10Of2 = 0.301029995663981195214;
            static const double kLnOf2 = 0.693147180559945309417;
            switch (second)
            {
               case 0xd0: return true;                               // FNOP
               case 0xe9: return push(kLog2Of10);                    // FLDL2T
               case 0xea: return push(kLog2OfE);                     // FLDL2E
               case 0xeb: return push(kPi);                          // FLDPI
               case 0xec: return push(kLog10Of2);                    // FLDLG2
               case 0xed: return push(kLnOf2);                       // FLDLN2
               case 0xf0:                                            // F2XM1
                  if (!need(1)) return false;
                  st[0] = std::expm1(st[0] * kLnOf2);
                  return true;
               case 0xf1:                                            // FYL2X
                  if (!need(2)) return false;
                  st[1] = st[1] * std::log2(st[0]);
                  pop();
                  return true;
               case 0xf2:                                            // FPTAN
                  if (!need(1)) return false;
                  st[0] = std::tan(st[0]);
                  clearC2();
                  return push(1.0);
               case 0xf3:                                            // FPATAN
                  if (!need(2)) return false;
                  st[1] = std::atan2(st[1], st[0]);
                  pop();
                  return true;
               case 0xf4:                                            // FXTRACT
               {
                  if (!need(1)) return false;
                  int e = 0;
                  const double m = std::frexp(st[0], &e);
                  if (st[0] == 0.0)
                  {
                     const double z = st[0];
                     st[0] = -std::numeric_limits<double>::infinity();
                     return push(z);
                  }
                  st[0] = static_cast<double>(e - 1);
                  return push(m * 2.0);
               }
               case 0xf5:                                            // FPREM1
               case 0xf8:                                            // FPREM
               {
                  if (!need(2)) return false;
                  const double a = st[0], b = st[1];
                  const double q = second == 0xf8 ? std::trunc(a / b) :
                                                    std::nearbyint(a / b);
                  st[0] = second == 0xf8 ? std::fmod(a, b) :
                                           std::remainder(a, b);
                  const uint64_t qi = std::isfinite(q) ?
                     static_cast<uint64_t>(std::fabs(std::fmod(q, 8.0))) : 0;
                  m_state.x87Status &= ~0x4700u;
                  if (qi & 1) m_state.x87Status |= 0x0200u;   // C1 = Q0
                  if (qi & 2) m_state.x87Status |= 0x4000u;   // C3 = Q1
                  if (qi & 4) m_state.x87Status |= 0x0100u;   // C0 = Q2
                  return true;
               }
               case 0xf9:                                            // FYL2XP1
                  if (!need(2)) return false;
                  st[1] = st[1] * std::log1p(st[0]) / kLnOf2;
                  pop();
                  return true;
               case 0xfb:                                            // FSINCOS
               {
                  if (!need(1)) return false;
                  const double x = st[0];
                  st[0] = std::sin(x);
                  clearC2();
                  return push(std::cos(x));
               }
               case 0xfc:                                            // FRNDINT
                  if (!need(1)) return false;
                  st[0] = X87Round(st[0], m_state.x87Control);
                  return true;
               case 0xfd:                                            // FSCALE
               {
                  if (!need(2)) return false;
                  const double s = std::trunc(st[1]);
                  if (std::isnan(s))
                     st[0] = s;
                  else
                     st[0] = std::ldexp(st[0], static_cast<int>(
                        std::max(-100000.0, std::min(100000.0, s))));
                  return true;
               }
               case 0xfe:                                            // FSIN
                  if (!need(1)) return false;
                  st[0] = std::sin(st[0]);
                  clearC2();
                  return true;
               case 0xff:                                            // FCOS
                  if (!need(1)) return false;
                  st[0] = std::cos(st[0]);
                  clearC2();
                  return true;
            }
            return notMine();
         }
         case 0xda:
         case 0xdb:
         {
            if (opcode == 0xda && second == 0xe9)                    // FUCOMPP
            {
               if (!need(2)) return false;
               compareStatus(st[0], st[1]);
               pop();
               pop();
               return true;
            }
            if (second < 0xe0)                                       // FCMOVcc
            {
               const uint32_t f = m_state.eflags;
               bool condition;
               switch ((second >> 3) & 3)
               {
                  case 0: condition = (f & kFlagCarry) != 0; break;
                  case 1: condition = (f & kFlagZero) != 0; break;
                  case 2: condition = (f & (kFlagCarry | kFlagZero)) != 0; break;
                  default: condition = (f & kFlagParity) != 0; break;
               }
               if (opcode == 0xdb)
                  condition = !condition;
               if (!need(i + 1)) return false;
               if (condition)
                  st[0] = st[i];
               return true;
            }
            if (opcode == 0xdb && second >= 0xe8 && second < 0xf8)   // FUCOMI/FCOMI
            {
               if (!need(i + 1)) return false;
               compareFlags(st[0], st[i]);
               return true;
            }
            return notMine();
         }
         case 0xdd:
         {
            const unsigned group = (second >> 3) & 7;
            if (group == 2 || group == 3)                            // FST/FSTP ST(i)
            {
               if (!need(1)) return false;
               if (i >= depth)
                  return FailAddress(error, "x87 store to an empty register at",
                                     instructionAddress);
               st[i] = st[0];
               if (group == 3)
                  pop();
               return true;
            }
            if (group == 4 || group == 5)                            // FUCOM/FUCOMP ST(i)
            {
               if (!need(i + 1)) return false;
               compareStatus(st[0], st[i]);
               if (group == 5)
                  pop();
               return true;
            }
            return notMine();
         }
         case 0xdf:
            if (second >= 0xe8 && second < 0xf8)                     // FUCOMIP/FCOMIP
            {
               if (!need(i + 1)) return false;
               compareFlags(st[0], st[i]);
               pop();
               return true;
            }
            return notMine();
         case 0xdc:
            if (second >= 0xd0 && second < 0xe0)                     // FCOM/FCOMP (DC alias)
            {
               if (!need(i + 1)) return false;
               compareStatus(st[0], st[i]);
               if (second >= 0xd8)
                  pop();
               return true;
            }
            return notMine();
      }
      return notMine();
   }

   sDecodedRM operand;
   unsigned operation;
   if (!DecodeRM(second, &operand, &operation, error))
      return false;
   const uint32_t address = operand.address;
   switch (opcode)
   {
      case 0xd9:
         if (operation == 4)                                         // FLDENV
            return loadEnvironment(address, false);
         if (operation == 6)                                         // FNSTENV
         {
            if (!storeEnvironment(address)) return false;
            m_state.x87Control |= 0x3f;   // and masks every exception
            return true;
         }
         break;
      case 0xdb:
         if (operation == 1)                                         // FISTTP m32
            return fisttp(address, 4);
         if (operation == 5)                                         // FLD m80
         {
            uint8_t bytes[10];
            for (unsigned b = 0; b < 10; ++b)
               if (!m_memory.Read8(address + b, &bytes[b], error)) return false;
            return push(ExtendedToDouble(bytes));
         }
         if (operation == 7)                                         // FSTP m80
         {
            if (!need(1)) return false;
            uint8_t bytes[10];
            DoubleToExtended(st[0], bytes);
            for (unsigned b = 0; b < 10; ++b)
               if (!m_memory.Write8(address + b, bytes[b], error)) return false;
            pop();
            return true;
         }
         break;
      case 0xdd:
         if (operation == 1)                                         // FISTTP m64
            return fisttp(address, 8);
         if (operation == 4)                                         // FRSTOR
         {
            if (!loadEnvironment(address, true)) return false;
            for (unsigned r = 0; r < 8; ++r)
            {
               uint8_t bytes[10];
               for (unsigned b = 0; b < 10; ++b)
                  if (!m_memory.Read8(address + 28 + r * 10 + b, &bytes[b], error))
                     return false;
               st[r] = ExtendedToDouble(bytes);
            }
            return true;
         }
         if (operation == 6)                                         // FNSAVE
         {
            if (!storeEnvironment(address)) return false;
            for (unsigned r = 0; r < 8; ++r)
            {
               uint8_t bytes[10];
               DoubleToExtended(r < depth ? st[r] : 0.0, bytes);
               for (unsigned b = 0; b < 10; ++b)
                  if (!m_memory.Write8(address + 28 + r * 10 + b, bytes[b], error))
                     return false;
            }
            depth = 0;   // and FNINIT
            m_state.x87Status = 0;
            m_state.x87Control = 0x037f;
            return true;
         }
         break;
      case 0xdf:
         if (operation == 1)                                         // FISTTP m16
            return fisttp(address, 2);
         break;
   }
   return notMine();
}

} // namespace osm32
