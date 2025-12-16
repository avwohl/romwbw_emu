/*
 * HBIOS Dispatch - Shared RomWBW HBIOS Handler
 *
 * This module provides HBIOS function handling that can be shared between
 * different platform implementations (CLI, WebAssembly, iOS).
 *
 * All I/O operations go through emu_io.h for platform independence.
 *
 * Function codes derived from RomWBW Source/HBIOS/hbios.inc
 */

#ifndef HBIOS_DISPATCH_H
#define HBIOS_DISPATCH_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

//=============================================================================
// HBIOS Function Codes (from RomWBW hbios.inc)
//=============================================================================

// HBIOS error/result codes (from hbios.inc ERR_* values)
enum HBiosResult {
  HBR_SUCCESS   = 0,     // ERR_NONE: Success
  HBR_UNDEF     = -1,    // ERR_UNDEF: Undefined error
  HBR_NOTIMPL   = -2,    // ERR_NOTIMPL: Function not implemented
  HBR_NOFUNC    = -3,    // ERR_NOFUNC: Invalid function
  HBR_NOUNIT    = -4,    // ERR_NOUNIT: Invalid unit number
  HBR_NOMEM     = -5,    // ERR_NOMEM: Out of memory
  HBR_RANGE     = -6,    // ERR_RANGE: Parameter out of range
  HBR_NOMEDIA   = -7,    // ERR_NOMEDIA: Media not present
  HBR_NOHW      = -8,    // ERR_NOHW: Hardware not present
  HBR_IO        = -9,    // ERR_IO: I/O error
  HBR_READONLY  = -10,   // ERR_READONLY: Write to read-only media
  HBR_TIMEOUT   = -11,   // ERR_TIMEOUT: Device timeout
  HBR_BADCFG    = -12,   // ERR_BADCFG: Invalid configuration
  HBR_INTERNAL  = -13,   // ERR_INTERNAL: Internal error
  // Legacy compatibility
  HBR_FAILED    = 0xFF,  // Generic failure (unsigned)
};

// HBIOS function codes (passed in B register)
// Derived from RomWBW hbios.inc BF_* definitions
enum HBiosFunc {
  // Character I/O (CIO) - 0x00-0x06
  HBF_CIO       = 0x00,
  HBF_CIOIN     = 0x00,  // Character input
  HBF_CIOOUT    = 0x01,  // Character output
  HBF_CIOIST    = 0x02,  // Character input status
  HBF_CIOOST    = 0x03,  // Character output status
  HBF_CIOINIT   = 0x04,  // Init/reset device/line config
  HBF_CIOQUERY  = 0x05,  // Report device/line config
  HBF_CIODEVICE = 0x06,  // Report device info

  // Disk I/O (DIO) - 0x10-0x1B
  HBF_DIO       = 0x10,
  HBF_DIOSTATUS = 0x10,  // Disk status
  HBF_DIORESET  = 0x11,  // Disk reset
  HBF_DIOSEEK   = 0x12,  // Disk seek
  HBF_DIOREAD   = 0x13,  // Disk read sectors
  HBF_DIOWRITE  = 0x14,  // Disk write sectors
  HBF_DIOVERIFY = 0x15,  // Disk verify sectors
  HBF_DIOFORMAT = 0x16,  // Disk format track
  HBF_DIODEVICE = 0x17,  // Disk device info report
  HBF_DIOMEDIA  = 0x18,  // Disk media report
  HBF_DIODEFMED = 0x19,  // Define disk media
  HBF_DIOCAP    = 0x1A,  // Disk capacity report
  HBF_DIOGEOM   = 0x1B,  // Disk geometry report

  // RTC (Real-Time Clock) - 0x20-0x28
  HBF_RTC       = 0x20,
  HBF_RTCGETTIM = 0x20,  // Get time
  HBF_RTCSETTIM = 0x21,  // Set time
  HBF_RTCGETBYT = 0x22,  // Get NVRAM byte by index
  HBF_RTCSETBYT = 0x23,  // Set NVRAM byte by index
  HBF_RTCGETBLK = 0x24,  // Get NVRAM data block
  HBF_RTCSETBLK = 0x25,  // Set NVRAM data block
  HBF_RTCGETALM = 0x26,  // Get alarm
  HBF_RTCSETALM = 0x27,  // Set alarm
  HBF_RTCDEVICE = 0x28,  // RTC device info report

  // DSKY (Display/Keypad) - 0x30-0x3A
  HBF_DSKY       = 0x30,
  HBF_DSKYRESET  = 0x30,  // Reset DSKY hardware
  HBF_DSKYSTAT   = 0x31,  // Get keypad status
  HBF_DSKYGETKEY = 0x32,  // Get key from keypad
  HBF_DSKYSHOWHEX= 0x33,  // Display binary value in hex
  HBF_DSKYSHOWSEG= 0x34,  // Display encoded segment string
  HBF_DSKYKEYLEDS= 0x35,  // Set/clear keypad LEDs
  HBF_DSKYSTATLED= 0x36,  // Set/clear status LEDs
  HBF_DSKYBEEP   = 0x37,  // Beep onboard DSKY speaker
  HBF_DSKYDEVICE = 0x38,  // DSKY device info report
  HBF_DSKYMESSAGE= 0x39,  // DSKY message handling
  HBF_DSKYEVENT  = 0x3A,  // DSKY event handling

  // Video Display Adapter (VDA) - 0x40-0x4F
  HBF_VDA       = 0x40,
  HBF_VDAINI    = 0x40,  // Initialize VDU
  HBF_VDAQRY    = 0x41,  // Query VDU status
  HBF_VDARES    = 0x42,  // Soft reset VDU
  HBF_VDADEV    = 0x43,  // Device info
  HBF_VDASCS    = 0x44,  // Set cursor style
  HBF_VDASCP    = 0x45,  // Set cursor position
  HBF_VDASAT    = 0x46,  // Set character attribute
  HBF_VDASCO    = 0x47,  // Set character color
  HBF_VDAWRC    = 0x48,  // Write character
  HBF_VDAFIL    = 0x49,  // Fill
  HBF_VDACPY    = 0x4A,  // Copy
  HBF_VDASCR    = 0x4B,  // Scroll
  HBF_VDAKST    = 0x4C,  // Get keyboard status
  HBF_VDAKFL    = 0x4D,  // Flush keyboard buffer
  HBF_VDAKRD    = 0x4E,  // Read keyboard
  HBF_VDARDC    = 0x4F,  // Read character

  // Sound (SND) - 0x50-0x58
  HBF_SND       = 0x50,
  HBF_SNDRESET  = 0x50,  // Reset sound system
  HBF_SNDVOL    = 0x51,  // Request sound volume
  HBF_SNDPRD    = 0x52,  // Request sound period
  HBF_SNDNOTE   = 0x53,  // Request note
  HBF_SNDPLAY   = 0x54,  // Initiate sound command
  HBF_SNDQUERY  = 0x55,  // Query sound capabilities
  HBF_SNDDUR    = 0x56,  // Request duration
  HBF_SNDDEVICE = 0x57,  // Sound device info request
  HBF_SNDBEEP   = 0x58,  // Play beep sound

  // Extension Functions - 0xE0-0xE7
  HBF_EXT       = 0xE0,
  HBF_EXTSLICE  = 0xE0,  // Slice calculation

  // Host File Transfer - 0xE1-0xE7 (EMU custom extension)
  HBF_HOST_OPEN_R = 0xE1,  // Open host file for reading (DE=path addr)
  HBF_HOST_OPEN_W = 0xE2,  // Open host file for writing (DE=path addr)
  HBF_HOST_READ   = 0xE3,  // Read byte from host (returns E=byte, A=status)
  HBF_HOST_WRITE  = 0xE4,  // Write byte to host (E=byte)
  HBF_HOST_CLOSE  = 0xE5,  // Close host file (C=0 for read, C=1 for write)
  HBF_HOST_MODE   = 0xE6,  // Get/set mode (C=0 get, C=1 set; E=mode)
  HBF_HOST_GETARG = 0xE7,  // Get cmd arg by index (E=index, DE=buf addr)

  // System Functions - 0xF0-0xFC
  HBF_SYS       = 0xF0,
  HBF_SYSRESET  = 0xF0,  // Soft reset HBIOS
  HBF_SYSVER    = 0xF1,  // Get HBIOS version
  HBF_SYSSETBNK = 0xF2,  // Set current bank
  HBF_SYSGETBNK = 0xF3,  // Get current bank
  HBF_SYSSETCPY = 0xF4,  // Bank memory copy setup
  HBF_SYSBNKCPY = 0xF5,  // Bank memory copy
  HBF_SYSALLOC  = 0xF6,  // Alloc HBIOS heap memory
  HBF_SYSFREE   = 0xF7,  // Free HBIOS heap memory
  HBF_SYSGET    = 0xF8,  // Get HBIOS info
  HBF_SYSSET    = 0xF9,  // Set HBIOS parameters
  HBF_SYSPEEK   = 0xFA,  // Get byte from alt bank
  HBF_SYSPOKE   = 0xFB,  // Set byte in alt bank
  HBF_SYSINT    = 0xFC,  // Manage interrupt vectors

  // EMU custom extension (avoid conflict with standard codes)
  HBF_SYSBOOT   = 0xFE,  // EMU: Boot from device
};

// SYSRESET subtypes (C register for SYSRESET)
enum HBiosSysResetType {
  SYSRES_INT    = 0x00,  // Reset HBIOS internal
  SYSRES_WARM   = 0x01,  // Warm start (restart boot loader)
  SYSRES_COLD   = 0x02,  // Cold start
  SYSRES_USER   = 0x03,  // User reset request
};

// SYSGET subfunctions (C register for SYSGET)
enum HBiosSysGetFunc {
  SYSGET_CIOCNT   = 0x00,  // Get char unit count
  SYSGET_CIOFN    = 0x01,  // Get CIO unit fn/data adr
  SYSGET_DIOCNT   = 0x10,  // Get disk unit count
  SYSGET_DIOFN    = 0x11,  // Get DIO unit fn/data adr
  SYSGET_RTCCNT   = 0x20,  // Get RTC unit count
  SYSGET_DSKYCNT  = 0x30,  // Get DSKY unit count
  SYSGET_VDACNT   = 0x40,  // Get VDA unit count
  SYSGET_VDAFN    = 0x41,  // Get VDA unit fn/data adr
  SYSGET_SNDCNT   = 0x50,  // Get SND unit count
  SYSGET_SNDFN    = 0x51,  // Get SND unit fn/data adr
  SYSGET_SWITCH   = 0xC0,  // Get non-volatile switch value
  SYSGET_TIMER    = 0xD0,  // Get current timer value
  SYSGET_SECS     = 0xD1,  // Get current seconds value
  SYSGET_BOOTINFO = 0xE0,  // Get boot information
  SYSGET_CPUINFO  = 0xF0,  // Get CPU information
  SYSGET_MEMINFO  = 0xF1,  // Get memory capacity info
  SYSGET_BNKINFO  = 0xF2,  // Get bank assignment info
  SYSGET_CPUSPD   = 0xF3,  // Get clock speed & wait states
  SYSGET_PANEL    = 0xF4,  // Get front panel switches val
  SYSGET_APPBNKS  = 0xF5,  // Get app bank information
  // EMU custom extension
  SYSGET_DEVLIST  = 0xFD,  // EMU: List available devices
};

// SYSSET subfunctions (C register for SYSSET)
enum HBiosSysSetFunc {
  SYSSET_SWITCH   = 0xC0,  // Set non-volatile switch value
  SYSSET_TIMER    = 0xD0,  // Set timer value
  SYSSET_SECS     = 0xD1,  // Set seconds value
  SYSSET_BOOTINFO = 0xE0,  // Set boot information
  SYSSET_CPUSPD   = 0xF3,  // Set clock speed & wait states
  SYSSET_PANEL    = 0xF4,  // Set front panel LEDs
};

// Media ID values
enum HBiosMediaId {
  MID_NONE   = 0,
  MID_MDROM  = 1,
  MID_MDRAM  = 2,
  MID_RF     = 3,
  MID_HD     = 4,
  MID_FD720  = 5,
  MID_FD144  = 6,
  MID_FD360  = 7,
  MID_FD120  = 8,
  MID_FD111  = 9,
  MID_HDNEW  = 10,
};

//=============================================================================
// Memory Disk (MD) State
//=============================================================================

struct MemDiskState {
  uint32_t current_lba = 0;     // Current LBA position
  uint8_t start_bank = 0;       // Starting bank number
  uint8_t num_banks = 0;        // Number of banks
  bool is_rom = false;          // True if ROM disk (read-only)
  bool is_enabled = false;      // True if this MD unit exists

  // Calculate total sectors (512 bytes per sector, 64 sectors per 32KB bank)
  uint32_t total_sectors() const {
    return (uint32_t)num_banks * 64;
  }
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
  uint32_t current_lba = 0;   // Current LBA position (set by DIOSEEK)

  // Partition/slice info (detected from MBR on first EXTSLICE call)
  bool partition_probed = false;     // True if MBR has been parsed
  uint32_t partition_base_lba = 0;   // Start of RomWBW partition (2048 for hd1k, 0 for hd512)
  uint32_t slice_size = 16640;       // Sectors per slice (16384 for hd1k, 16640 for hd512)
  bool is_hd1k = false;              // True for hd1k format (MID_HDNEW=10), false for hd512 (MID_HD=4)
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

  // Memory disk initialization (call after ROM is loaded)
  void initMemoryDisks();

  // Populate disk unit table in HCB (call after disks are loaded)
  // This updates the HCB at 0x160 (HCB+0x60) with disk device info
  // so romldr and other tools can discover available disks
  void populateDiskUnitTable();

  // ROM application management
  void addRomApp(const std::string& name, const std::string& path, char key);
  void clearRomApps();

  // Host file transfer (EMU extension)
  void setHostCmdLine(const std::string& cmdline) { host_cmd_line = cmdline; }

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

  // Handle bank call at 0xFFF9 (used for PRTSUM etc.)
  bool handleBankCall();

  // Handle PRTSUM - print device summary (called by boot loader 'D' command)
  void handlePRTSUM();

  // Handle HBIOS dispatch triggered by OUT to port 0xEF
  // This is the unified entry point for all platforms (CLI, web, iOS, Mac)
  // Sets skip_ret=true since Z80 proxy has its own RET instruction
  void handlePortDispatch();

  // Check if trapping is enabled
  bool isTrappingEnabled() const { return trapping_enabled; }
  void setTrappingEnabled(bool enable) { trapping_enabled = enable; }

  // Check if waiting for console input (CIOIN/VDAKRD called with no data)
  bool isWaitingForInput() const { return waiting_for_input; }
  void clearWaitingForInput() { waiting_for_input = false; }

  // Set whether blocking I/O is allowed (false for web/WASM)
  void setBlockingAllowed(bool allowed) { blocking_allowed = allowed; }
  bool isBlockingAllowed() const { return blocking_allowed; }

  // Control whether handlers do a synthetic RET
  // Set to true for I/O port dispatch (Z80 proxy has its own RET)
  // Set to false for PC-based trapping (we need to do the RET)
  void setSkipRet(bool skip) { skip_ret = skip; }
  bool getSkipRet() const { return skip_ret; }

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
  void handleDSKY();  // Display/Keypad
  void handleEXT();   // Extension functions (slice calc)

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
  bool skip_ret = false;           // Skip synthetic RET (for I/O port dispatch)
  bool blocking_allowed = true;    // Can we block for I/O? (false for web/WASM)
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

  // Bank copy state (SYSSETCPY/SYSBNKCPY)
  uint8_t bnkcpy_src_bank = 0x8E;
  uint8_t bnkcpy_dst_bank = 0x8E;
  uint16_t bnkcpy_count = 0;

  // HBIOS heap state (SYSALLOC)
  // Heap is in bank 0x80 starting after HCB (0x0200) up to 0x8000
  uint16_t heap_ptr = 0x0200;
  static constexpr uint16_t heap_end = 0x8000;

  // Bitmap tracking which RAM banks (0x80-0x8F) have been initialized
  uint16_t initialized_ram_banks = 0;

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

  // Host file transfer state (EMU extension 0xE1-0xE7)
  void* host_read_file = nullptr;   // File handle for reading (FILE*)
  void* host_write_file = nullptr;  // File handle for writing (FILE*)
  uint8_t host_transfer_mode = 0;   // 0=auto, 1=text, 2=binary
  std::string host_cmd_line;        // Original command line for GETARG

  // Reset callback for SYSRESET
  ResetCallback reset_callback = nullptr;

  // Disks
  HBDisk disks[16];

  // Memory disks (MD0=RAM, MD1=ROM)
  MemDiskState md_disks[2];

  // ROM applications
  std::vector<HBRomApp> rom_apps;

  // Helper: set result code and Z flag for HBIOS return
  void setResult(uint8_t result);

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
