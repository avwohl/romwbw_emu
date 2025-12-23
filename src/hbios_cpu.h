/*
 * HBIOS CPU - Shared Z80 CPU subclass for RomWBW emulation
 *
 * This class provides the port I/O handlers for RomWBW HBIOS.
 * It delegates HBIOS function handling to HBIOSDispatch.
 */

#ifndef HBIOS_CPU_H
#define HBIOS_CPU_H

#include "qkz80.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"

// Interface that emulator must implement to receive callbacks
class HBIOSCPUDelegate {
public:
  virtual ~HBIOSCPUDelegate() = default;

  // Get the memory system
  virtual banked_mem* getMemory() = 0;

  // Get the HBIOSDispatch instance
  virtual HBIOSDispatch* getHBIOS() = 0;

  // Initialize a RAM bank if first access (for CP/M 3 direct bank switching)
  virtual void initializeRamBankIfNeeded(uint8_t bank) = 0;

  // Handle CPU halt
  virtual void onHalt() = 0;

  // Handle unimplemented opcode
  virtual void onUnimplementedOpcode(uint8_t opcode, uint16_t pc) = 0;

  // Debug logging (optional, can be no-op)
  virtual void logDebug(const char* fmt, ...) = 0;
};

// Z80 CPU with HBIOS port I/O support
class hbios_cpu : public qkz80 {
public:
  HBIOSCPUDelegate* delegate;

  hbios_cpu(qkz80_cpu_mem* memory, HBIOSCPUDelegate* del = nullptr)
    : qkz80(memory), delegate(del) {}

  // Override port I/O
  qkz80_uint8 port_in(qkz80_uint8 port) override;
  void port_out(qkz80_uint8 port, qkz80_uint8 value) override;

  // Override halt handler
  void halt(void) override;

  // Override unimplemented opcode handler
  void unimplemented_opcode(qkz80_uint8 opcode, qkz80_uint16 pc) override;
};

#endif // HBIOS_CPU_H
