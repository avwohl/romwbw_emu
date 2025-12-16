# Working with CP/M Disk Images

## Disk Format

The emulator uses RomWBW's `wbw_hd1k` format for 8MB hard disk images.

### Adding wbw_hd1k to your system

Add this to `/etc/cpmtools/diskdefs`:

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

This format is also available in the RomWBW source tree at `Tools/cpmtools/diskdefs`.

## Copying Files

### Copy files TO a disk image

```bash
cpmcp -f wbw_hd1k disk.img file.com 0:
cpmcp -f wbw_hd1k disk.img file1.com file2.com 0:
```

### Copy files FROM a disk image

```bash
cpmcp -f wbw_hd1k disk.img 0:file.com ./
cpmcp -f wbw_hd1k disk.img 0:file.com /path/to/destination/
```

### List files on a disk image

```bash
cpmls -f wbw_hd1k disk.img
```

### Delete files from a disk image

```bash
cpmrm -f wbw_hd1k disk.img 0:file.com
```

## Creating a New Disk Image

```bash
# Create empty 8MB image
dd if=/dev/zero of=newdisk.img bs=512 count=16384

# Format with CP/M filesystem
mkfs.cpm -f wbw_hd1k newdisk.img
```

## Base Images

Working base images with CP/M 2.2 installed are in the `web/` directory:
- `hd1k_cpm22.img` - CP/M 2.2 with standard utilities
- `hd1k_zsdos.img` - ZSDOS
- `hd1k_games.img` - Games collection

To create a new disk based on an existing one:
```bash
cp web/hd1k_cpm22.img disks/mydisk.img
cpmcp -f wbw_hd1k disks/mydisk.img myfile.com 0:
```
