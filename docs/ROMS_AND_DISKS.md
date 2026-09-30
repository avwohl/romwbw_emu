# ROMs and Disk Images

`romwbw-get list` prints the ROMs and disks a release publishes - a couple of
dozen per release: system disks, language toolchains and games. Fetch one by
its id:

```bash
tools/romwbw-get fetch hd1k_games
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work hd1k_games)"
```

`--romwbw=` wants an `emu_*` ROM, not a stock RomWBW one. Bank 0 of an `emu_*`
ROM is this project's HBIOS proxy; a ROM built for real hardware drives
peripherals that are not here, and the emulator warns and carries on rather
than refusing.

`hd1k_combo` is the catalog's slot-0 disk, so it is what `@disk0` and a bare
`romwbw-get run` resolve to: 49 MB, six slices, CP/M 2.2 and the utilities. It
is also the only published image carrying `R8` and `W8`, which `list` marks
`R8/W8`.

## RomWBW releases

One binary boots **any** RomWBW release, and this repository names none. The
version the guest sees is read from the loaded ROM's HBIOS Configuration Block
at run time; `romwbw-get versions` says which releases exist, from the catalog.

That is the whole of it, and it used to be more. Until v1.44 the core carried a
compile-time allowlist, so publishing a new RomWBW release meant rebuilding and
re-releasing Windows, macOS, iOS, Android and Linux before anybody could boot
it - the exact coupling the catalog was built to remove. What this core
actually depends on is the emulator-to-ROM interface: two I/O ports and the set
of HBIOS functions `src/hbios_dispatch.cc` services. That is versioned by the
catalog's own name - `v0` - and an interface change this core could not service
would be published as `v1`, which `romwbw-get` refuses by name without anyone
rebuilding anything.

`romwbw-get use 3.5.1` picks a release and remembers it; `--romwbw VER` before
a subcommand does it for one run.

**A ROM and the disk you boot have to be the same release**, or the guest
prints `*** WARNING: HBIOS/CBIOS Version Mismatch ***`. `romwbw-get` only ever
hands out a matched pair. Using another release's disks for data files, without
booting from them, is fine. `./roms/verify_romwbw_pin.sh [tree]` checks every
ROM and image it can find, in the tree you name and in the `romwbw-get` cache,
and is the only thing here that checks that pairing; `make -C src test` runs
it.

## Disk formats

Format is auto-detected:

| | |
|---|---|
| **hd1k, single slice** | exactly 8,388,608 bytes, no prefix. 16 KB system area, 1024 directory entries |
| **hd1k combo** | partition type 0x2E in the MBR. A 1 MB prefix, then 8 MB slices laid out as plain hd1k images |
| **hd512** | the fallback. No prefix, 8.32 MB slices (8,519,680 bytes), 128 KB system area, 512 directory entries |

SIMH AltairZ80 hard disk images work directly at either of the two single-disk
sizes above - 8,388,608 and 8,519,680 bytes. See
[DISK_FORMATS.md](DISK_FORMATS.md).

## Reading an image

Use `cpm_disk.py`, a stdlib-only Python file owned by
[cpmemu](https://github.com/avwohl/cpmemu) at `util/cpm_disk.py` and kept in no
copy here. Point `$CPM_DISK` at it, keep a cpmemu checkout beside this one, or
run cpmemu's `make install`. The .deb and .rpm carry a copy for the batch
tools, taken from cpmemu when the release was built, at
`/usr/share/romwbw_emu/cpm_disk.py`, and `python3` runs that like any other.

```bash
CPM=~/src/cpmemu/util/cpm_disk.py
COMBO=$(tools/romwbw-get path @disk0)              # 49 MB combo, mode 0444

python3 "$CPM" list "$COMBO"                       # slice 0, auto-detected
python3 "$CPM" list --slice 3 "$COMBO"             # any of the six slices
mkdir -p out && python3 "$CPM" extract "$COMBO" W8.COM -o out

WORK=$(tools/romwbw-get path --work @disk0)        # a writable copy
python3 "$CPM" delete --slice 0 "$WORK" W8.COM
python3 "$CPM" add    --slice 0 "$WORK" ./w8.com
```

The format is taken from the file size, so there is nothing to configure and
no definitions file to pick.

## Drive letters

Drive letters are not fixed: CBIOS builds the map at cold boot from what HBIOS
reports, so it depends on how many hard disks are attached and on what you
booted. Slices per hard disk = `max(2, 8 / number of hard disks)`.

On a **ROM boot** the memory disks come first - `hd1k_combo` in `--disk0`,
`hd1k_infocom` in `--disk1`, `--boot=C`:

```
A:=MD0:0	B:=MD1:0	C:=HDSK0:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK1:0	H:=HDSK1:1	I:=HDSK1:2	J:=HDSK1:3
```

On a **disk boot** the booted slice becomes `A:` - `hd1k_combo` in `--disk0`,
`--boot=2`:

```
A:=HDSK0:0	B:=MD0:0	C:=MD1:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK0:4	H:=HDSK0:5	I:=HDSK0:6	J:=HDSK0:7
```

CP/M's `ASSIGN` shows the live map and changes it (`ASSIGN D:=HDSK0:2`), which
is how to reach a slice the automatic map did not cover. See
[drive_assignment.md](drive_assignment.md).
