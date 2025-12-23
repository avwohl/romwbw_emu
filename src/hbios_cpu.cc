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

  HBIOSDispatch* hbios = delegate->getHBIOS();
  banked_mem* memory = delegate->getMemory();

  switch (port) {
    case 0x68: {  // UART data register - read character
      int ch = hbios->readInputChar();
      return (ch >= 0) ? (ch & 0xFF) : 0;
    }

    case 0x6D:  // UART Line Status Register (LSR)
      // Bit 0: Data Ready, Bit 5: THRE (transmitter empty), Bit 6: TEMT
      return 0x60 | (hbios->hasInputChar() ? 0x01 : 0x00);

    case 0x69:  // UART IER
    case 0x6A:  // UART IIR
    case 0x6B:  // UART LCR
    case 0x6C:  // UART MCR
    case 0x6E:  // UART MSR
    case 0x6F:  // UART SCR
      return 0x00;  // Safe defaults

    case 0x70:  // RTC latch register
      return 0xFF;  // No RTC present

    case 0x78:  // Bank register (RAM)
    case 0x7C:  // Bank register (ROM)
      return memory ? memory->get_current_bank() : 0xFF;

    case 0xFE:  // Sense switches (front panel)
      return 0x00;

    default:
      return 0xFF;  // Floating bus
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
    case 0x68:  // UART data register - output character
      hbios->queueOutputChar(value);
      break;

    case 0x69:  // UART IER
    case 0x6A:  // UART FCR
    case 0x6B:  // UART LCR
    case 0x6C:  // UART MCR
    case 0x6F:  // UART SCR
      // Ignored
      break;

    case 0x70:  // RTC latch register
      // Ignored
      break;

    case 0x78:  // RAM bank select
    case 0x7C:  // ROM bank select
      // Initialize RAM bank if first access
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
      // Unknown port - ignore
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
