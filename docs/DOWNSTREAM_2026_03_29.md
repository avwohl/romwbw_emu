# Downstream Porting Notes - 2026-03-29

Code review refactoring. Most changes are in shared code and just need a
rebuild, but platform I/O files need updating.

## New file: src/emu_io_common.cc

Shared implementations extracted from emu_io_cli.cc and emu_io_wasm.cc.
Your build must compile and link this file alongside your platform's
emu_io_*.cc.

Functions now in emu_io_common.cc (remove from your platform file if
you copied them from CLI or WASM):

	emu_file_load
	emu_file_load_to_mem
	emu_file_save
	emu_disk_open / close / read / write / flush / flush_all / size
	emu_get_time

These use only standard C (fopen/fread/fwrite/fclose, localtime).
No platform-specific APIs.

## Functions that stay in your platform file

These require platform-specific implementations and must NOT come from
emu_io_common.cc:

	emu_strcasecmp      Unix: strcasecmp       Windows: _stricmp
	emu_strncasecmp     Unix: strncasecmp      Windows: _strnicmp
	emu_file_exists     Unix: stat()           Windows: _stat() or GetFileAttributes
	emu_file_size       Unix: stat()           Windows: _stat()
	emu_sleep_ms        Unix: usleep           Windows: Sleep
	emu_set_debug       just sets a bool, but lives in platform file

See emu_io_cli.cc or emu_io_wasm.cc for reference implementations.

## New function: emu_set_debug

Declared in emu_io.h, was previously missing from CLI. Your platform
file needs:

	static bool emu_debug_enabled = false;

	void emu_set_debug(bool enable) {
	  emu_debug_enabled = enable;
	}

## Shared code changes (auto-propagate on rebuild)

hbios_dispatch.cc:
- Removed dead code (dlog/debug_log_file that never produced output)
- Removed drive map population from populateDiskUnitTable() -- this was
  dead code, overwritten by emu_populate_drive_map() called afterward
  from emu_complete_init()

romwbw_mem.h:
- Removed load_rom_file() method. Use emu_load_rom() from emu_init.h
  instead.

## No action needed

These changes are CLI-specific and don't affect other ports:

- romwbw_emu.cc removed duplicate terminal management, stdin state,
  and escape handling -- now uses emu_io API throughout
- Removed `using cpm_mem = banked_mem` alias
- Removed validate_disk_image() wrapper
