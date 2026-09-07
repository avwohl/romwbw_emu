# RomWBW Disk Formats

This document describes the disk formats supported by the romwbw_emu emulator and how to work with disk images using cpmtools.

## Quick Reference

| Format | Size | Sectors/Slice | Dir Entries | SIMH Equivalent |
|--------|------|---------------|-------------|-----------------|
| **hd1k single** | 8 MB (8,388,608 bytes) | 16,384 (0x4000) | 1024 | HDSK (compatible) |
| **hd1k combo** | 1MB + N×8MB | 16,384 per slice | 1024 | - |
| **hd512** | 8.32 MB (8,519,680 bytes) | 16,640 (0x4100) | 512 | HDCPM (compatible) |

## SIMH Compatibility

This emulator is compatible with disk images created by the SIMH AltairZ80 simulator. The table below shows which SIMH formats work:

### Supported SIMH Formats

| SIMH Format | File Size | Extension | Our Format | Status |
|-------------|-----------|-----------|------------|--------|
| **HDSK** (default hard disk) | 8,388,608 bytes (8 MB) | `.dsk`, `.img` | hd1k | **Works** |
| **HDCPM** (Amstrad hard disk) | 8,519,680 bytes (8.32 MB) | `.dsk`, `.img` | hd512 | **Works** |

### Not Supported

| SIMH Format | File Size | Reason |
|-------------|-----------|--------|
| **88-DISK** (8" floppy) | 337,568 bytes (~330 KB) | Uses 137-byte hard-sectored format |
| **Mini-disk** (5.25" floppy) | 76,720 bytes (~75 KB) | Uses 137-byte hard-sectored format |

### Why It Works

The SIMH HDSK format and our hd1k format are **binary compatible**. Both store data as sequential sectors from byte 0:

- SIMH HDSK: 2048 tracks × 32 sectors × 128 bytes = 8,388,608 bytes
- RomWBW hd1k: 1024 tracks × 16 sectors × 512 bytes = 8,388,608 bytes

The geometry differs but the byte layout is identical. CP/M doesn't care about physical geometry - it uses logical block addressing at the BIOS level.

### Using SIMH Disk Images

```bash
# 8 MB SIMH hard disk image - works directly
./romwbw_emu --romwbw="$(romwbw-get path @rom)" --disk0=cpm3.dsk

# Check file size first
ls -l myimage.dsk
# If exactly 8,388,608 bytes → treated as hd1k
# If exactly 8,519,680 bytes → treated as hd512
```

### Quick Size Reference

To determine if a `.dsk` or `.img` file will work:

```bash
ls -l *.dsk *.img
```

| Size (bytes) | Size (readable) | Format | Compatible? |
|--------------|-----------------|--------|-------------|
| 8,388,608 | 8.0 MB exactly | HDSK/hd1k | Yes |
| 8,519,680 | 8.1 MB (~8.32 MB) | HDCPM/hd512 | Yes |
| 51,380,224 | 49 MB (1MB + 6×8MB) | RomWBW combo | Yes (native) |
| 337,568 | 330 KB | 88-DISK floppy | No |
| 76,720 | 75 KB | Mini-disk floppy | No |

**Note:** The 51MB combo disk format is RomWBW-native (not a SIMH format). It contains a 1MB MBR prefix followed by six 8MB slices, each of which uses the same sector layout as SIMH HDSK.

### Obtaining SIMH Disk Images

SIMH CP/M disk images can be found at:
- Peter Schorn's site: https://schorn.ch/altair.html
- The `cpmplus.zip` distribution includes bootable CP/M 3 images
- Various CP/M archives with `.dsk` files sized at 8 MB

## Setting Up cpmtools

cpmtools needs a `diskdefs` file carrying the `wbw_*` formats. The one in this
repository, `disks/diskdefs`, is the definitive copy: it has `wbw_hd1k` and the
per-slice `wbw_hd1k_0` through `wbw_hd1k_5`, all six checked against a combo
image.

### Run cpmtools from `disks/`

```bash
cd disks
cpmls -T logical -f wbw_hd1k_0 "$(../tools/romwbw-get path @disk0)"
```

That is the whole of the setup. cpmtools reads `./diskdefs` if there is one and
the system copy (`/etc/cpmtools/diskdefs`, or the homebrew equivalent)
otherwise - one or the other, never both - so working from `disks/` puts the
right definitions in front of it and there is nothing to install.

The `_0` is not decoration: `@disk0` is the combo image, so its slice 0 starts
1 MB into the file and plain `wbw_hd1k` would read the MBR prefix as a
directory. `wbw_hd1k` is for an image that is one 8 MB slice and nothing else.

The `-T logical` is not decoration either. Without it cpmtools 2.23 addresses
a plain `wbw_hd1k` image one boot area too far into the file, reports no error,
and on a write destroys data - see [Always pass
`-T logical`](#always-pass--t-logical) below for the cause and the
measurements. Pass it to the slice definitions as well. They do not need it, but
it costs nothing and it means one recipe rather than two.

### Do not set DISKDEFS

This document used to tell you to export `DISKDEFS` pointing into a RomWBW
source tree. That advice is wrong twice over, and both halves are quiet.

A `DISKDEFS` naming a path that does not exist is **silently ignored** - cpmtools
falls back to its usual search and the instruction appears to have worked, so
the mistake is invisible until a directory listing comes out empty.

And no system copy is upstream's. The cpmtools 2.23 tarball from moria.de ships
139 diskdefs and not one of them mentions RomWBW, so every `wbw_*` definition
anywhere is a packager's addition and has to be checked rather than assumed.
Homebrew's copy carries the per-slice definitions; Debian's and Ubuntu's
cpmtools 2.23 carries `wbw_hd1k` and no `wbw_hd1k_0` at all. That difference is
what once made a `w8.com` that was on an image read as missing and turned CI red.

## Format Details

### hd1k (Modern Format - Recommended)

The hd1k format is the modern RomWBW standard with 1024 directory entries.

**Diskdef: `wbw_hd1k`**
```
diskdef wbw_hd1k
  seclen 512
  tracks 1024
  sectrk 16
  blocksize 4096
  maxdir 1024
  skew 0
  boottrk 2
  os 2.2
end
```

**Geometry:**
- 512 bytes/sector
- 16 sectors/track
- 1024 tracks
- 4096 bytes/block (8 sectors)
- 1024 directory entries
- 2 boot tracks (reserved)
- Total: 1024 × 16 × 512 = 8,388,608 bytes (exactly 8 MB)

### hd1k Combo Disk (Multi-Slice)

Combo disks have a 1 MB MBR prefix followed by multiple 8 MB slices.

**Structure:**
```
Offset          Size    Content
0x00000000      1 MB    MBR prefix (partition type 0x2E at offset 0x1C2)
0x00100000      8 MB    Slice 0
0x00900000      8 MB    Slice 1
0x01100000      8 MB    Slice 2
...
```

**Diskdefs for combo slices**, as `disks/diskdefs` defines them. cpmtools takes
`offset` in bytes here, not in tracks:

- `wbw_hd1k_0` - offset 1048576 (slice 0, after the 1 MB prefix)
- `wbw_hd1k_1` - offset 9437184
- `wbw_hd1k_2` - offset 17825792
- `wbw_hd1k_3` - offset 26214400
- `wbw_hd1k_4` - offset 34603008
- `wbw_hd1k_5` - offset 42991616

Formula: `offset (bytes) = 1048576 + (8388608 × slice_num)`. All six are in
`disks/diskdefs` and all six were checked against `hd1k_combo`, which is
51,380,224 bytes = 1 MB + six 8 MB slices exactly. This section used to list
four, with offsets in tracks (`128T`, `1152T`, `2176T`, `3200T`); that is
homebrew's stock file, which `disks/diskdefs` exists to replace.

### hd512 (Legacy Format)

The older RomWBW format with 512 directory entries.

**Diskdef: `wbw_hd512`**
- 1040 tracks × 16 sectors × 512 bytes = 8,519,680 bytes (8.32 MB)
- 512 directory entries
- 16 boot tracks

## Emulator Auto-Detection

The emulator automatically detects disk format:

1. **MBR Check**: Reads sector 0, looks for signature 0x55AA at offset 510-511
2. **Partition Scan**: If MBR valid, scans for partition type 0x2E (RomWBW hd1k)
3. **Size Check**: If no 0x2E partition but size = exactly 8 MB, assumes hd1k single-slice
4. **Fallback**: Otherwise assumes hd512 format

**For auto-detect to work:**
- Single-slice hd1k: must be exactly 8,388,608 bytes
- Multi-slice combo: must have valid MBR with partition type 0x2E

## Working with Disk Images

Every cpmtools command below carries `-T logical`, and it is load-bearing:
without it cpmtools 2.23 double-counts `boottrk` on a definition that has no
`offset` - which `wbw_hd1k` is - and reads and writes one boot area too far into
the file with no error at all. See [Always pass
`-T logical`](#always-pass--t-logical). `mkfs.cpm` is the one exception: it has
no `-T` option and does not need one.

### Creating a New Disk

```bash
# Create blank 8MB image
dd if=/dev/zero bs=1024 count=8192 of=mydisk.img

# Format as hd1k.  mkfs.cpm has no -T option and does not need one: it puts the
# empty directory at boottrk*sectrk*seclen = 16384, which is where -T logical
# then looks.  A bare cpmls on the fresh image fails outright with
# "cannot open <path> (No such file or directory)" - that is libdsk's
# auto-probe, not a bad image.
mkfs.cpm -f wbw_hd1k mydisk.img
```

### Copying Files TO a Disk

```bash
# Copy single file to user 0
cpmcp -T logical -f wbw_hd1k mydisk.img myfile.com 0:

# Copy multiple files
cpmcp -T logical -f wbw_hd1k mydisk.img *.com 0:

# Copy to user area 1
cpmcp -T logical -f wbw_hd1k mydisk.img myfile.com 1:
```

### Copying Files FROM a Disk

```bash
# Copy from user 0
cpmcp -T logical -f wbw_hd1k mydisk.img 0:myfile.com ./

# Copy all .com files from user 0
cpmcp -T logical -f wbw_hd1k mydisk.img "0:*.com" ./
```

### Listing Directory

```bash
# List all files
cpmls -T logical -f wbw_hd1k mydisk.img

# Long format with sizes
cpmls -T logical -l -f wbw_hd1k mydisk.img
```

### Working with Combo Disks

```bash
cd disks    # so cpmtools reads ./diskdefs, not the system one
COMBO=$(../tools/romwbw-get path @disk0)         # read-only, mode 0444
WORK=$(../tools/romwbw-get path --work @disk0)   # writable copy

# List slice 0 of a combo disk
cpmls -T logical -f wbw_hd1k_0 "$COMBO"

# Copy a file into slice 0 - the WORKING copy, never the cached one
cpmcp -T logical -f wbw_hd1k_0 "$WORK" myfile.com 0:
```

Slice 0 because slice 1 and up are out of reach of a packaged cpmtools, whatever
`-T` is set to. The `dd` recipe under [Always pass
`-T logical`](#always-pass--t-logical) reaches them; ["cannot read superblock
(Bad parameter)" on a higher
slice](#cannot-read-superblock-bad-parameter-on-a-higher-slice) is why it is
needed.

### Deleting Files

```bash
cpmrm -T logical -f wbw_hd1k mydisk.img 0:oldfile.com
```

## Where Disk Images Come From

This repository ships none. `disks/` held two until v1.40 and holds none now;
`web/` has held none for considerably longer - `.gitignore` excludes
`web/*.img`. The images are built and published by
[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) and fetched with
`tools/romwbw-get`, which checks every byte against the SHA-256 the catalog
publishes. [CATALOG.md](CATALOG.md) is the full account.

```bash
romwbw-get list --disks          # what the selected release has
romwbw-get path @disk0           # the verified original - read-only
romwbw-get path --work @disk0    # a writable copy, which is what you attach
```

### Two directories, and they are not interchangeable

| Directory | What is in it |
|-----------|---------------|
| `~/.cache/romwbw_emu/v0/<release>/assets/` | the download, hash-verified and mode 0444 |
| `~/.local/share/romwbw_emu/disks/<release>/` | the copy the emulator writes to |

A guest writes to its disks - `hbios_dispatch.cc` opens every image `"rw"` -
but the cached original is mode 0444, so that open falls back to `"r"` and the
image boots read-only. The guest's first write is what fails: a
`[HBIOS DIOWRITE] ... short write` from the emulator and `Bdos Err On A: Bad
Sector` inside CP/M. The cache is not damaged and `romwbw-get verify` still
passes; what is lost is the guest's work. `path --work` copies into the second
directory on first use and prints that path instead, and that is the one to
attach.

The cache is safe to delete at any time; it costs a re-download and nothing
else. The working copies are not - that is where a guest's saved files are.

### Web deployments

`~/www/romwbw` and `~/www/romwbw1` are still the production and development web
trees, but nothing is copied into them by hand any more. `make -C web
deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` mirrors the catalog into the first and
`make -C web deploy-dev` into the second, both by running `romwbw-get mirror`,
which writes a `catalog/manifest.json` the page builds its disk list from. A
newly published image reaches the page by re-running a deploy.

### Verifying image integrity

```bash
romwbw-get verify            # re-hash everything cached, against the catalog
romwbw-get verify --repair   # and re-fetch whatever does not match
```

There is no md5 against a local RomWBW source tree any more. The catalog's
SHA-256 is the reference, and it is checked on download as well as on demand.

### The upstream package

The stock RomWBW distribution - the `Package.zip` the `emu_*` ROMs take banks
1-15 from, and the same tree the `~/esrc/RomWBW-v3.5.1/Binary/hd1k_*.img`
reference images this document used to name came out of - is in the catalog as
`@upstream`:

```bash
romwbw-get path @upstream
```

`roms/build_emu_rom.sh` fetches it that way when you give it no source ROM.

## Command Line Usage

### Options

```bash
# Attach a disk as HBIOS unit 2.  One disk means eight slices - see below.
./romwbw_emu --romwbw="$(romwbw-get path @rom)" \
    --disk0="$(romwbw-get path --work @disk0)"

# Multiple disks.  The second is named by its catalog id - `romwbw-get list
# --disks` prints them.  As published today only one disk carries a default
# slot, so @disk1 has nothing to resolve to and romwbw-get says exactly that.
./romwbw_emu --romwbw="$(romwbw-get path @rom)" \
    --disk0="$(romwbw-get path --work @disk0)" \
    --disk1="$(romwbw-get path --work hd1k_games)"
```

`romwbw-get run` does the first of those in one word, fetching whatever is
missing first.

### Disk Unit Mapping

| Option | HBIOS Unit |
|--------|------------|
| --disk0 | HD0 (unit 2) |
| --disk1 | HD1 (unit 3) |

Note: Units 0-1 are MD0/MD1 (RAM/ROM disks), so hard disks start at unit 2.
The pattern continues to `--disk15`: slot N is HBIOS unit N+2. Sixteen slots is
what the emulator accepts and what the settings file's `disks` array holds.

The **unit number is fixed; the drive letters are not.** CBIOS builds the map
at cold boot out of what HBIOS reports, so the same `--disk0` gets different
letters depending on how many disks are attached and what was booted: slices
per hard disk are `max(2, 8 / number of hard disks)`, and on a disk boot the
booted slice becomes `A:`. This table once said `--disk0` is always
`C: D: E: F:`, which is the two-disk answer; with one disk it is eight slices.

Measured, one disk in `--disk0` and a ROM boot (`romwbw-get run -- --boot=C`),
under `Configuring Drives...`:

```
  A:=MD0:0
  B:=MD1:0
  C:=HDSK0:0
  D:=HDSK0:1
  E:=HDSK0:2
  F:=HDSK0:3
  G:=HDSK0:4
  H:=HDSK0:5
  I:=HDSK0:6
  J:=HDSK0:7
```

The same one disk booted from it (`--boot=2`) - slice 0 is now `A:` and the
memory disks follow it:

```
  A:=HDSK0:0
  B:=MD0:0
  C:=MD1:0
  D:=HDSK0:1
  E:=HDSK0:2
  F:=HDSK0:3
  G:=HDSK0:4
  H:=HDSK0:5
  I:=HDSK0:6
  J:=HDSK0:7
```

The "Drive Letters" section of `../README.md` has the rule again with a
two-disk map, and `drive_assignment.md` beside this file has the CBIOS side. CP/M's
`ASSIGN` shows the live map and changes it (`ASSIGN D:=HDSK0:2`), which is how
to reach a slice the automatic map did not cover.

## Troubleshooting

### "Unknown format" from cpmtools

Run cpmtools from `disks/`, which is where this repository's `diskdefs` lives.
Do not set `DISKDEFS` - see "Setting Up cpmtools" above for why that looks like
it works and does not.

### A directory listing that is plausible and wrong

The wrong diskdef does not report an error. `cpmls` prints a garbage directory,
usually an empty one, which reads as "the file is not on the image" - and that
is exactly how a `w8.com` that was on `hd1k_infocom.img` came to be recorded as
absent. Match the definition to the image: `wbw_hd1k` for a plain 8 MB image,
`wbw_hd1k_N` for slice N of a combo. A listing that surprises you is a diskdef
question before it is a disk question.

A missing `-T logical` produces the same symptom without the diskdef being
wrong, and it is the commonest way to meet it - see [Always pass
`-T logical`](#always-pass--t-logical).

### Always pass `-T logical`

Give every `cpmls`, `cpmcp` and `cpmrm` in this document its `-T logical`. It is
not a workaround for an unusual machine and there is nothing to check first: it
is the correct invocation, and the bare form silently corrupts any plain
`wbw_hd1k` image it is asked to write to.

**The cause.** cpmtools 2.23 cannot be configured without libdsk - `./configure`
aborts with "No libdsk.h" - so every packaged build has libdsk under it. Its
default libdsk path double-counts `boottrk` on a diskdef that carries no
`offset`, and `wbw_hd1k` is exactly such a definition. Everything the bare form
addresses therefore lands one boot area too far into the file. `-T logical`
names the driver instead of letting libdsk auto-probe for one, and reads and
writes then land where the geometry says.

Measured on macOS with homebrew cpmtools 2.23, whose `cpmls` links
`libdsk.3.dylib`, against the published 3.6.0 `hd1k_infocom`. That image's
directory is where the geometry puts it, at `boottrk * sectrk * seclen`
= 2 * 16 * 512 = 16384.

**Reading:**

```bash
cd disks
IMG=$(../tools/romwbw-get path hd1k_infocom)
cpmls -f wbw_hd1k "$IMG"              # 373 entries of mojibake in 28 user areas
cpmls -T logical -f wbw_hd1k "$IMG"   # the 69 files that are on the image
```

**Writing**, which is the half that does damage. The cached original is mode
0444, so this needs a copy:

```bash
cp "$IMG" copy.img && chmod u+w copy.img
cpmcp -f wbw_hd1k copy.img f.txt 0:f.txt              # exit 0, no output
cpmcp -T logical -f wbw_hd1k copy.img f.txt 0:f.txt   # exit 0, no output
```

Both exit 0 and say nothing. `cmp -l` against the original says what each of
them actually did:

| | directory entry | data block |
|---|---|---|
| bare | byte 32768 | byte 102400 - 4 KB **inside `amfv.z4`** |
| `-T logical` | byte 23232 | byte 5984256 - a free block |

32768 - 16384 is `boottrk * sectrk * seclen` over again: one boot area past the
directory. The 32 KB directory region runs to 49152, so that entry still lands
in a valid slot and a later `cpmls -T logical` does list the file - but its data
went to a block the allocation map says is in use. On this image the bare
`cpmcp` destroyed 4096 bytes of `amfv.z4`, a 262016-byte game file, and reading
`f.txt` back afterwards returns those bytes rather than the file. The
`-T logical` write put the entry at 23232, inside the directory, and the data in
a block that was free.

So the bare form does not merely fail to read. It corrupts an image it is asked
to write to, exits 0, and a subsequent bare `cpmls` reads the corruption back as
a plausible directory.

**`mkfs.cpm` is the exception.** It has no `-T` option - `mkfs.cpm: illegal
option -- T` - and does not need one. `mkfs.cpm -f wbw_hd1k` on a blank 8 MB
file puts the empty directory at 16384, and `-T logical` reads and writes the
result correctly. A bare `cpmls` on that same fresh image does not print a wrong
answer, it fails outright with `cannot open <path> (No such file or directory)`
and exit 1, because libdsk's auto-probe does not recognise it.

**The slice definitions are unaffected** by any of this, because each carries a
nonzero `offset` - which is why `hd1k_combo` always looked fine and the plain
images did not. Pass them `-T logical` anyway; it costs nothing and it means one
recipe rather than two.

#### Reaching slices 1 to 5

`-T logical` does not lift the separate 8 MB ceiling described in the next
section, so `wbw_hd1k_1` through `_5` still answer "Bad parameter" with it. Cut
the slice out and read it as a plain image instead. Slice N starts at
1 MB + 8 MB * N, so `skip=$((1 + 8 * N))`:

```bash
cd disks
COMBO=$(../tools/romwbw-get path @disk0)
dd if="$COMBO" of=/tmp/slice1.img bs=1048576 skip=9 count=8
cpmls -T logical -f wbw_hd1k /tmp/slice1.img       # 204 files
```

Measured that way, slices 1 through 5 of the published 3.6.0 `hd1k_combo` hold
204, 303, 190, 248 and 66 files. `skip=1` cuts out slice 0, which gives the same
166 files as reading it in place through `wbw_hd1k_0` - so the cut is faithful,
not an approximation. Writing back is `dd` in the other direction with
`seek=$((1 + 8 * N))` and `conv=notrunc`, on a `romwbw-get path --work` copy.

### "cannot read superblock (Bad parameter)" on a higher slice

cpmtools 2.23 cannot be configured without libdsk - `./configure` aborts with
"No libdsk.h" - and its libdsk backend cannot address anything past 8 MB from
the start of the image. So this is not a distribution's doing: on any cpmtools
linked against libdsk, which is every packaged build, `wbw_hd1k_1` and up answer
this, and even `wbw_hd1k_0` cannot reach the last 1 MB of slice 0. Measured on
homebrew's cpmtools 2.23, whose `cpmls` links `libdsk.3.dylib`: `wbw_hd1k_0`
lists `hd1k_combo`, `wbw_hd1k_1` through `_5` all report "Bad parameter". That
is a limit of the build, not of the definitions - only a `device_posix` build of
the same sources reads all six.

`-T logical` does not help here. It fixes the `boottrk` double-count described
under [Always pass `-T logical`](#always-pass--t-logical), which is a different
bug; the 8 MB ceiling is in libdsk's addressing either way, and all five higher
slices answer "Bad parameter" with the flag exactly as they do without it. Cut
the slice out with `dd` and read it as a plain image - the recipe is
[Reaching slices 1 to 5](#reaching-slices-1-to-5).

### Disk not detected / wrong format

- Verify file size matches expected format
- For combo disks, verify MBR has partition type 0x2E at offset 0x1C2
- Run emulator with `--debug` to see format detection messages

### Can't write to disk

- Check file permissions
- Verify disk image isn't mounted elsewhere
- A file straight out of the `romwbw-get` cache is mode 0444 by design. Do not
  chmod it - ask for `romwbw-get path --work` instead, which is the copy meant
  to be written to. For any other read-only source image, copy first:
  `cp original.img working.img`

### Files copied but not visible in CP/M

- Ensure you copied to user area 0:
  `cpmcp -T logical -f wbw_hd1k disk.img file.com 0:`
- Check you're using correct diskdef for the image type
- For combo disks, use slice-specific diskdef (e.g., `wbw_hd1k_0`)
- If you left out `-T logical`, the copy went one boot area too far into the
  file and landed in the data area. It did not just fail to appear, it
  overwrote something - see [Always pass `-T logical`](#always-pass--t-logical)
