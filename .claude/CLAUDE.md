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

**Read and write CP/M images with `cpm_disk.py`. Not with cpmtools.**

`cpm_disk.py` is the family's CP/M image tool: one stdlib-only Python file,
owned by **cpmemu** at `util/cpm_disk.py`, used by every repository here. It
lives in exactly one repo on purpose — romwbw_disks vendored a copy, called it
"the canonical one" while calling it from nothing, and that copy was deleted on
2026-09-10 (romwbw_disks `ff6adec`).

There is no copy in this tree and there should not be one. Find it the way
`src/makefile` finds qkz80 — `$CPM_DISK`, then a sister checkout, then whatever
`make install` put on PATH. `disks/verify_disk_utils.sh` already does exactly
that; copy its probe rather than hardcoding a path.

```bash
CPM=~/src/cpmemu/util/cpm_disk.py
COMBO=$(tools/romwbw-get path @disk0)            # 49 MB combo, mode 0444

python3 "$CPM" list "$COMBO"                     # slice 0, auto-detected
python3 "$CPM" list --slice 3 "$COMBO"           # any slice, 0-5
python3 "$CPM" extract "$COMBO" W8.COM -o ./out  # -o must already exist

WORK=$(tools/romwbw-get path --work @disk0)      # writable copy
python3 "$CPM" delete --slice 0 "$WORK" W8.COM
python3 "$CPM" add    --slice 0 "$WORK" ./w8.com
```

Format is auto-detected from size — 8 MB is a plain hd1k slice, 51,380,224 is a
combo — so there is **no diskdefs file, no `-T logical`, and no libdsk**. That
retires the entire hazard this section used to describe, and it was a real one:
cpmtools 2.23 cannot be configured without libdsk, its default path
double-counts `boottrk` on a diskdef carrying no `offset`, and the failure is
silent. Measured against the published `hd1k_infocom`, whose directory is at
16384: `cpmls -f wbw_hd1k` listed nothing, and `cpmcp` exited 0 having written
at 32768 — into the data area, over a file already there. libdsk also cannot
address past 8 MB from the start of a file, which put combo slices 1–5 out of
reach entirely. `cpm_disk.py` reads the whole file and indexes it, so
`--slice 5` is no harder than `--slice 0`.

**One limit worth knowing before you reach for it:** the **cache is mode 0444**.
Anything you intend to write to has to be a `tools/romwbw-get path --work`
copy; see the section above.

There used to be a second — `add` on a combo refused files over 16,384 bytes,
because `ComboDisk` wrote a single logical extent while `Hd1kDisk` handled
multi-extent files correctly. `ComboDisk` is a subclass of `Hd1kDisk` now
(cpmemu `566fd00`), so there is one implementation and the limit is gone: files
of any size that fit the slice round-trip, and both classes refuse a file that
would run past the end of the disk rather than growing the image.

This section used to prescribe cpmtools in detail — which diskdef per image,
why `-T logical` "is not optional", which distributions ship which `wbw_*`
definitions. All of it was accurate about cpmtools and all of it was the wrong
tool, and because it was written down here it is what a reader reached for. It
went on 2026-09-10, along with `disks/diskdefs` (nothing in this repository
reads it any more) and cpmtools from `.github/workflows/test.yml`. romwbw_disks
still uses cpmtools in `tools/build_disks.sh` to build the published images;
that is its own repository's business, and its `tools/diskdefs` is load-bearing
there.

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
