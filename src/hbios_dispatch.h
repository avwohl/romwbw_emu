/*
 * HBIOS Dispatch - Shared RomWBW HBIOS Handler
 *
 * This module provides HBIOS function handling that can be shared between
 * different platform implementations (CLI, WebAssembly, iOS).
 *
 * All I/O operations go through emu_io.h for platform independence.
 */

#ifndef HBIOS_DISPATCH_H
#define HBIOS_DISPATCH_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

//=============================================================================
// HBIOS Function Codes (from RomWBW)
//=============================================================================

// HBIOS Result codes
enum HBiosResult {
  HBR_SUCCESS  = 0x00,  // Success
  HBR_FAILED   = 0xFF,  // General failure
  HBR_PENDING  = 0xFE,  // Operation pending
  HBR_NODATA   = 0xFD,  // No data available
};

// HBIOS function codes (passed in B register)
enum HBiosFunc {
  // Serial I/O (Character I/O)
  HBF_CIOIN    = 0x00,  // Read character
  HBF_CIOOUT   = 0x01,  // Write character
  HBF_CIOIST   = 0x02,  // Input status
  HBF_CIOOST   = 0x03,  // Output status
  HBF_CIOINIT  = 0x04,  // Initialize port
  HBF_CIOQUERY = 0x05,  // Query port
  HBF_CIODEVICE= 0x06,  // Get device info

  // Disk I/O
  HBF_DIOSTATUS= 0x10,  // Get disk status
  HBF_DIORESET = 0x11,  // Reset disk
  HBF_DIOREAD  = 0x12,  // Read sectors
  HBF_DIOWRITE = 0x13,  // Write sectors
  HBF_DIOVERIFY= 0x14,  // Verify sectors
  HBF_DIOSENSE = 0x15,  // Sense media
  HBF_DIOCAP   = 0x16,  // Get capacity
  HBF_DIOGEOM  = 0x17,  // Get geometry
  HBF_DIOINIT  = 0x18,  // Initialize disk
  HBF_DIOQUERY = 0x19,  // Query disk
  HBF_DIODEVICE= 0x1A,  // Get device info
  HBF_DIOFORMAT= 0x1B,  // Format track

  // DSKY (Display/Keypad) functions - 0x30-0x3A
  HBF_DSKYRESET  = 0x30,  // Reset DSKY
  HBF_DSKYSTATUS = 0x31,  // Input status
  HBF_DSKYGETKEY = 0x32,  // Get key
  HBF_DSKYSETLEDS= 0x33,  // Set LEDs
  HBF_DSKYSETHEX = 0x34,  // Set hex display
  HBF_DSKYSETSEG = 0x35,  // Set segments
  HBF_DSKYBEEP   = 0x36,  // Beep
  HBF_DSKYINIT   = 0x38,  // Initialize
  HBF_DSKYQUERY  = 0x39,  // Query
  HBF_DSKYDEVICE = 0x3A,  // Get device info

  // Video Display Adapter (VDA) functions - 0x40-0x4F
  HBF_VDAINIT    = 0x40,  // Initialize
  HBF_VDAQUERY   = 0x41,  // Query
  HBF_VDARESET   = 0x42,  // Reset
  HBF_VDADEVICE  = 0x43,  // Get device info
  HBF_VDASCS     = 0x44,  // Set cursor style
  HBF_VDASCP     = 0x45,  // Set cursor position
  HBF_VDASAT     = 0x46,  // Set attribute
  HBF_VDASCO     = 0x47,  // Set color
  HBF_VDAWRC     = 0x48,  // Write character
  HBF_VDAFIL     = 0x49,  // Fill region
  HBF_VDACPY     = 0x4A,  // Copy region
  HBF_VDASCR     = 0x4B,  // Scroll
  HBF_VDAKST     = 0x4C,  // Keyboard status
  HBF_VDAKFL     = 0x4D,  // Keyboard flush
  HBF_VDAKRD     = 0x4E,  // Keyboard read
  HBF_VDARDC     = 0x4F,  // Read character at cursor

  // Sound functions - 0x50-0x58
  HBF_SNDRESET   = 0x50,  // Reset
  HBF_SNDVOL     = 0x51,  // Set volume
  HBF_SNDPER     = 0x52,  // Set period
  HBF_SNDNOTE    = 0x53,  // Set note
  HBF_SNDPLAY    = 0x54,  // Play
  HBF_SNDQUERY   = 0x55,  // Query
  HBF_SNDDUR     = 0x56,  // Set duration
  HBF_SNDDEVICE  = 0x57,  // Get device info
  HBF_SNDBEEP    = 0x58,  // Simple beep

  // RTC (Real-Time Clock) - 0x20-0x2F
  HBF_RTCGETTIM  = 0x20,  // Get time
  HBF_RTCSETTIM  = 0x21,  // Set time
  HBF_RTCGETBYT  = 0x22,  // Get NVRAM byte
  HBF_RTCSETBYT  = 0x23,  // Set NVRAM byte
  HBF_RTCGETBLK  = 0x24,  // Get NVRAM block
  HBF_RTCSETBLK  = 0x25,  // Set NVRAM block
  HBF_RTCGETALA  = 0x26,  // Get alarm
  HBF_RTCSETALA  = 0x27,  // Set alarm
  HBF_RTCINIT    = 0x28,  // Initialize
  HBF_RTCQUERY   = 0x29,  // Query
  HBF_RTCDEVICE  = 0x2A,  // Get device info

  // System functions - 0xF0-0xFF
  HBF_SYSRESET   = 0xF0,  // System reset
  HBF_SYSVER     = 0xF1,  // Get HBIOS version
  HBF_SYSSETBNK  = 0xF2,  // Set bank
  HBF_SYSGETBNK  = 0xF3,  // Get bank
  HBF_SYSSETCPY  = 0xF4,  // Set copy params
  HBF_SYSBNKCPY  = 0xF5,  // Bank-to-bank copy
  HBF_SYSALLOC   = 0xF6,  // Allocate memory
  HBF_SYSFREE    = 0xF7,  // Free memory
  HBF_SYSGET     = 0xF8,  // Get system info
  HBF_SYSSET     = 0xF9,  // Set system info
  HBF_SYSPEEK    = 0xFA,  // Peek byte from bank
  HBF_SYSPOKE    = 0xFB,  // Poke byte to bank
  HBF_SYSINT     = 0xFC,  // Interrupt management
  HBF_SYSBOOT    = 0xFE,  // EMU: Boot from device (custom)
};

// SYSGET/SYSSET subfunctions (C register)
enum HBiosSysGetFunc {
  SYSGET_CIOCNT    = 0x00,  // Get CIO device count
  SYSGET_CIODEV    = 0x01,  // Get CIO device info
  SYSGET_DIOCNT    = 0x10,  // Get DIO device count
  SYSGET_DIODEV    = 0x11,  // Get DIO device info
  SYSGET_RTCCNT    = 0x20,  // Get RTC device count
  SYSGET_RTCDEV    = 0x21,  // Get RTC device info
  SYSGET_VDACNT    = 0x40,  // Get VDA device count
  SYSGET_VDADEV    = 0x41,  // Get VDA device info
  SYSGET_SNDCNT    = 0x50,  // Get SND device count
  SYSGET_SNDDEV    = 0x51,  // Get SND device info
  SYSGET_TIMER     = 0xD0,  // Get timer value
  SYSGET_SECS      = 0xD1,  // Get seconds counter
  SYSGET_BOOTINFO  = 0xD2,  // Get boot info
  SYSGET_CPUINFO   = 0xF0,  // Get CPU info
  SYSGET_MEMINFO   = 0xF1,  // Get memory info
  SYSGET_BNKINFO   = 0xF2,  // Get bank info
  SYSGET_DEVLIST   = 0xFD,  // EMU: List available devices (custom)
};

//=============================================================================
// Disk Structure
//=============================================================================

struct HBDisk {
  bool is_open = false;
  std::string path;
  std::vector<uint8_t> data;  // For in-memory disks
  void* handle = nullptr;     // For file-backed disks (emu_disk_handle)
  bool file_backed = false;
  size_t size = 0;
};

//=============================================================================
// ROM Application Structure (for boot menu)
//=============================================================================

struct HBRomApp {
  std::string name;      // Display name
  std::string sys_path;  // Path to .sys file
  char key = 0;          // Key to press (e.g., 'B' for BASIC)
  bool is_loaded = false;
};

//=============================================================================
// HBIOS Dispatch Class
//=============================================================================

// Forward declarations for memory/CPU interfaces
class qkz80;
class banked_mem;

class HBIOSDispatch {
public:
  HBIOSDispatch();
  ~HBIOSDispatch();

  // Initialize/reset state
  void reset();

  // Set CPU and memory references (must be called before use)
  void setCPU(qkz80* cpu) { this->cpu = cpu; }
  void setMemory(banked_mem* mem) { this->memory = mem; }

  // Enable/disable debug output
  void setDebug(bool enable) { debug = enable; }
  bool getDebug() const { return debug; }

  // Disk management
  bool loadDisk(int unit, const uint8_t* data, size_t size);
  bool loadDiskFromFile(int unit, const std::string& path);
  void closeDisk(int unit);
  bool isDiskLoaded(int unit) const;
  const HBDisk& getDisk(int unit) const;

  // ROM application management
  void addRomApp(const std::string& name, const std::string& path, char key);
  void clearRomApps();

  // Signal port handler (port 0xEE)
  // Supports two protocols:
  // 1. Simple status: 0x01=starting, 0xFE=preinit, 0xFF=init complete
  // 2. Address registration: state machine for per-handler dispatch addresses
  void handleSignalPort(uint8_t value);

  // Check if PC is at an HBIOS trap address
  // Returns true if this PC should trigger HBIOS dispatch
  bool checkTrap(uint16_t pc) const;

  // Get which handler type for a trap PC (or from B register)
  // Returns: 0=CIO, 1=DIO, 2=RTC, 3=SYS, 4=VDA, 5=SND, -1=not a trap
  int getTrapType(uint16_t pc) const;
  static int getTrapTypeFromFunc(uint8_t func);

  // Handle an HBIOS call (when trap is detected)
  // Reads B,C,D,E,HL from CPU, performs operation, sets A (result)
  // Returns true if call was handled, false if unknown function
  bool handleCall(int trap_type);

  // Handle HBIOS call at main entry point (0xFFF0)
  // Dispatches based on function code in B register
  bool handleMainEntry();

  // Check if trapping is enabled
  bool isTrappingEnabled() const { return trapping_enabled; }
  void setTrappingEnabled(bool enable) { trapping_enabled = enable; }

  // Check if waiting for console input (CIOIN/VDAKRD called with no data)
  bool isWaitingForInput() const { return waiting_for_input; }
  void clearWaitingForInput() { waiting_for_input = false; }

  // Set reset callback for SYSRESET function
  // The callback should perform: switch to ROM bank 0, clear input, set PC to 0
  using ResetCallback = std::function<void(uint8_t reset_type)>;
  void setResetCallback(ResetCallback cb) { reset_callback = cb; }

  // Main entry point address (default 0xFFF0)
  void setMainEntry(uint16_t addr) { main_entry = addr; }
  uint16_t getMainEntry() const { return main_entry; }

  // Individual function handlers
  void handleCIO();   // Character I/O
  void handleDIO();   // Disk I/O
  void handleRTC();   // Real-time clock
  void handleSYS();   // System functions
  void handleVDA();   // Video display
  void handleSND();   // Sound

  // Get dispatch addresses (for debugging)
  uint16_t getCIODispatch() const { return cio_dispatch; }
  uint16_t getDIODispatch() const { return dio_dispatch; }
  uint16_t getRTCDispatch() const { return rtc_dispatch; }
  uint16_t getSYSDispatch() const { return sys_dispatch; }
  uint16_t getVDADispatch() const { return vda_dispatch; }
  uint16_t getSNDDispatch() const { return snd_dispatch; }

private:
  // CPU and memory references (not owned)
  qkz80* cpu = nullptr;
  banked_mem* memory = nullptr;
  bool debug = false;

  // Trapping control
  bool trapping_enabled = false;
  bool waiting_for_input = false;  // Set when CIOIN/VDAKRD needs input
  uint16_t main_entry = 0xFFF0;  // Main HBIOS entry point

  // Dispatch addresses (set via signal port, optional)
  uint16_t cio_dispatch = 0;
  uint16_t dio_dispatch = 0;
  uint16_t rtc_dispatch = 0;
  uint16_t sys_dispatch = 0;
  uint16_t vda_dispatch = 0;
  uint16_t snd_dispatch = 0;

  // Signal port state machine
  uint8_t signal_state = 0;
  uint16_t signal_addr = 0;

  // Bank for PEEK/POKE
  uint8_t cur_bank = 0;

  // VDA state
  int vda_rows = 25;
  int vda_cols = 80;
  int vda_cursor_row = 0;
  int vda_cursor_col = 0;
  uint8_t vda_attr = 0x07;

  // Sound state
  uint8_t snd_volume[4] = {0};
  uint16_t snd_period[4] = {0};
  uint16_t snd_duration = 100;

  // Reset callback for SYSRESET
  ResetCallback reset_callback = nullptr;

  // Disks
  HBDisk disks[16];

  // ROM applications
  std::vector<HBRomApp> rom_apps;

  // Helper: perform RET instruction (pop PC from stack)
  void doRet();

  // Helper: write string to console
  void writeConsoleString(const char* str);

  // Helper: find ROM app by key
  int findRomApp(char key) const;

  // Helper: boot from disk or ROM app
  bool bootFromDevice(const char* cmd_str);
};

#endif // HBIOS_DISPATCH_H
