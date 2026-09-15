# Working with CP/M Disk Images

The tool is `cpm_disk.py`: one stdlib-only Python file, owned by
[cpmemu](https://github.com/avwohl/cpmemu) at `util/cpm_disk.py`, used by every
repository in this family and kept in no copy here. Find it with `$CPM_DISK`, a
sister checkout, or whatever cpmemu's `make install` put on PATH -
`disks/verify_disk_utils.sh` has the probe to copy.

```bash
CPM=~/src/cpmemu/util/cpm_disk.py
```

The format is auto-detected from the file size, so there is nothing to
configure and nothing to install. [DISK_FORMATS.md](DISK_FORMATS.md) has the
format reference, including the one format `cpm_disk.py` does not read.

## Copying Files

### To a disk image

```bash
python3 "$CPM" add disk.img file.com
python3 "$CPM" add disk.img file1.com file2.com
python3 "$CPM" add --user 1 disk.img file.com     # into user area 1
python3 "$CPM" add --sys disk.img tool.com        # SYS: visible from any user area
```

### From a disk image

```bash
mkdir -p out
python3 "$CPM" extract disk.img FILE.COM -o out
python3 "$CPM" extract disk.img FILE1.COM FILE2.COM -o out
```

Files are named one by one - there is no wildcard, and the output directory has
to exist already. `list` is how you find out what is there. The host file is
written under the lowercased name, so `FILE.COM` lands as `file.com`.

### Listing and deleting

```bash
python3 "$CPM" list   disk.img
python3 "$CPM" delete disk.img FILE.COM
python3 "$CPM" verify disk.img      # check the image is internally consistent
```

### Combo images

A combo is 51,380,224 bytes: a 1 MB MBR prefix and six 8 MB slices. Every
subcommand takes `--slice N` to pick one, and slice 5 is no harder to reach than
slice 0. Without `--slice` every subcommand acts on slice 0.

```bash
python3 "$CPM" list --slice 3 combo.img
python3 "$CPM" add  --slice 3 combo.img file.com
```

## Creating a New Disk Image

```bash
python3 "$CPM" create newdisk.img            # 8 MB hd1k
python3 "$CPM" create --combo newdisk.img    # 51 MB combo, six slices
```

There is a `create --sssd` for a 250 KB 8" floppy, but it fails its own
post-create verify and writes no file, and the emulator accepts no image that
size in any case.

`create` writes the image and its empty directory in one step - there is no
separate blank-file-then-format sequence, and `--force` overwrites an existing
file.

## Starting From a Published Image

There are no base images in this repository. They come from the romwbw_disks
catalog, fetched and checked against a published SHA-256 by `tools/romwbw-get` -
see [CATALOG.md](CATALOG.md). `romwbw-get list --disks` prints what a release
has: about two dozen.

```bash
romwbw-get fetch hd1k_cpm22                      # download and verify
cp "$(romwbw-get path hd1k_cpm22)" mydisk.img    # start from it
chmod u+w mydisk.img                             # the cached copy is 0444
python3 "$CPM" add mydisk.img myfile.com
```

The `chmod` is not an annoyance to work around. The cache holds the bytes the
hash was checked against, mode 0444 so that nothing writes through to them by
accident, and `cp` carries that mode to the new file. If what you want is a
writable copy of a published disk rather than a new image of your own,
`romwbw-get path --work hd1k_cpm22` makes one under
`~/.local/share/romwbw_emu/disks/` and prints its path.
