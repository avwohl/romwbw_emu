# Using the Emulator

How to run the emulator once it is built or installed: the boot menu, `--boot`,
the command line, the keyboard, the settings file and R8/W8. The short version
is in [../README.md](../README.md#quick-start).

## Running from a checkout

In the Quick Start of [../README.md](../README.md#quick-start), the first two
lines are the one dependency a fresh clone is missing; see
[Building](BUILDING.md#building) for the other three ways to supply it. Anything after a
bare `--` goes to the emulator: `tools/romwbw-get run -- --boot=2.1`.

To drive the emulator yourself, ask for the paths:

```bash
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)"
```

`@rom` and `@disk0` are the catalog's defaults for the selected release; any
id from `romwbw-get list` works in their place. `path` prints the hash-verified
download, which is read-only, and `path --work` prints a writable copy - which
is what a disk needs, because the guest writes to it. `romwbw-get verify`
re-hashes the cache against the catalog and `--repair` re-fetches what does not
match; `--offline` before a subcommand never opens a socket, and `cache-dir`
prints the cache root.

## Boot menu keys

Every command is read as a line, so nothing happens until you press Enter.

- `2` - boot the first hard disk (`--disk0`), slice 0; `2.3` for slice 3
- `C` - boot CP/M 2.2 from ROM; `Z` for Z-System
- `D` - list the boot units
- `W` - SYSCONF, RomWBW's configuration utility
- `H` - the command set

Units 0 and 1 are the on-board RAM and ROM memory disks and carry no operating
system. `--disk0` is unit 2, `--disk1` unit 3, and so on.

**`H` shows a different menu on each release.** On 3.6.0 it lists the ten ROM
applications alongside the commands. On 3.5.1 it lists seven commands and no
application letters - `C` and `Z` work, but you need `L` (List ROM
Applications) to see them, and `L` is a 3.5.1 command that 3.6.0 answers with
`*** Invalid command`.

## File Transfer (R8/W8)

`R8` imports a host file into CP/M; `W8` exports a CP/M file to the host.

```
C>R8 /home/me/getkey.com          imports the host file as GETKEY.COM
C>W8 MYFILE.TXT                   exports it as ./myfile.txt on the host
C>W8 MYFILE.TXT /home/me/out.txt  exports it to that path instead
```

The host path is the whole rest of the command line, so a directory name may
contain spaces. With no host path `W8` writes the CP/M name, lowercased, into
the directory the emulator was started from. In the browser `R8` opens a file
picker and `W8` downloads.

Both live on slice 0 of `hd1k_combo`, so **which drive they are on depends on
how you booted**: after a ROM boot (`--boot=C`) that slice is `C:` and you land
on `B:`, so type `C:` first or CP/M answers `R8?`. After a disk boot
(`--boot=2`) it is `A:` and they are already on your path.

[FILE_TRANSFER.md](FILE_TRANSFER.md) has the rest: the naming and
case rules, the host-path interlock, what survives a round trip, every message
the two print, and which images carry them.

## Boot Configuration

`--boot` takes what you would have typed at the boot menu:

```
--boot=C	Boot ROM app C (CP/M 2.2); any ROM app letter works
--boot=Z	Boot ROM app Z (Z-System)
--boot=2	Boot the first hard disk (unit 2), slice 0
--boot=2.3	Boot unit 2, slice 3
--boot=H	Show the boot menu
--boot=none	Forget the persisted boot target and show the menu ('off' is the same)
```

The ROM applications are `M` Monitor, `C` CP/M 2.2, `Z` Z-System, `B` BASIC,
`T` Tasty BASIC, `F` Forth, `P` Play a Game, `N` Network Boot, `X` XModem Flash
Updater and `U` User App.

A `--boot` on the command line applies to **that run only** and is not written
back, so a script cannot change what you boot by default. A target the guest
sets with `SYSCONF` during the run is saved; `--boot=none` is the way to undo
one. NVRAM lives in `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default
`~/.config/romwbw_emu/nvram`), a single line such as `C` or `2.3`.

Type `W` at the boot menu for SYSCONF, where a boot choice can be saved as the
autoboot default. Its command set and worked examples are in
[BOOT_CONFIGURATION.md](BOOT_CONFIGURATION.md).

## Command Line Options

`romwbw_emu --help` prints the built-in usage. It covers most of these, but not
`--romapp`, `--romldr`, or `--sense`, `--load`, `--start`, `--mask-interrupt`
and `--nmi` below; `--trace` and `--symbols` it does list. The ones in daily
use:

```
./romwbw_emu --romwbw=<rom.rom> [options]
./romwbw_emu <rom.rom> [options]      # a bare path is taken as the ROM

  --version, -v     Print the version and the RomWBW releases this build runs
  --help, -h        Print the built-in usage
  --romwbw=FILE     The ROM image
  --diskN=FILE      Attach a disk image to hard-disk slot N, for N = 0..15.
                    --disk0 is boot unit 2, --disk1 unit 3, and so on.
  --boot=CMD        Auto-boot command (C, Z, 2, 2.3, H, none), this run only
  --escape=CHAR     Key reserved for the sim> prompt (default ^E)
  --escape=none     Reserve no key; every byte reaches CP/M
  --romapp=K=Name:path  Add a ROM application under boot menu key K
  --romapp=K:path       Same, with the name derived from the key
  --romldr=FILE     Use FILE as the RomWBW boot loader (romldr) image
  --config=FILE     Load settings from a JSON file (this beats --no-config)
  --no-config       Ignore auto-discovered settings files
  --save-config[=F] Write the effective settings as JSON and exit
  --debug           Enable debug output
  --strict-io       Halt on unexpected I/O ports
  --max-instructions=N  Stop after N Z80 instructions; 0 for no limit.
                    Default: none if stdin is a terminal, else ten billion
```

A run ends with exit status 0 however the guest ends it - at end of piped
input, `quit` at `sim>`, a `HLT` - and 124, as `timeout(1)` gives, when
`--max-instructions` stopped it; the line it prints on stderr says so. With no
`--max-instructions`, a run whose stdin is a terminal has no limit at all, and
one whose stdin is a pipe, a file or `/dev/null` stops at ten billion
instructions, some seven minutes at full speed. A run with a limit says so on
stderr as it starts (`Instruction limit: 10000000000 ...`). Until 2026-09-25
the limit had no option, applied at a terminal too, and a run stopped there
exited 0, which a script could not tell from the guest finishing.

`--version`, `-v`, `--help` and `-h` are answered before any settings file is
read, so they work even with a malformed `romwbw_emu.json` in the directory.

### Debugging options

`--symbols=FILE` loads a symbol table so `sim>` takes `.LABEL` where it takes
an address, and annotates the addresses it prints. `--trace=FILE` writes an
execution trace, with `--load=ADDR` naming the load address written into it.
`--start=ADDR` begins execution somewhere other than `0x0000`, and `--sense=N`
sets the sense switches (`0x...` accepted). `--mask-interrupt <min>-<max> <rst|call> <n>` and
`--nmi <min>-<max>` fire an interrupt in a cycle range - three separate
arguments, not one `=value`.

These are command-line only: a per-run diagnostic is not something a settings
file should turn on behind you. `--symbols` is the exception, and has a
`symbols` key too.

## Keyboard

Every control character goes to the guest, because CP/M software uses them:
`^R` retypes the line at the CCP prompt, `^E`/`^S`/`^D`/`^X` are the WordStar
cursor diamond, `^Q` and `^O` prefix its command sets, and `^C` warm-boots. The
terminal is put in raw mode with IXON and the line discipline's literal-next
and discard keys cleared, so `^S`, `^Q`, `^V` and `^O` reach the guest rather
than the tty driver - which also means there is no terminal-level scroll-pause
on `^S`. IXOFF is deliberately left alone: it only governs the kernel throttling
a fast sender and never consumes a typed `^S`.

**On an interactive terminal the escape character is reserved and never reaches
CP/M.** It suspends the guest and drops you at the `sim>` prompt, where `help`
lists the debugger commands and `quit` exits. The default `^E` is WordStar
cursor-up, so under WordStar or VDE move it (`--escape=^]`) or turn it off
(`--escape=none`); `CHAR` is `^A` through `^_`, a literal character, or `none` (`off` means the same).
With piped stdin nothing is reserved at all, and the startup banner says which
case you are in.

Enter arrives as CR and `Ctrl+J` sends LF - they are distinct keys. A pty
harness (`expect`, `socat`, `ttyd`) must send `\r` for Enter, not `\n`. Piped
stdin is unaffected.

**Piped stdin is held until the guest first asks for a key** - reads one, or
sits in a loop polling for one - or, failing both, until about a second after
the boot loader hands over to the OS it booted, so a script's first line goes
to CP/M's CCP, ZPM3's prompt, the boot loader's prompt or a program that starts
at boot and never waits, not to the autoboot countdown, which reads keys up to
Enter looking for Esc and used to swallow it: `printf 'STAT DSK:\rSTAT\r' |
romwbw_emu ... --boot=2` runs both. One key gets through: an Esc the script
starts with, which the countdown sees and stops on, as it does for a person -
so `printf '\033D\r' | romwbw_emu ... --boot=2` stops at the loader's prompt
and runs `D` there. The Esc has to be in the pipe when the countdown looks, a
fraction of a second into the run; `--boot=H` starts at the prompt with no
race. After that, typed-ahead input is CP/M's to handle as it would a fast
typist's: the BDOS keeps a key it sees while printing, and DDT stops a listing
on one - and so does DIR, so `printf 'DIR\rSTAT\r'` lists one file and then
runs `TAT`. For anything longer than a few lines, `tools/romwbw-batch` runs the
commands from a submit file instead - see [BATCH.md](BATCH.md).

In the browser, xterm.js translates `Ctrl`+letter to the control byte and the
page forwards it verbatim, and the terminal takes focus on load, so `Ctrl+R` is
CP/M's retype-line rather than a page reload. `Ctrl+W`, `Ctrl+T`, `Ctrl+N` and
`Ctrl+Q` still reach the browser as well - no page can prevent that - so the
tab warns before closing while the emulator is running or a disk has unsaved
writes.

## Settings File

The machine description (ROM, disks, boot command, escape character, ROM apps)
can live in a JSON file instead of a long command line. Save your current
command line, then run bare:

```bash
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)" \
               --boot=2 --save-config
src/romwbw_emu   # boots the saved machine
```

`--save-config` writes the paths as they were resolved, so the saved file names
the cached ROM and the working copy directly and never consults the catalog
again. It writes to `--save-config=FILE` if given, else to the `--config=` file
if there was one, else to `./romwbw_emu.json` - never to a discovered XDG path.

The emulator looks for `./romwbw_emu.json`, then
`$XDG_CONFIG_HOME/romwbw_emu/config.json`; `--config=FILE` names one explicitly
and `--no-config` disables discovery. CLI flags always override file values.

A key the loader does not recognise is ignored without a word, so
`--save-config` is the way to see the schema it actually reads. The keys are
`version`, `rom`, `boot`, `escape`, `symbols`, `romldr`, `debug`, `strictIo`,
`disks[]` and `romapps[]`; [CONFIGURATION.md](CONFIGURATION.md) has
the schema. `romwbw-get` keeps its own release and artifact choice in a
separate file, `$XDG_CONFIG_HOME/romwbw_emu/catalog.json`.
