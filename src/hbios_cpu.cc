/*
 * HBIOS CPU - Shared Z80 CPU subclass for RomWBW emulation
 *
 * Implements port I/O handlers for RomWBW HBIOS.
 */

#include "hbios_cpu.h"
#include "emu_io.h"
#include <cstdio>
#include <cstdarg>

//=============================================================================
// Port IN handler
//=============================================================================

qkz80_uint8 hbios_cpu::port_in(qkz80_uint8 port) {
  if (!delegate) return 0xFF;

  banked_mem* memory = delegate->getMemory();

  switch (port) {
    case 0x00:  // Console status port (UART-style)
      // Bit 0: RX ready (input available)
      // Bit 1: TX ready (always ready)
      return (emu_console_has_input() ? 0x01 : 0x00) | 0x02;

    case 0x01:  // Console data port (UART-style)
      // Read character from console queue
      if (emu_console_has_input()) {
        int ch = emu_console_read_char();
        return (ch >= 0) ? (uint8_t)ch : 0;
      }
      return 0;

    case 0x78:  // Bank register (RAM)
    case 0x7C:  // Bank register (ROM)
      return memory ? memory->get_current_bank() : 0xFF;

    case 0xFF:  // Sense switches / keyboard state
      // Return console status in high bits for Dazzler programs
      // D7: input available, D6-D0: 0 (or could map to last key)
      return emu_console_has_input() ? 0x80 : 0x00;

    default:
      // Delegate to client for unknown ports (e.g., Dazzler)
      return delegate->handleUnknownPortIn(port);
  }
}

//=============================================================================
// Port OUT handler
//=============================================================================

void hbios_cpu::port_out(qkz80_uint8 port, qkz80_uint8 value) {
  if (!delegate) return;

  HBIOSDispatch* hbios = delegate->getHBIOS();
  banked_mem* memory = delegate->getMemory();

  switch (port) {
    case 0x01:  // Console data port (UART-style output)
      emu_console_write_char(value);
      break;

    case 0x78:  // RAM bank select
    case 0x7C:  // ROM bank select
      delegate->initializeRamBankIfNeeded(value);
      if (memory) memory->select_bank(value);
      break;

    case 0xEC: {
      // EMU BNKCPY port - inter-bank memory copy
      // Parameters from memory: 0xFFE4=src_bank, 0xFFE7=dst_bank
      // Registers: HL=src_addr, DE=dst_addr, BC=length
      if (!memory) break;

      uint16_t src_addr = regs.HL.get_pair16();
      uint16_t dst_addr = regs.DE.get_pair16();
      uint16_t length = regs.BC.get_pair16();
      uint8_t src_bank = memory->fetch_mem(0xFFE4);
      uint8_t dst_bank = memory->fetch_mem(0xFFE7);

      // Perform inter-bank copy
      for (uint16_t i = 0; i < length; i++) {
        uint8_t byte;
        uint16_t s_addr = src_addr + i;
        uint16_t d_addr = dst_addr + i;

        // Read from source
        if (s_addr >= 0x8000) {
          byte = memory->fetch_mem(s_addr);
        } else {
          byte = memory->read_bank(src_bank, s_addr);
        }

        // Write to dest
        if (d_addr >= 0x8000) {
          memory->store_mem(d_addr, byte);
        } else {
          memory->write_bank(dst_bank, d_addr, byte);
        }
      }

      // Leave the registers where an LDIR would: source and destination
      // advanced past the copy, count exhausted. RomWBW's HBX_BNKCPY is an
      // LDIR and its callers rely on that - 3.6.0's romldr copies a ROM image
      // in a loop and tests the returned HL to decide whether it is done, so a
      // ROM component spanning a bank boundary would have re-copied the same
      // chunk each pass.
      regs.HL.set_pair16((uint16_t)(src_addr + length));
      regs.DE.set_pair16((uint16_t)(dst_addr + length));
      regs.BC.set_pair16(0);
      break;
    }

    case 0xED: {
      // EMU BNKCALL port - bank call
      // On entry: A (value) = target bank, IX = call address
      uint16_t call_addr = regs.IX.get_pair16();

      // Handle known vectors via HBIOSDispatch
      if (call_addr == 0x0406) {
        // PRTSUM - Print device summary
        hbios->handlePRTSUM();
      }
      // Other vectors handled by Z80 proxy code
      break;
    }

    case 0xEE:
      // EMU signal port - handled by HBIOSDispatch
      hbios->handleSignalPort(value);
      break;

    case 0xEF:
      // HBIOS dispatch trigger port
      // Set skip_ret since Z80 proxy has its own RET
      hbios->setSkipRet(true);
      hbios->handlePortDispatch();
      hbios->setSkipRet(false);
      break;

    default:
      // Delegate to client for unknown ports (e.g., Dazzler)
      delegate->handleUnknownPortOut(port, value);
      break;
  }
}

//=============================================================================
// Halt handler
//=============================================================================

void hbios_cpu::halt(void) {
  if (delegate) {
    delegate->onHalt();
  }
}

//=============================================================================
// Unimplemented opcode handler
//=============================================================================

void hbios_cpu::unimplemented_opcode(qkz80_uint8 opcode, qkz80_uint16 pc) {
  if (delegate) {
    delegate->onUnimplementedOpcode(opcode, pc);
  }
}
