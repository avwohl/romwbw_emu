# RomWBW Disk Formats

The disk formats the emulator reads, and how to work with images using
`cpm_disk.py`.

## Quick Reference

| Format | Size | Sectors/Slice | Dir Entries | SIMH Equivalent |
|--------|------|---------------|-------------|-----------------|
| **hd1k single** | 8 MB (8,388,608 bytes) | 16,384 (0x4000) | 1024 | HDSK (compatible) |
| **hd1k combo** | 1MB + N×8MB | 16,384 per slice | 1024 | - |
| **hd512** | 8.32 MB (8,519,680 bytes) | 16,640 (0x4100) | 512 | HDCPM (compatible) |

## Reading and Writing Images

Use `cpm_disk.py`. It is a single stdlib-only Python file owned by
[cpmemu](https://github.com/avwohl/cpmemu) at `util/cpm_disk.py`, used by every
repository in this family, and there is no copy in this one. Find it the way
`src/makefile` finds qkz80 - `$CPM_DISK`, then a sister checkout, then whatever
`make install` put on PATH; `disks/verify_disk_utils.sh` has the probe.

```bash
CPM=~/src/cpmemu/util/cpm_disk.py
```

Format is auto-detected from the file size, so there is no definitions file to
pick, no driver flag, and nothing to install. A combo's slices are addressed
with `--slice N` and slice 5 is no harder to reach than slice 0.

```bash
COMBO=$(tools/romwbw-get path @disk0)       # the verified original, mode 0444
WORK=$(tools/romwbw-get path --work @disk0) # a writable copy

python3 "$CPM" list "$COMBO"                 # slice 0, auto-detected
python3 "$CPM" list --slice 3 "$COMBO"       # any slice, 0-5
python3 "$CPM" verify "$COMBO"               # check the image is consistent

mkdir -p out
python3 "$CPM" extract "$COMBO" W8.COM R8.COM -o out    # names, not wildcards
python3 "$CPM" extract --slice 3 "$COMBO" BBCBASIC.COM -o out

python3 "$CPM" add    "$WORK" myfile.com     # into slice 0, user 0
python3 "$CPM" add    --slice 2 --user 1 "$WORK" myfile.com
python3 "$CPM" add    --sys "$WORK" tool.com # SYS attribute: visible from any user area
python3 "$CPM" delete "$WORK" OLDFILE.COM
```

Write only to a `--work` copy. The cache is mode 0444 - see
[Two directories](#two-directories-and-they-are-not-interchangeable) below.

### Creating a new image

```bash
python3 "$CPM" create mydisk.img            # 8 MB hd1k
python3 "$CPM" create --combo mydisk.img    # 51 MB combo, six slices
python3 "$CPM" create --sssd floppy.img     # 250 KB 8" SSSD
```

`create` makes the image and its empty directory in one step; there is no
separate blank-file-then-format sequence.

### The one format it does not read: hd512

`cpm_disk.py` handles sssd, hd1k and combo. **It does not handle hd512.** An
8,519,680-byte image falls through its size check to the hd1k path, whose
directory is at 16,384 where hd512's is at 131,072, so the listing is wrong
rather than empty.

The emulator boots hd512 images perfectly well - it is the legacy RomWBW format
and the auto-detect fallback - but nothing in this family edits one. Every image
the catalog publishes is hd1k or combo. If you have an hd512 image whose
contents you need, boot it and use [`R8`/`W8`](FILE_TRANSFER.md).

### Boot areas

```bash
python3 "$CPM" read-boot  mydisk.img boot.bin
python3 "$CPM" write-boot mydisk.img boot.bin        # at sector 0
python3 "$CPM" write-boot mydisk.img boot.bin 4      # starting at sector 4
```

Boot area sizes: 16,384 bytes for hd1k and for a combo slice (after the 1 MB
prefix), 6,656 bytes for SSSD.

## SIMH Compatibility

Disk images created by the SIMH AltairZ80 simulator work.

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

SIMH HDSK and hd1k are **binary compatible**. Both store data as sequential
sectors from byte 0:

- SIMH HDSK: 2048 tracks × 32 sectors × 128 bytes = 8,388,608 bytes
- RomWBW hd1k: 1024 tracks × 16 sectors × 512 bytes = 8,388,608 bytes

The geometry differs but the byte layout is identical. CP/M uses logical block
addressing at the BIOS level and does not care about the physical geometry.

### Size Reference

| Size (bytes) | Size (readable) | Format | Compatible? |
|--------------|-----------------|--------|-------------|
| 8,388,608 | 8.0 MB exactly | HDSK/hd1k | Yes |
| 8,519,680 | 8.1 MB (~8.32 MB) | HDCPM/hd512 | Yes |
| 51,380,224 | 49 MB (1MB + 6×8MB) | RomWBW combo | Yes (native) |
| 337,568 | 330 KB | 88-DISK floppy | No |
| 76,720 | 75 KB | Mini-disk floppy | No |

The 51 MB combo format is RomWBW-native, not a SIMH format: a 1 MB MBR prefix
followed by six 8 MB slices, each with the same sector layout as SIMH HDSK.

### Obtaining SIMH Disk Images

- Peter Schorn's site: https://schorn.ch/altair.html
- The `cpmplus.zip` distribution includes bootable CP/M 3 images
- Various CP/M archives with `.dsk` files sized at 8 MB

## Format Details

### hd1k (modern format, recommended)

- 512 bytes/sector
- 16 sectors/track
- 1024 tracks
- 4096 bytes/block (8 sectors)
- 1024 directory entries
- 2 boot tracks (reserved), so the directory is at 2 × 16 × 512 = 16,384
- Total: 1024 × 16 × 512 = 8,388,608 bytes (exactly 8 MB)

### hd1k combo (multi-slice)

A 1 MB MBR prefix followed by six 8 MB slices, each laid out as a plain hd1k
image:

```
Offset          Size    Content
0x00000000      1 MB    MBR prefix (partition type 0x2E at offset 0x1C2)
0x00100000      8 MB    Slice 0
0x00900000      8 MB    Slice 1
0x01100000      8 MB    Slice 2
...
```

Slice N starts at `1048576 + (8388608 × N)`. `cpm_disk.py --slice N` does that
arithmetic; a slice is a plain hd1k image at an offset, and `ComboDisk` is a
subclass of `Hd1kDisk`, so there is one implementation of the format rather than
two.

### hd512 (legacy format)

- 1040 tracks × 16 sectors × 512 bytes = 8,519,680 bytes (8.32 MB)
- 512 directory entries
- 16 boot tracks, so the directory is at 16 × 16 × 512 = 131,072

The emulator reads and boots these. `cpm_disk.py` does not edit them - see
[above](#the-one-format-it-does-not-read-hd512).

## Emulator Auto-Detection

1. **MBR check:** reads sector 0, looks for signature 0x55AA at offset 510-511
2. **Partition scan:** if the MBR is valid, scans for partition type 0x2E (RomWBW hd1k)
3. **Size check:** if no 0x2E partition but size is exactly 8 MB, assumes hd1k single-slice
4. **Fallback:** otherwise assumes hd512

So a single-slice hd1k image must be exactly 8,388,608 bytes, and a combo must
carry a valid MBR with partition type 0x2E.

## Where Disk Images Come From

This repository ships none. They are built and published by
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

A guest writes to its disks - `hbios_dispatch.cc` opens every image `"rw"` - but
the cached original is mode 0444, so that open falls back to `"r"` and the image
boots read-only. The guest's first write is what fails: a `[HBIOS DIOWRITE] ...
short write` from the emulator and `Bdos Err On A: Bad Sector` inside CP/M. The
cache is not damaged and `romwbw-get verify` still passes; what is lost is the
guest's work. `path --work` copies into the second directory on first use and
prints that path instead, and that is the one to attach.

The cache is safe to delete at any time; it costs a re-download and nothing
else. The working copies are not - that is where a guest's saved files are.

### Web deployments

`~/www/romwbw` and `~/www/romwbw1` are the production and development web trees.
Nothing is copied into them by hand: `make -C web
deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` mirrors the catalog into the first and
`make -C web deploy-dev` into the second, both by running `romwbw-get mirror`,
which writes the `catalog/manifest.json` the page builds its disk list from. A
newly published image reaches the page by re-running a deploy.

### Verifying image integrity

```bash
romwbw-get verify            # re-hash everything cached, against the catalog
romwbw-get verify --repair   # and re-fetch whatever does not match
```

The catalog's SHA-256 is the reference, and it is checked on download as well as
on demand.

### The upstream package

The stock RomWBW distribution - the `Package.zip` the `emu_*` ROMs take banks
1-15 from - is in the catalog as `@upstream`:

```bash
romwbw-get path @upstream
```

`roms/build_emu_rom.sh` fetches it that way when you give it no source ROM.

## Command Line Usage

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

Units 0-1 are MD0/MD1 (RAM/ROM disks), so hard disks start at unit 2. The
pattern continues to `--disk15`: slot N is HBIOS unit N+2. Sixteen slots is what
the emulator accepts and what the settings file's `disks` array holds.

The **unit number is fixed; the drive letters are not.** CBIOS builds the map at
cold boot out of what HBIOS reports, so the same `--disk0` gets different letters
depending on how many disks are attached and what was booted: slices per hard
disk are `max(2, 8 / number of hard disks)`, and on a disk boot the booted slice
becomes `A:`.

Measured, one disk in `--disk0` and a ROM boot (`romwbw-get run -- --boot=C`),
under `Configuring Drives...`:

```
  A:=MD0:0   B:=MD1:0   C:=HDSK0:0   D:=HDSK0:1   E:=HDSK0:2
  F:=HDSK0:3 G:=HDSK0:4 H:=HDSK0:5   I:=HDSK0:6   J:=HDSK0:7
```

The same one disk booted from it (`--boot=2`) - slice 0 is now `A:` and the
memory disks follow it:

```
  A:=HDSK0:0 B:=MD0:0   C:=MD1:0     D:=HDSK0:1   E:=HDSK0:2
  F:=HDSK0:3 G:=HDSK0:4 H:=HDSK0:5   I:=HDSK0:6   J:=HDSK0:7
```

`../README.md` has the rule again with a two-disk map, and
[drive_assignment.md](drive_assignment.md) has the CBIOS side. CP/M's `ASSIGN`
shows the live map and changes it (`ASSIGN D:=HDSK0:2`), which is how to reach a
slice the automatic map did not cover.

## Troubleshooting

### A listing that is empty or looks like garbage

Check the file size first: `cpm_disk.py` picks the format from it, and an image
that is not sssd, exactly 8 MB, or a combo with a valid MBR is treated as hd1k
whatever it really is. The commonest case is an hd512 image, which is 8,519,680
bytes and keeps its directory somewhere else - see
[above](#the-one-format-it-does-not-read-hd512).

For a combo, check you passed `--slice N`. Without it every subcommand acts on
slice 0.

### Disk not detected, or the wrong format

- Verify the file size matches the expected format
- For combo disks, verify the MBR has partition type 0x2E at offset 0x1C2
- Run the emulator with `--debug` to see the format-detection messages

### Can't write to a disk

A file straight out of the `romwbw-get` cache is mode 0444 by design. Do not
`chmod` it - ask for `romwbw-get path --work`, which is the copy meant to be
written to. For any other read-only source image, copy it first.

### Files copied but not visible in CP/M

- CP/M shows user area 0 by default; `cpm_disk.py add` defaults to user 0, so
  check you did not pass `--user N`. Use `--sys` to set the SYS attribute, which
  makes a file visible from any user area.
- On a combo, check the slice: the file may be on slice 0 while CP/M has a
  different slice mapped to the drive you are looking at. `ASSIGN` shows the map.
