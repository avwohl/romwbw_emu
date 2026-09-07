# CPMEMU Project Rules

## No ROM and no disk image is tracked here

This repository ships neither. Do not add one, do not restore a deleted one,
and do not commit a build output that is one: `.github/workflows/test.yml`
fails on any tracked `.rom` or `.img` outside `archive/`. Until v1.40 there
were about 61 MB of them, and the cost was never the size - it was that adding
a disk image meant releasing this repository and the three GUI clients besides.

The ROMs and images are published by
[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) behind a
two-level catalog, and `tools/romwbw-get` fetches them and verifies every byte
against the SHA-256 the catalog publishes:

```
tools/romwbw-get run                   fetch the default ROM and disk, and boot
tools/romwbw-get versions              what is published, and what this build runs
tools/romwbw-get list                  the ROMs and disks in the selected release
tools/romwbw-get path @rom             one path, fetching if needed - for $(...)
tools/romwbw-get path --work @disk0    a WRITABLE copy of the default disk
```

**Read [docs/CATALOG.md](../docs/CATALOG.md)** before touching anything that
fetches, verifies, mirrors, packages or names an artifact. The one rule to
carry out of it: the cache under `$XDG_CACHE_HOME/romwbw_emu` is mode 0444 and
hash-pinned, so anything you intend to *write* to has to be a
`romwbw-get path --work` copy. Point `--disk0=` at the cache and it boots
read-only: the guest's first write fails with `Bdos Err On A: Bad Sector` and
the cache is left exactly as it was. Nothing is damaged - the guest's work
simply has nowhere to go.

One file is the exception: `archive/cpm22/com/mbasic.com`, third-party CP/M
software with no source in this tree and no upstream this project controls.
Leave it where it is; CI's guard names `archive/` for its sake. The other six
binaries that used to sit under `archive/` went on 2026-09-07 - a stock RomWBW
ROM recoverable from the upstream package, a zero-byte image, an x86-64 build
artifact, and three files `cpm22asm/build_cpm.sh` rebuilds. See
[archive/cpm22/README.md](../archive/cpm22/README.md).

## RomWBW Integration

When working on RomWBW integration:

1. **NO EMULATED PERIPHERAL CHIPS** - there is no 16550, no disk controller,
   no RTC, no video hardware. A guest does not reach a peripheral by driving
   its registers, and adding a port to stand for a real chip is the thing this
   rule forbids. What exists instead is a small private port interface, listed
   in rule 2.

   This rule and the next used to read "NO HARDWARE I/O PORTS ... do not write
   code that uses IN/OUT instructions to communicate with the emulator" and
   "NO RST TRAPS - do not rely on RST 08 or any other RST instruction to trap
   into the emulator". Both said the opposite of what the tree does, and had
   for as long as they had been written down.

2. **THE PRIVATE PORT INTERFACE, AND DO NOT ADD TO IT** - seven ports and one
   RST vector, all in `src/hbios_cpu.cc`'s two switches:

   | Port | |
   |---|---|
   | `0x00` / `0x01` | console status and data, UART-shaped (`hbios_cpu.cc:22`, `:27`, `:61`) |
   | `0x78` / `0x7C` | RAM and ROM bank registers (`:35`, `:36`, `:65`, `:66`) |
   | `0xFF` | sense switches / keyboard state (`:39`) |
   | `0xEC` | bank copy (`:71`) - `EMU_BNKCPY_PORT`, `emu_hbios.asm:40` |
   | `0xED` | bank call (`:116`) - `EMU_BNKCALL_PORT`, `emu_hbios.asm:39` |
   | `0xEE` | signal, init and status (`:130`) - `EMU_SIGNAL_PORT`, `:37` |
   | `0xEF` | HBIOS dispatch (`:135`) - `EMU_DISPATCH_PORT`, `:38` |

   A guest enters HBIOS by `RST 08`, which `emu_hbios.asm:14-20` documents as
   `RST 08` -> `JP 0FFF0h` -> `OUT (0EFh),A`. `emu_hbios.asm` also says why a
   port trap and not PC-matching: it works regardless of the bank
   configuration, and the RST vectors keep an ordinary `C3` jump that guest
   code can inspect.

   Adding a port here is a change three downstream ports have to follow, so it
   is a DOWNSTREAM.md-sized decision rather than a local one.

3. **HBIOS IS IN C++** - The HBIOS implementation lives in
   `src/hbios_dispatch.cc` (with the CPU-side interception in
   `src/hbios_cpu.cc`). The emulator provides HBIOS services by handling that
   dispatch, not by running RomWBW's Z80 HBIOS. (This rule named
   `altair_emu.cc`, a file that has never existed in this repository.)

4. **BUILD A ROMWBW ROM** - The goal is a ROM image that:
   - Uses our C++ HBIOS implementation instead of Z80 driver code
   - Contains the RomWBW boot loader, OS images, and ROM disk
   - Does NOT contain hardware-probing driver code

   That ROM is built and published by romwbw_disks now, not here.
   `roms/build_emu_rom.sh` reproduces it from `src/emu_hbios.asm` and checks
   the result against the published hash; see the Build Tools section.

5. **STUDY THE BUILD SYSTEM** - Understand how RomWBW builds ROMs and how to
   configure it to exclude hardware drivers while keeping the useful parts
   (boot loader, OS, ROM disk).

## Build Tools

The four Z80 sources - `src/r8.asm`, `src/w8.asm`, `src/emu_hbios.asm`,
`src/emu_rom.asm` - stay in this tree even though nothing here ships their
output any more. romwbw_disks builds the published ROM and the disk-resident
`r8.com`/`w8.com` from its own copies, and its `tools/check_source_drift.sh`
compares them against these and asserts they are here. Deleting one turns that
check red in another repository.

- **Z80 Assembler**: Use `um80` for assembling Z80 code. Do NOT use pasmo, z80asm, or other assemblers.
  - Add `.z80` directive at start of file to enable Z80 instructions
  - Assemble: `um80 -g file.asm` (creates file.rel in same directory as source)
  - Link: `ul80 -o output.bin -p 0000 file.rel`
  - **Building the ROM is one command.** `roms/build_emu_rom.sh` does the
    assemble, the 32 KB pad and the overlay itself; there is no hand-run
    `um80`/`ul80`/`dd` sequence any more, and no stock ROM in the tree to pass
    it:
    ```
    roms/build_emu_rom.sh
    ```
    It assembles bank 0 from `src/emu_hbios.asm`, takes banks 1-15 from a
    512 KB stock RomWBW ROM you name or from the sha256-pinned upstream
    `Package.zip` it fetches with `tools/romwbw-get`, and then - this is the
    point of running it - compares the result against the SHA-256 the catalog
    publishes:
    ```
    PASS: byte-identical to the published emu_avw for RomWBW 3.5.1
          sha256 4b11402a29fad22de304775b7c415eb6a74600df06bd57828b9931a7e9693258
    ```
    The file is written to a temporary directory it prints at the end; it
    refuses to write inside the repository, because a built ROM in the tree is
    exactly what was removed. It can only reproduce the release
    `ROMWBW_DEFAULT_*` in `src/romwbw_pin.h` names, because `src/emu_hbios.asm`
    here hardcodes `db 035h` / `db 010h` at CB_VERSION and again in the ident
    block. Overlaying this bank 0 on another release's banks would build a ROM
    that boots and then prints
    `*** WARNING: HBIOS/CBIOS Version Mismatch ***`, so the script refuses it
    rather than producing one.
  - **Never write `org 0100h` in a CP/M `.COM` source.** L80 bases a relocatable
    code segment at 0100h by itself, so an ORG is applied *on top of* that base
    and puts the code at 0200h behind 256 zero bytes. The result runs - CP/M
    loads the whole file at 0100h and the Z80 slides through 256 NOPs into the
    code - which is why `src/w8.asm` carried one unnoticed for a long time,
    wasting 256 bytes of every copy and making the binary uncomparable with
    `src/r8.asm`, which had none. Build a `.COM` with no ORG at all:
    ```
    um80 -o w8.rel ../src/w8.asm && ul80 -o w8.com w8.rel
    ```
    (The `-p` origin flag does not help: it names where the image is *loaded*,
    not where the segment starts. The `-p 0000` above is for a ROM image, which
    genuinely does start at address zero.)

## Emulator Architecture

- `qkz80` - Z80/8080 CPU emulator
- `banked_mem` / `romwbw_mem.h` - Bank-switched memory (512KB ROM + 512KB RAM)
- `romwbw_emu.cc` - Main emulator with HBIOS service handlers
- HBIOS calls are handled by intercepting execution at specific addresses and reading/writing CPU registers directly from C++

## Disk Formats (IMPORTANT)

**Read `docs/DISK_FORMATS.md` before working on disk-related code.**

Key points:
- **hd1k format**: 8MB (8,388,608 bytes exactly) single-slice, or combo with 1MB MBR prefix
- **hd512 format**: 8.32MB (8,519,680 bytes) legacy format
- The emulator auto-detects format by:
  1. Checking for MBR signature (0x55AA) and partition type 0x2E
  2. If no MBR but size = exactly 8MB, assumes hd1k single-slice
  3. Otherwise falls back to hd512

**Common pitfalls:**
- Single-slice hd1k images MUST be exactly 8,388,608 bytes for auto-detect
- Combo disks need the 1MB MBR prefix with partition type 0x2E at offset 0x1C2
- Disks attach as `--disk0=` through `--disk15=`, and the path is a fetched
  file, not one in this tree:
  `--disk0="$(tools/romwbw-get path --work @disk0)"`. Use `path --work` and not
  a bare `path`: the guest writes to the image, and the cached copy is the
  hash-verified original.

**cpmtools: pick the right diskdef, and do not set DISKDEFS**

`disks/diskdefs` in this repository is the definitive one, and
`disks/verify_disk_utils.sh` `cd`s into `disks/` before every cpmtools command
so that it is the one picked up. Do the same by hand. Do not rely on the
system file: cpmtools reads `./diskdefs` OR the system copy, never both, and
what the system copy holds depends on the distribution. This file used to say
the stock file "already defines the whole `wbw_hd1k` family including the
per-slice `wbw_hd1k_0..3`. There is nothing to export." That is true of
homebrew and false of Debian and Ubuntu, whose cpmtools 2.23 has `wbw_hd1k`
and no per-slice definition at all - and it is what once sent a reading of
this astray and turned CI red for a file that was on the image. Nobody's
copy is upstream's: the 2.23 tarball from moria.de ships 139 diskdefs and
not one of them mentions RomWBW, so every `wbw_*` definition anywhere is a
packager's addition and has to be checked rather than assumed. Setting
`DISKDEFS` to a path that does not exist is *silently ignored*, so the wrong
instruction looks like it works.

`disks/diskdefs` now carries `wbw_hd1k` and `wbw_hd1k_0..5`, all six slice
definitions checked against the published `hd1k_combo` image. Its comment
header records the one thing worth knowing before using the higher slices:
cpmtools 2.23 cannot be built without libdsk, and its libdsk backend cannot
address anything past 8 MB from the start of the image, so on any cpmtools
linked against libdsk - which is every packaged build, homebrew's as much as
Debian's - `wbw_hd1k_1` and up answer "cannot read superblock (Bad parameter)"
and even `wbw_hd1k_0` cannot reach the last 1 MB of slice 0. Measured on
homebrew's cpmtools 2.23, whose `cpmls` links `libdsk.3.dylib`: `wbw_hd1k_1`
through `_5` all report it. That is a limit of the build, not of the
definitions; a device_posix build of the same sources reads all six.

The images are not in this tree, so the first step is fetching one. Read from
the cache copy; write only to a `--work` copy. A write aimed at the cache does
not land: `cpmcp` prints `can not umount device: Disc is read-only.` and exits
1, `cpmrm` exits 0 in silence, and either way the image is byte-for-byte what
it was. The cache is never damaged - the edit is just not there.

```bash
cd disks    # so cpmtools reads ./diskdefs, not the system one
COMBO=$(../tools/romwbw-get path @disk0)             # 49 MB combo, mode 0444
INFOCOM=$(../tools/romwbw-get path hd1k_infocom)     # fetched on demand

cpmls -T logical -f wbw_hd1k   "$INFOCOM"            # plain 8 MB image
cpmls -T logical -f wbw_hd1k_0 "$COMBO"              # combo: slice 0, past the 1 MB MBR
cpmcp -T logical -f wbw_hd1k_0 "$COMBO" 0:w8.com ./w8.com    # extract

WORK=$(../tools/romwbw-get path --work @disk0)       # writable copy
cpmrm -T logical -f wbw_hd1k_0 "$WORK" 0:w8.com      # cpmcp will NOT
cpmcp -T logical -f wbw_hd1k_0 "$WORK" ./w8.com 0:w8.com     # overwrite
```

**`-T logical` is not optional, and leaving it off destroys data.** cpmtools
2.23 cannot be configured without libdsk, and its default libdsk path
double-counts `boottrk` on a diskdef that carries no `offset` - which
`wbw_hd1k` is - so everything the bare form addresses lands one boot area too
far into the file. Measured here against the published `hd1k_infocom`, whose
directory is at `boottrk*sectrk*seclen` = 16384: bare `cpmls` prints 373 entries
of mojibake where 69 files are, and bare `cpmcp` exits 0 in silence having put
the directory entry at 32768 and the file's data at 102400 - over 4 KB of
`amfv.z4`, which was already there. With `-T logical` the same copy writes at
23232 and into a free block. The offset-carrying slice definitions are
unaffected either way, which is why the combo always looked fine; pass them the
flag anyway, so there is one recipe rather than two. `docs/DISK_FORMATS.md` has
the full measurements and the `dd` recipe for slices 1 to 5, which are past
libdsk's separate 8 MB ceiling and which `-T logical` does not reach.
`disks/verify_disk_utils.sh` passes the flag when the local cpmtools advertises
`-T`.

The `wbw_hd1k_0` definition is what handles the combo image's 1 MB prefix, so
the dd slice-extract-and-write-back dance this file used to prescribe is not
needed for slice 0. It is still the only way into slices 1 to 5, which are past
libdsk's 8 MB ceiling: `dd ... bs=1048576 skip=$((1 + 8*N)) count=8`, then read
the cut as a plain `wbw_hd1k` image. **The real hazard is the one worth
keeping:** the wrong diskdef does not fail, it prints a garbage directory, which
reads as "no such file". That is exactly how `hd1k_infocom.img` was once
recorded as carrying no `w8.com` when it carries both. `-T logical` does not
retire that hazard - it only removes the largest instance of it, which was a
correct diskdef reading the wrong part of the file.

For `r8.com` / `w8.com` specifically, do not do any of this by hand.
`disks/rebuild_disk_utils.sh` is gone with the images it installed into:
romwbw_disks builds the published images and asserts the same things at build
time. What is left here is `disks/verify_disk_utils.sh` (run by
`make -C src test`), and it now asks two questions:

- It always assembles `src/r8.asm` and `src/w8.asm` and asserts the w8.com they
  produce still carries the bytes `06 e9 cf` - `ld b,H_CAPS` then `rst 8`, the
  whole of `check_host_path_safe`. That half needs no network and no image, and
  it is what stands between an edit to `src/w8.asm` and a W8 that hands a
  guest-supplied host path to a front end that promised nothing about where it
  lands.
- It then inspects any `*.img` in a directory you name, defaulting to the
  romwbw-get cache, and compares what it finds against what it just built. An
  empty cache is a SKIP, not a failure: a machine that has fetched nothing has
  nothing to be wrong.
