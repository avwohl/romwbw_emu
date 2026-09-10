# Working with CP/M Disk Images

## Disk Format

The emulator uses RomWBW's `wbw_hd1k` format for 8MB hard disk images.

### Where the definitions live

`disks/diskdefs` in this repository carries `wbw_hd1k` and the per-slice
`wbw_hd1k_0` through `wbw_hd1k_5`. cpmtools reads `./diskdefs` if there is one
and the system copy otherwise - one or the other, never both - so run cpmtools
from `disks/` and there is nothing to install. Do not set `DISKDEFS`: a path
that does not exist is silently ignored, so getting it wrong looks like it
worked. [DISK_FORMATS.md](DISK_FORMATS.md) has the longer version, including
why no distribution's system copy can be assumed to have the slice definitions.

Every command below passes `-T logical`, and it has to. cpmtools 2.23 cannot be
configured without libdsk, and its default libdsk path double-counts `boottrk`
on a diskdef that carries no `offset` - which `wbw_hd1k` is - so the bare form
addresses a plain 8 MB image one boot area too far into the file and reports no
error. A listing comes back as mojibake, and a copy-in exits 0 having written
over a file that was already there. `-T logical` bypasses libdsk's auto-probe
and both land where the geometry says. The cause, the measured byte offsets and
the `dd` recipe for slices 1 to 5 are in
[DISK_FORMATS.md](DISK_FORMATS.md#always-pass--t-logical).

The plain 8 MB definition, for reference:

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

## Copying Files

### Copy files TO a disk image

```bash
cpmcp -T logical -f wbw_hd1k disk.img file.com 0:
cpmcp -T logical -f wbw_hd1k disk.img file1.com file2.com 0:
```

### Copy files FROM a disk image

```bash
cpmcp -T logical -f wbw_hd1k disk.img 0:file.com ./
cpmcp -T logical -f wbw_hd1k disk.img 0:file.com /path/to/destination/
```

### List files on a disk image

```bash
cpmls -T logical -f wbw_hd1k disk.img
```

### Delete files from a disk image

```bash
cpmrm -T logical -f wbw_hd1k disk.img 0:file.com
```

## Creating a New Disk Image

```bash
# Create empty 8MB image
dd if=/dev/zero of=newdisk.img bs=512 count=16384

# Format with CP/M filesystem.  mkfs.cpm has no -T option and needs none: it
# puts the empty directory at 16384, which is where -T logical then looks.
mkfs.cpm -f wbw_hd1k newdisk.img
```

## Starting From a Published Image

There are no base images in this repository. This section used to point at
`web/hd1k_cpm22.img`, `web/hd1k_zsdos.img` and `web/hd1k_games.img`, and `web/`
has held none of them for a long time - `.gitignore` excludes `web/*.img`. v1.40
removed the images from `disks/` as well.

They come from the romwbw_disks catalog now, fetched and checked against a
published SHA-256 by `tools/romwbw-get` - see [CATALOG.md](CATALOG.md).
`romwbw-get list --disks` prints what a release has, and it is more than three:
about two dozen, including all of the above.

```bash
romwbw-get fetch hd1k_cpm22                      # download and verify
cp "$(romwbw-get path hd1k_cpm22)" mydisk.img    # start from it
chmod u+w mydisk.img                             # the cached copy is 0444
cpmcp -T logical -f wbw_hd1k mydisk.img myfile.com 0:
```

Leaving `-T logical` off that last line does not fail. It writes the directory
entry one boot area past the directory and the file's data over whatever was in
that block - see [Where the definitions live](#where-the-definitions-live).

The `chmod` is not an annoyance to work around. The cache holds the bytes the
hash was checked against, mode 0444 so that nothing writes through to them by
accident; `cp` carries that mode to the new file. If what you want is a writable
copy of a published disk rather than a new image of your own, `romwbw-get path
--work hd1k_cpm22` makes one under `~/.local/share/romwbw_emu/disks/` and prints
its path.
