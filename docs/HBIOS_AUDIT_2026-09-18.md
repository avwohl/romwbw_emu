# HBIOS audit, 2026-09-18

What `src/hbios_dispatch.cc` does that RomWBW does not, read against the
release the published ROMs are built from.

**Why this file exists.** A pass in September 2026 produced 74 findings,
about thirty were fixed, and the rest were never written down - they went
with the session transcript, and `todo.txt` carried "they are recorded
NOWHERE" for weeks afterwards because re-deriving them meant doing the whole
pass again. This is that pass done again, and written down. Nothing here
should ever have to be re-derived.

**What it was read against.** RomWBW v3.6.0, from the package URL pinned in
`romwbw_disks/versions/3.6.0/version.json`, sha256 verified against that pin
before a line of it was read: `Source/HBIOS/hbios.asm` (267,954 bytes) and
its drivers, `Source/HBIOS/hbios.inc`, and `Source/Doc/SystemGuide.md`.
Seven readers took a function group each, one of them the part of the file no
earlier pass had covered, and every finding was then put to a separate reader
told to refute it. 77 findings were raised and 47 survived that.

**How to use it.** A finding is a place a guest would observe something
different from real RomWBW. Each carries the upstream line that decides it,
so checking one is reading, not re-deriving. Fixed entries say so and stay
here: this is the record of what was looked at, not a work queue that empties.

**State on 2026-09-18:** 77 raised. 47 confirmed. 16 more were fixed while the
audit was still running, so their verifier found nothing left to confirm. 14 did
not hold up.

Of the 47 confirmed: **44 are fixed**, 2 are confirmed-and-kept, and 1 is open.

The two kept are BF_CIOQUERY's status byte and the bare-8MB hd1k heuristic.
Both are real divergences from RomWBW and both were tried the faithful way and
measured: the faithful CIOQUERY breaks MODE.COM, and the faithful hd1k rule
makes Z3PLUS unbootable on both published releases. Each entry says what was
measured and the code says it again at the branch. They are not a backlog.

The one open item is the ROM signature pointer at 0x0005. It is a one-byte
source fix, and the only one in this audit whose fix changes every published
ROM's sha256 - so taking it means re-cutting both releases, advancing each
`generation`, and every installed client re-downloading. No shipped program was
found that reads the address. It is a decision about the release channel rather
than about the code, so it is filed as `DECISIONS.md` #8 and is the only thing
this audit leaves for someone to rule on.

---

## High severity (4)

### BF_DIOGEOM puts three of its four return values in the wrong registers and answers SUCCESS  — FIXED

- **Function:** `BF_DIOGEOM (0x1B)`
- **RomWBW:** Every RomWBW driver returns D = heads with bit 7 SET (LBA capable) - hdsk/ide/sd all do "LD D,16 | $80"; E = sectors per track = 16 ("LD E,16 ; SECTORS / TRACK = 16"); HL = cylinder count = blocks/256 ("LD L,H / LD H,E ; DIVIDE BY 256 FOR # TRACKS"); and BC = block size = 512, carried through untouched from the driver's *_CAP call. md.asm:222-236 uses the same layout with 1 head and HL = blocks/16. The geometry is self-consistent: heads * spt * cyls == capacity.
- **This dispatcher:** cpu->regs.BC.set_low(63) puts 63 in C and leaves B holding the incoming function number, so BC = 0x1B3F = 6975 instead of 512. DE.set_high(16) sets D = 0x10 with bit 7 CLEAR, i.e. "not LBA capable". DE.set_low(cyls & 0xFF) puts the cylinder low byte in E, where sectors-per-track belongs. HL = sectors/(16*63), clamped to >= 1. Status A = 0 (success) regardless.
- **A guest would see:** A caller of BF_DIOGEOM can get no correct answer out of it. On the published 49MB combo image (100,352 sectors) real RomWBW reports D=0x90, E=16, HL=392, BC=512; the emulator reports D=0x10, E=99, HL=99, BC=6975 - so sector size reads as 6975 bytes, sectors/track as 99, and the device as CHS-only. On the 256KB RAM disk (512 sectors) real reports 1 head/16 spt/32 cyls = 512 sectors; the emulator reports 16*63*1 = 1008 sectors, twice the medium. I could not find a caller inside the RomWBW 3.6.0 Source tree (only DIODEVICE/DIOMEDIA/DIOCAP are called there), so the exposure is to guest programs using the published API rather than to a stock .COM I can name.
- **Upstream:** `Source/HBIOS/hdsk.asm:176-185 (also ide.asm:791-799, sd.asm:1178-1185, md.asm:222-236); contract at Doc/SystemGuide.md:1401-1419`
- **Here:** `src/hbios_dispatch.cc:1511-1514`
- **Smallest fix:** Mirror the drivers: for a hard disk set D = 0x80|16, E = 16, HL = (uint16_t)(sectors >> 8), BC = 512; for a memory disk set D = 0x80|1, E = 16, HL = (uint16_t)(sectors >> 4), BC = 512. Set BC as a pair (set_pair16(512)), not just C - B currently still holds the function number.
- **Status:** fixed - 16x16 geometry, LBA bit set, one value per register

### SYSBNKCPY does not advance HL and DE by the byte count  — FIXED

- **Function:** `BF_SYSBNKCPY ($F5)`
- **RomWBW:** HBX_BNKCPY copies through the bounce buffer with LDIR and returns HL = source address + count, DE = destination address + count (BC = 0), A = 0. SystemGuide states this explicitly: "On return, the New Destination Address (DE) will be value of the original Destination Address (DE) incremented by the count of bytes copied. Likewise for the New Source Address (HL). This allows iterative invocations of this function to continue copying where the prior invocation left off."
- **This dispatcher:** The handler reads HL and DE, performs the byte loop, and never writes either register back. HL and DE come out exactly as the caller passed them in.
- **A guest would see:** The manual documents the chained-copy idiom: call SYSSETCPY once with a chunk size, then call SYSBNKCPY repeatedly. On the emulator every iteration recopies the same chunk to the same destination, so a loop that moves N chunks moves only the first one N times and silently reports success each time. Any banked-memory mover written against the documented contract (CP/M 3 / ZPM3 banked BIOS style block moves, an app paging data into an application bank) gets wrong data with A=0. I could not name a specific .COM from this tree - the RomWBW Apps sources are not in the downloaded package - so the name is left out deliberately.
- **Upstream:** `hbios.asm:5934-5942 (SYS_BNKCPY -> HB_BNKCPY), hbios.asm:1049 (HBX_BNKCPY), hbios.asm:1113-1130 (HBX_BC_ITER: "HL = UPDATED SRC, DE = UPDATED DEST"); Doc/SystemGuide.md:2684-2700`
- **Here:** `hbios_dispatch.cc:1773-1802`
- **Smallest fix:** After the copy loop, write the advanced pointers back: cpu->regs.HL.set_pair16((uint16_t)(src_addr + count)); cpu->regs.DE.set_pair16((uint16_t)(dst_addr + count));
- **Status:** fixed - HL and DE now advance past the block

### SYSINT answers success for every subfunction and sets no registers at all  — FIXED

- **Function:** `BF_SYSINT ($FC) - subfunctions INFO $00, GET $10, SET $20`
- **RomWBW:** SYS_INT switches on C. $00 returns D = INTMODE (1 on SBC_simh_std, Config/SBC_simh_std.asm:50) and E = the vector/chain count, A=0. $10 returns HL = the installed vector for index E, or ERR_RANGE if E is out of range, or ERR_BADCFG when INTMODE==0. $20 writes the new vector at index E and returns HL = the previous vector. Any other C value -> SYSCHKERR(ERR_NOFUNC).
- **This dispatcher:** `case HBF_SYSINT: { // Interrupt management - just return success  break; }`. C is never examined. D, E and HL are left holding whatever the caller passed in, and A is set to 0.
- **A guest would see:** This is the "success while doing nothing" case. A program asking INTINF reads its own input bytes back as the interrupt mode and IVT size. A program that hooks the timer tick calls INTSET, is told success, and is handed back its own new vector as the "previous vector" - so nothing is installed, and on exit it restores the fake previous vector over a slot it never changed. Nothing is hooked and nothing reports a problem. Real HBIOS on this ROM (INTMODE=1) would either install the handler or, if the emulator has no interrupt framework, refuse.
- **Upstream:** `hbios.asm:6954-6966 (SYS_INT dispatch), 6968-6984 (SYS_INTINFO), 6986-7023 (SYS_INTVECADR), 7025-7037 (SYS_INTGET), 7039-7062 (SYS_INTSET); Doc/SystemGuide.md:3288-3320`
- **Here:** `hbios_dispatch.cc:2233-2236`
- **Smallest fix:** Dispatch on C. If the emulator runs no interrupt framework, answer honestly: $00 -> D=0 (INTMODE 0), E=0, A=0; $10 and $20 -> HBR_BADCFG (ERR_BADCFG, what hbios.asm:6989 returns when INTMODE==0); anything else -> HBR_NOFUNC. Never A=0 with the registers untouched.
- **Status:** fixed - declines with ERR_NOTIMPL instead of reporting success

### BF_SNDPLAY: the channel is register D, not C, and volume/period are one pending set per unit, not four  — FIXED

- **Function:** `BF_SNDPLAY (0x54), BF_SNDVOL (0x51), BF_SNDPRD (0x52), BF_SNDNOTE (0x53)`
- **RomWBW:** C is the sound UNIT. SNDVOL/SNDPRD/SNDNOTE set a single pending volume and a single pending period for that unit. SNDPLAY reads the channel from D (masked to 0..3), programs the chip's registers for that one channel with the pending values, and returns; the tone then plays indefinitely.
- **This dispatcher:** D is never read. The unit number in C is used as a per-channel array index, so snd_volume[]/snd_period[] are effectively always index 0 (the only unit the emulator reports). SNDPLAY then loops all four channels and re-emits every one whose stored volume is non-zero.
- **A guest would see:** A three-voice tune player sets vol/note and calls SNDPLAY with D=0, then D=1, then D=2. On real hardware three channels sound together. On the emulator every call overwrites the same slot 0 and re-triggers channel 0, so only the last note of each triple is heard and the other two voices never exist. RomWBW's own beep path (hbios.asm:5190) also depends on D.
- **Upstream:** `ay38910.asm:376-383 ("; B = FUNCTION / C = AUDIO DEVICE / D = CHANNEL") and ay38910.asm:397 ("LD A,D ; LIMIT CHANNEL 0-2"); sn76489.asm:278 ("SN7_APPLY_VOL ... ; D CONTAINS THE CHANNEL NUMBER"); hbios.asm:5189-5190 ("LD B,$54 / LD D,0 ; CHANNEL 0"); SystemGuide.md:2352-2366 and the worked example at SystemGuide.md:2245-2248 ("HBIOS B=54 C=00 D=01 ; Play note on Channel 1")`
- **Here:** `hbios_dispatch.cc:2536 (uint8_t channel = cpu->regs.BC.get_low();), used at 2559-2561, 2565-2567, 2576-2585; SNDPLAY at 2591-2613`
- **Smallest fix:** Keep one pending volume/period/duration per unit. In HBF_SNDPLAY read `uint8_t ch = cpu->regs.DE.get_high() & 3;` and emit that one channel with the pending values; drop the per-channel indexing from SNDVOL/SNDPRD/SNDNOTE.
- **Status:** fixed - C is the unit, D is the channel


## Medium severity (25)

### BF_CIODEVICE sets only D and E, both to zero; C, H and L come back holding whatever the caller passed in  — FIXED

- **Function:** `BF_CIODEVICE (0x06)`
- **RomWBW:** Every CIO driver's DEVICE handler sets C = Device Attributes, D = Device Type, E = Device Number, and (all drivers except TTY) H = Device Mode and L = Device I/O Base, then XOR A for success. UART_DEVICE: 'LD D,CIODEV_UART / LD E,(IY) / LD C,$00 / LD H,(IY+1) / LD L,(IY+2)'. TTY_DEVICE sets C = VDA unit with bit 7 set to mark a terminal. C's top two bits are the documented type: bit 6 = parallel, bit 7 = terminal, both clear = RS-232 (decoded at Source/HBIOS/invntdev.asm:218-226).
- **This dispatcher:** The whole case body is 'cpu->regs.DE.set_pair16(0x0000); break;'. C is never written, so it still holds the unit number the caller put there; HL is never written, so it still holds the caller's value. D = 0 = CIODEV_UART (a 16C550) and E = 0 are hardcoded.
- **A guest would see:** A guest asking about the documented console pseudo-unit $80 gets C=$80 back, whose bit 7 means TERMINAL, while D=$00 claims a 16C550 UART — a combination RomWBW never produces. Any unit number >= $40 decodes as 'parallel'. A caller that prints the I/O base address (L) or the chip variant (H) prints whatever it happened to have in HL. MODE.COM, which ships on the published combo image and is the caller the BF_CIOQUERY comment just above (hbios_dispatch.cc:947-951) records as measured, is the concrete guest; I did not have MODE.COM's source in the download, so I cannot quote the exact line it prints.
- **Upstream:** `Source/HBIOS/uart.asm:740-747 (UART_DEVICE); Source/HBIOS/tty.asm:138-146 (TTY_DEVICE); Source/HBIOS/acia.asm:593-600; Source/HBIOS/lpt.asm:335-342; contract at Source/Doc/SystemGuide.md:1041-1063`
- **Here:** `src/hbios_dispatch.cc:956-961`
- **Smallest fix:** Write all five: C = $00 (RS-232) to match the D=CIODEV_UART the case already claims, H = 0 (no chip variant), L = 0 or the emulator's pseudo base port, keeping D/E as they are. If the console is meant to be a terminal instead, use TTY_DEVICE's shape: D = CIODEV_TERM ($02) and C with bit 7 set.
- **Status:** fixed - all five registers answered

### No CIO function validates the unit number, so every unit 0-255 answers success on the host console  — FIXED

- **Function:** `BF_CIOIN, BF_CIOOUT, BF_CIOIST, BF_CIOOST, BF_CIOINIT, BF_CIODEVICE`
- **RomWBW:** CIO_DISPATCH sends every call through HB_DISPCALC, which compares C against CIO_CNT before anything else and returns ERR_NOUNIT (-4) for C >= CIO_CNT. Units with bit 7 set are first folded onto the active console by CIO_SPECIAL, so $80-$FF are always valid; $00 through CIO_CNT-1 are valid; everything between CIO_CNT and $7F is an error.
- **This dispatcher:** handleCIO reads 'uint8_t unit = cpu->regs.BC.get_low()' at line 825 and no case ever tests it. Every function operates on the one host console regardless of C and returns A=0. The emulator itself reports exactly one character device — SYSGET_CIOCNT answers E=1 at hbios_dispatch.cc:1861 — so units $01-$7F are all units it has just told the guest do not exist.
- **A guest would see:** BF_CIOOUT to unit 1 returns A=0 and the byte appears on the console, where real RomWBW returns A=$FC (-4) and writes nothing — the 'answers SUCCESS while doing nothing right' case. BF_CIOIN on unit 1 blocks on and consumes the console's keystrokes, so a program reading a second serial port silently eats the user's typing. A program that enumerates character units by calling until ERR_NOUNIT never terminates. On the shipped combo image, 'MODE COM1:' is answered for a port that does not exist rather than rejected.
- **Upstream:** `Source/HBIOS/hbios.asm:7449-7452 (HB_DISPCALC: 'LD A,C / CP (IY-1) / JR NC,HB_UNITERR'); unit count at Source/HBIOS/hbios.asm:4584 (CIO_CNT); Source/HBIOS/hbios.asm:4554-4559 (CIO_SPECIAL); ERR_NOUNIT = -4 at Source/HBIOS/hbios.inc:223`
- **Here:** `src/hbios_dispatch.cc:825 (unit is read) and :969 (the only place it is used — the default-case log)`
- **Smallest fix:** At the top of handleCIO, before the switch: if ((unit & 0x80) == 0 && unit >= 1) { setResult(HBR_NOUNIT); doRet(); return; } — using the same unit count that SYSGET_CIOCNT reports rather than a literal. (Minor, same site: RomWBW checks the unit before the function, so an out-of-range unit with an out-of-range function gets ERR_NOUNIT, not ERR_NOFUNC; putting the unit test before the switch gets that ordering for free.)
- **Status:** FIXED

### BF_CIOQUERY returns ERR_NOTIMPL, which no RomWBW character driver ever returns  — KEPT ON PURPOSE, MEASURED

- **Function:** `BF_CIOQUERY (0x05)`
- **RomWBW:** Every CIO driver in the tree returns A=0 from QUERY. A terminal console (TTY_QUERY) returns DE=$FFFF, HL=$FFFF *with success* — $FFFF is how RomWBW spells 'no line configuration', and the status is still 0. ERR_NOTIMPL from BF_CIOQUERY does not occur anywhere in RomWBW v3.6.0.
- **This dispatcher:** The case sets DE=$FFFF and HL=$FFFF — exactly TTY_QUERY's values — and then deliberately overrides the status to HBR_NOTIMPL (-2), which the comment at lines 947-951 records as a considered choice made to stop MODE.COM printing a decoded garbage baud rate.
- **A guest would see:** The emulator runs the real ROM's boot loader as guest code, and romldr.asm:985-988 does 'ld b,BF_CIOQUERY / rst 08 / jp nz,err_invcmd'. Under the emulator the boot loader's CONSOLE <unit> <baud> command therefore aborts with 'Invalid command'; on real RomWBW with a terminal console it succeeds and goes on to BF_CIOINIT. The MODE.COM output the comment is defending against is what real RomWBW produces too, since TTY_QUERY hands MODE.COM the same $FFFF with success — so the deviation buys tidier MODE output at the cost of matching the authority. I did not have MODE.COM's source in the download and am relying on the in-tree comment for its behaviour; the romldr path I read directly.
- **Upstream:** `Source/HBIOS/tty.asm:130-134 (TTY_QUERY: 'LD DE,$FFFF / LD HL,$FFFF / XOR A / RET'); also uart.asm:732-736, acia.asm:585-589, asci.asm:593-597, sio.asm:979-983, duart.asm:622-626, lpt.asm:327-331, pio.asm:196-200, sser.asm:131-134, esp.asm:476-480 — every one ends 'XOR A' (success)`
- **Here:** `src/hbios_dispatch.cc:952 ('result = HBR_NOTIMPL;')`
- **Smallest fix:** Return HBR_SUCCESS with DE=HL=$FFFF, i.e. delete line 952, matching tty.asm:130-134 exactly.
- **Status:** CONFIRMED against upstream, DELIBERATELY NOT FIXED, and measured both ways. TTY_QUERY does return $FFFF with SUCCESS, so the finding is right about RomWBW. It is not right about the two callers that ship on the published images, and they want opposite things: invntdev (the ROM device inventory) ignores the status and tests the value - "LD A,D / AND E / INC A / JP Z,PS_PRTNUL" - so it needs $FFFF; MODE.COM ignores the value and aborts on status - "rst 08 / ret nz" - so with SUCCESS it goes on to decode $FF as a baud code and prints "COM0: 7372800,S,8,2". There is no serial line here to describe at all; the console is a host terminal. $FFFF with NOTIMPL is the one answer that leaves both callers correct, and it was arrived at by fixing the inventory first and then running MODE. The reasoning is in the code at the branch.

### BF_DIOSEEK ignores CHS mode and treats a CHS address as a linear sector number  — FIXED

- **Function:** `BF_DIOSEEK (0x12)`
- **RomWBW:** Every driver tests bit 7 of D. Clear means CHS: D=head, E=sector, HL=track, and HB_CHS2LBA converts it to LBA = track*256 + head*16 + sector (it builds L = (head<<4)|sector, H = old L, E = old H, D = 0). Bit 7 is then cleared regardless, so the caller gets DEHL back holding the resolved LBA with D:7 clear.
- **This dispatcher:** lba = ((DE & 0x7FFF) << 16) | HL, unconditionally. The 0x7FFF mask is the right equivalent of RES 7,D for LBA mode, but there is no bit-7 test and no CHS conversion, and DE/HL are left exactly as the caller passed them.
- **A guest would see:** A CHS seek lands on a different sector. Head 0, sector 0, track 1 (DEHL = 0x00000001) means LBA 256 on real RomWBW and LBA 1 on the emulator - a guest reading track 1 gets sector 1. Head 1, sector 0, track 0 (DEHL = 0x01000000) means LBA 16 on real RomWBW and LBA 0x01000000 on the emulator, which is past the end of every image and so comes back as an I/O error from the following read. The manual states all drivers have accepted both modes since HBIOS v3.1, so this is a supported entry path, not a legacy one.
- **Upstream:** `Source/HBIOS/hdsk.asm:208-217 and md.asm:259-270 ("BIT 7,D / CALL Z,HB_CHS2LBA"); the conversion itself at hbios.asm:7223-7235; contract at Doc/SystemGuide.md:1171-1200`
- **Here:** `src/hbios_dispatch.cc:1080`
- **Smallest fix:** Before masking, test bit 7 of D. If clear, compute lba = ((uint32_t)HL << 8) | ((D & 0x0F) << 4) | (E & 0x0F), exactly as HB_CHS2LBA does, and write the resolved value back into DE/HL for the caller as the drivers do.
- **Status:** fixed - CHS addressing is honoured

### File-backed BF_DIOWRITE has no end-of-media bound and grows the host image instead of failing  — FIXED

- **Function:** `BF_DIOWRITE (0x14)`
- **RomWBW:** A write beyond the capacity the driver reports through BF_DIOCAP fails: md.asm range-checks against the media size and returns ERR_IO, and the hardware drivers pass the device's out-of-range result back as ERR_IO. The medium never grows.
- **This dispatcher:** The file-backed loop computes offset = (lba + s) * 512 and calls emu_disk_write with no comparison against disks[hd_unit].size. emu_disk_write (emu_io_common.cc:249-266) seeks and fwrites, extending the host file and updating its own internal size, and returns 512 - so A = 0, E = the full count. The in-memory branch immediately below does bound the write and returns HBR_IO, so the two backings disagree about the same disk.
- **A guest would see:** A program doing raw HBIOS writes past the medium - a slice copier told a wrong slice count, say - gets success for sectors that do not exist. The host image silently grows (a seek to LBA 10,000,000 on an 8MB image leaves a 5GB file), while HBDisk::size is not updated, so BF_DIOCAP keeps reporting the original capacity and the data just written is unreachable. On real RomWBW the same call returns ERR_IO and the program stops.
- **Upstream:** `Source/HBIOS/md.asm:290-312 plus MD_IOSETUP3 (the media-size check that becomes ERR_IO); hdsk.asm:228-315, where the device's result code becomes ERR_IO`
- **Here:** `src/hbios_dispatch.cc:1314-1330 (file-backed), contrast 1336-1345 (in-memory)`
- **Smallest fix:** Bound the file-backed loop the way the in-memory one already is: if ((offset + 512) > disks[hd_unit].size) { emu_error(...); result = HBR_IO; break; } before calling emu_disk_write.
- **Status:** FIXED

### BF_DIODEVICE never sets H and L, so the device mode and I/O base come back as the caller's HL  — FIXED

- **Function:** `BF_DIODEVICE (0x17)`
- **RomWBW:** The function returns five values: C = attributes, D = device type, E = device number, H = device unit mode, L = device I/O base address. Both drivers set H and L explicitly on every call, including the memory-disk driver which sets them to zero rather than leaving them.
- **This dispatcher:** The handler sets D, E and C and returns. HL is never touched, so the guest gets back whatever it happened to have in HL when it executed RST 08 - and, being the caller's own value, it looks plausible rather than obviously wrong.
- **A guest would see:** Any tool that reports or dispatches on the hardware port behind a disk unit reads its own stale HL. Nothing in the RomWBW 3.6.0 Source tree consumes H/L from this function (invntdev.asm:98 uses only D/E/C, invntslc.asm:86 only C, romldr.asm:731,768 only C), so I did not find a stock caller; the exposure is to guest programs using the documented return.
- **Upstream:** `Source/HBIOS/hdsk.asm:189-196 ("LD H,0 ; DRIVER HAS NO MODES / LD L,HDSK_IO ; BASE I/O ADDRESS") and md.asm:240-248 ("LD H,0 / LD L,0"); contract at Doc/SystemGuide.md:1294-1352`
- **Here:** `src/hbios_dispatch.cc:1388-1435`
- **Smallest fix:** Add cpu->regs.HL.set_pair16(0x00FD) for the hard-disk branch (H=0, L=HDSK_IO, matching hdsk.asm since the emulator already reports DIODEV_HDSK) and cpu->regs.HL.set_pair16(0) for the memory-disk branch.
- **Status:** FIXED

### BF_EXTSLICE calls a bare 8MB image hd1k where RomWBW calls it hd512  — KEPT ON PURPOSE, MEASURED

- **Function:** `BF_EXTSLICE (0xE0), slice arithmetic`
- **RomWBW:** MID_HDNEW and the 16384-sector slice stride are reached only through EXT_SLICE3B, i.e. only when a partition entry of type $2E is found in the MBR. With no $2E entry RomWBW falls to EXT_SLICE3C, keeps the media ID that DIOMEDIA gave it (MID_HD, 4) and uses SPS_HD512 = 16640 sectors per slice. Image length plays no part in the decision.
- **This dispatcher:** After the $2E scan fails, "if (!detected_format && disk_size == 8388608)" declares hd1k anyway: partition_base_lba = 0, slice_size = 16384, is_hd1k = true, and C comes back MID_HDNEW (0x0A).
- **A guest would see:** For an 8MB image with no partition table the emulator answers C = 10 and RomWBW answers C = 4. CBIOS picks the DPB from that media ID, so the emulator mounts the volume with a 1024-entry directory and RomWBW with a 512-entry one - the same image read two different ways, and a directory written under one is misread under the other. This looks deliberate (it makes bare single-slice hd1k images usable), but it is a divergence from the authority and worth stating as one rather than leaving undocumented.
- **Upstream:** `Source/HBIOS/hbios.asm:5565-5590 (EXT_SLICE3C) with SPS_HD512 = $4100 and SPS_HD1K = $4000 at hbios.inc:379-380`
- **Here:** `src/hbios_dispatch.cc:2797-2803`
- **Smallest fix:** Either drop the size heuristic and follow EXT_SLICE3C, or keep it but gate it on something the guest cannot mistake for RomWBW behaviour and say so in the release notes. If it stays, note that slice 0 is unaffected - only the media ID and the stride for slice > 0 differ.
- **Status:** CONFIRMED against upstream, DELIBERATELY NOT FIXED. The first arm was tried - the heuristic was deleted and EXT_SLICE3C followed exactly - and `romwbw_disks/tools/boot_test.sh` went red on Z3PLUS for BOTH published releases. The finding names the mechanism itself: CBIOS picks its DPB from the media ID, so MID_HD makes it read a 1024-entry hd1k directory with 512-entry parameters. The published single-slice images are cut with romwbw_disks' `wbw_hd1k` diskdefs but carry a type-06 partition entry rather than a $2E one, so upstream's own rule would call them hd512 too - the heuristic is the only reason they mount here. It is wrong about RomWBW and right about the artifacts this emulator exists to run. Restored, with that reasoning in the code at the branch. The second arm of the fix is what was done: it is stated, here and there, rather than left undocumented.

### SYSGET BOOTINFO never sets L (boot bank ID), and SYSSET BOOTINFO discards the L it is given  — FIXED

- **Function:** `BF_SYSGET ($F8) / BF_SYSSET ($F9) subfunction BOOTINFO $E0`
- **RomWBW:** GET returns L = CB_BOOTBID (the ROM bank the boot was launched from), D = boot disk unit, E = boot disk slice. SET stores L into CB_BOOTBID and DE into CB_BOOTVOL, both in the HCB.
- **This dispatcher:** GET writes only D and E from saved_boot_unit/saved_boot_slice; L is left holding whatever the caller passed in. SET reads only D and E and writes CB_BOOTVOL at 0x010D/0x010E; the L it is handed is never stored, and CB_BOOTBID (HCB offset $0F, emu_hbios.asm:143) stays 0 forever.
- **A guest would see:** A loader that does SYSSET BOOTINFO with a bank in L and later reads it back with SYSGET BOOTINFO gets its own register contents instead, with A=0. On a ROM-app boot the bank ID is how a caller knows which ROM bank it came from; on the emulator that value is garbage rather than zero, so a caller cannot even detect "not recorded".
- **Upstream:** `hbios.asm:6253-6258 (SYS_GETBOOTINFO: "LD A,(CB_BOOTBID) / LD L,A / LD DE,(CB_BOOTVOL)"), 6528-6533 (SYS_SETBOOTINFO: "LD A,L / LD (CB_BOOTBID),A / LD (CB_BOOTVOL),DE"); Doc/SystemGuide.md:2964-2977 lists L: Boot Bank ID`
- **Here:** `hbios_dispatch.cc:1901-1909 (SYSGET_BOOTINFO), 2205-2224 (SYSSET_BOOTINFO)`
- **Smallest fix:** In SYSGET_BOOTINFO set `cpu->regs.HL.set_low(saved_boot_bank)`; in SYSSET_BOOTINFO capture `cpu->regs.HL.get_low()` into that field and write it to the HCB at 0x010F alongside the CB_BOOTVOL write already there.
- **Status:** fixed - the boot bank id is carried both ways

### SYSGET CPUSPD reports full speed and success; the shipped ROMs are SPD_FIXED Z80 builds where RomWBW returns an error  — FIXED

- **Function:** `BF_SYSGET ($F8) subfunction CPUSPD $F3`
- **RomWBW:** On the two stock ROMs this emulator is built over - SBC_simh_std (versions/3.6.0/version.json stock_rom) and RCZ80_std - SYS_GETCPUSPD falls past every conditional arm and returns A = $FF with NZ, leaving L, D and E untouched.
- **This dispatcher:** Returns A = 0 with H = 0, L = 1 ("full speed") and DE = $FFFF. The in-code comment attributes DE=$FFFF to "RomWBW's SBC path", but that path is compiled out unless CPUSPDCAP is SPD_HILO, which neither shipped configuration sets.
- **A guest would see:** A program that queries CPU speed attributes gets "clock multiplier 1, wait states unknown, success" where the real ROM says "unsupported". The direction matters: real RomWBW lets the caller fall back (print "n/a"), the emulator hands it a fabricated answer it will print as fact. Low-confidence caveat: I verified the two configs named above; if a future ROM with SPD_HILO or a Z180 were loaded, the emulator's answer would become the right shape.
- **Upstream:** `hbios.asm:6310-6385 (SYS_GETCPUSPD). The SBC/MBC arm needs CPUSPDCAP==SPD_HILO, the next arms need PLT_HEATH or CPU_Z180; cfg_SBC.asm:69 and cfg_RCZ80.asm:69 both set CPUSPDCAP SPD_FIXED and both are Z80, so control reaches hbios.asm:6384-6385: "OR $FF / RET"`
- **Here:** `hbios_dispatch.cc:2032-2040`
- **Smallest fix:** `result = HBR_FAILED;` (A=$FF, matching "OR $FF") and leave HL/DE alone - or, better, derive the answer from the loaded ROM's platform the way HBF_SYSVER already derives the version.
- **Status:** FIXED

### SYSGET SWITCH answers success with HL=0 for an illegal switch key; RomWBW returns A=$FF with NZ  — FIXED

- **Function:** `BF_SYSGET ($F8) subfunction SWITCH $C0`
- **RomWBW:** SWITCH_RES rejects any switch key above SWITCH_LEN (3) and returns A = $FF with NZ; SYS_GETSWITCH propagates that immediately with RET NZ. HL is not written.
- **This dispatcher:** Only 0xFF, 1 (NVSW_BOOTOPTS) and 3 (NVSW_AUTOBOOT) are handled. Every other key - including 4 through 0xFE, which the real code rejects - sets HL = 0, logs "Unknown switch %d, returning 0", and returns A = 0.
- **A guest would see:** A configuration tool that probes switch keys to discover which ones exist (the documented NZ-means-error convention is the only way to do that) sees every key from 0 to 0xFE answer success with value 0, instead of three valid keys and the rest refused. It would then offer to set switches that do not exist.
- **Upstream:** `hbios.asm:6171-6177 (SYS_GETSWITCH: "CALL SWITCH_RES / RET NZ"), 6490-6508 (SWITCH_RES; SWITCH_LEN is 3, so D>3 falls to SWITCH_RES1: "OR $FF / RET"); Doc/SystemGuide.md:2901-2921 ("Errors are signaled in the return by setting the NZ flag")`
- **Here:** `hbios_dispatch.cc:1969-1975 (the trailing `else` of the switch_num chain)`
- **Smallest fix:** In that `else`, `if (switch_num > 3) { result = HBR_FAILED; break; }` before the HL=0 default, so keys above SWITCH_LEN get A=$FF/NZ as SWITCH_RES does.
- **Status:** FIXED

### SYSSET SWITCH silently ignores an unknown switch key and reports success  — FIXED

- **Function:** `BF_SYSSET ($F9) subfunction SWITCH $C0`
- **RomWBW:** SYS_SETSWITCH routes $FF to NVSW_RESET, otherwise calls SWITCH_RES and returns A=$FF with NZ for any key it rejects; it also returns early (NZ) when CB_SWITCHES is 0 (no NVRAM) or when SYS_GETSWITCH3 says NVRAM is not fully initialised.
- **This dispatcher:** Keys other than 0xFF, 1 and 3 fall into `else { if (debug_log) emu_log("...ignoring"); }`. Nothing is written and `result` stays HBR_SUCCESS, so A = 0.
- **A guest would see:** A setup program writing a switch the emulator does not model is told the write succeeded, reads it back (which also succeeds, returning 0 - see the SYSGET SWITCH finding) and concludes the setting did not take for some other reason, or worse, that it is set to 0. Real RomWBW refuses the write with NZ so the caller knows.
- **Upstream:** `hbios.asm:6457-6481 (SYS_SETSWITCH: "CALL SWITCH_RES / RET NZ ; RETURN IF NZ - swich number illegal" at 6471-6472), SWITCH_RES1 at 6506-6508`
- **Here:** `hbios_dispatch.cc:2198-2203 (the trailing `else` of the SYSSET_SWITCH chain)`
- **Smallest fix:** `result = HBR_FAILED;` in that `else` arm, matching SWITCH_RES1's "OR $FF".
- **Status:** FIXED

### BF_RTCDEVICE puts the device type in C, leaves D/E as an accidental DS1302, and never sets H or L  — FIXED

- **Function:** `BF_RTCDEVICE (0x28)`
- **RomWBW:** Returns C = device attributes (undefined, drivers leave it alone), D = device type from the RTCDEV_* table, E = physical device number (always 0), H = unit mode, L = base I/O address. All four of D, E, H, L are written by every driver.
- **This dispatcher:** cpu->regs.BC.set_low(0x40) puts 0x40 in C - the attributes slot - and 0x40 is not a value in the RTCDEV_* table at all. cpu->regs.DE.set_pair16(0x0000) then makes D = 0x00 = RTCDEV_DS, so the emulator claims to be a Maxim DS1302 on dsrtc.asm. H and L are never written, so the caller gets its own HL back.
- **A guest would see:** A program that formats the device type from D prints "DS1302" for the emulator, and then reads H (mode) and L (I/O base) out of whatever it had in HL before the call - garbage, not a port number. I found no caller of BF_RTCDEVICE among the .COM files on the three published 3.6.0 images (no aligned `06 28 CF`), so the impact today is on any new or third-party inventory tool rather than on a shipped one; the register placement is wrong regardless.
- **Upstream:** `dsrtc.asm:413-419 (DSRTC_DEVICE: LD D,RTCDEV_DS / LD E,0 / LD H,DSRTCMODE / LD L,DSRTC_IO / XOR A); identical shape in ds7rtc.asm:303-309. Contract: SystemGuide.md:1546-1570.`
- **Here:** `hbios_dispatch.cc:1645-1650`
- **Smallest fix:** Set D to the device type, E=0, H=0 (no modes), L to a base address (0 is honest for an emulated device), and leave C alone or set it to 0. Pick a device type the guest can act on: RTCDEV_SIMH (0x02) is the simulator entry and is a better claim than 0x00 = DS1302.
- **Status:** FIXED

### NVRAM is only 5 bytes: RTCGETBYT returns 0 and RTCSETBYT discards, both with A=0, for every index above 4  — FIXED

- **Function:** `BF_RTCGETBYT (0x22) / BF_RTCSETBYT (0x23)`
- **RomWBW:** The DS1302 the emulator claims to be has 31 bytes of NVRAM at indexes 0-30; HBIOS uses only 0-4 for the switch block and leaves the rest to applications. A read of index 7 returns what was written to index 7; a write to index 30 sticks.
- **This dispatcher:** GETBYT with C >= 5 returns E = 0 and result HBR_SUCCESS. SETBYT with C >= 5 logs (only when debug_log is on) and returns HBR_SUCCESS having stored nothing. Neither path returns an error.
- **A guest would see:** Any program that keeps data in the RTC's NVRAM beyond the switch block silently reads back zeros. The concrete idiom is RomWBW's own presence test (dsrtc.asm:482): save index 30, write its complement, read it back, compare. On the emulator the read-back is 0, the compare fails and the program concludes the RTC is broken - while HBIOS's own answer to BF_SYSGET/RTCCNT is "1 RTC device" (hbios_dispatch.cc:1893).
- **Upstream:** `dsrtc.asm:383-409 (GETBYT/SETBYT index straight into the chip: SLA A; ADD A,$C1/$C0), and dsrtc.asm:482-508 (DSRTC_DETECT uses NVRAM index 30 as a scratch byte, which is only legal because the part has 31)`
- **Here:** `hbios_dispatch.cc:1570-1610 (bounds tests at 1577 and 1593 against NVRAM_SIZE = 5, hbios_dispatch.h:736)`
- **Smallest fix:** Widen nvram_switches to 31 bytes, keep the checksum over bytes 0-3 and the persisted setting over bytes 0-4 as now, and let 5-30 round-trip. Failing that, return HBR_RANGE (-6) for an out-of-range index instead of success.
- **Status:** FIXED

### SYSSET_TIMER and SYSSET_SECS write the same origin, so setting either counter moves the other  — FIXED

- **Function:** `BF_SYSSET/TIMER (0xF9/0xD0) and BF_SYSSET/SECS (0xF9/0xD1)`
- **RomWBW:** HB_TICKS and HB_SECS are independent 32-bit counters. SYS_SETTIMER stores into HB_TICKS and does not touch HB_SECS; SYS_SETSECS stores into HB_SECS and does not touch HB_TICKS or the HB_SECTCK sub-second downcounter.
- **This dispatcher:** SYSSET_TIMER does setTicks(want); SYSSET_SECS does setTicks(want * TICKFREQ). Both shift the single tick_origin, and SYSGET_SECS is derived from it as ticks/50 (hbios_dispatch.cc:2000-2007). So a SECS set rewrites the tick counter, and a TIMER set rewrites the seconds counter and the sub-second remainder.
- **A guest would see:** TIMER.COM ships on the published images and its reset command is exactly `06 F9 / 0E D1 / 11 00 00 / 21 00 00 / CF` at 0x233 - SYSSET_SECS with DE:HL = 0. On real RomWBW that zeroes the seconds display and leaves the HBIOS tick counter running; on the emulator it also snaps the tick counter back to zero. VGMPLAY.COM and TUNE.COM read that tick counter (aligned `06 F8 0E D0` / `01 D0 F8`) and measure deltas across it.
- **Upstream:** `hbios.asm:6544-6550 (SYS_SETTIMER: LD BC,HB_TICKS / ST32) and hbios.asm:6561-6567 (SYS_SETSECS: LD BC,HB_SECS / ST32); the two counters are separate storage, hbios.asm:9527-9529, and only the tick ISR at hbios.asm:7365-7394 couples them.`
- **Here:** `hbios_dispatch.cc:2137-2151 (both cases call setTicks(); hbios_dispatch.h:679)`
- **Smallest fix:** Keep two origins - one for ticks, one for seconds - or keep a tick origin plus a separate signed seconds bias. SYSSET_TIMER adjusts only the tick origin; SYSSET_SECS adjusts only the seconds bias; SYSGET_SECS returns bias + ticks/50 and C = ticks%50.
- **Status:** fixed - the two counters are independent, and no multiply

### SYSGET_SWITCH/SYSSET_SWITCH answer success for switch numbers RomWBW rejects, and answer 0 for two it serves  — FIXED

- **Function:** `BF_SYSGET/SWITCH and BF_SYSSET/SWITCH (0xC0)`
- **RomWBW:** D=0 returns HL = 0x0057 (the 'W' signature byte, byte count 0 in SWITCH_TAB so only the low byte is taken) with A=0. D=1 returns the word L=CB_SW_AB_OPT, H=the byte after it. D=2 returns HL = that second boot-options byte. D=3 returns the autoboot byte in L. D of 4 or more - byte 4 is the checksum, and anything past the table - returns A=$FF with NZ and HL untouched. SYS_SETSWITCH is the mirror: D=0 and D=2 write that byte and then NVSW_UPDATE, D >= 4 returns $FF.
- **This dispatcher:** The get path handles only 0xFF, 1 and 3; everything else takes the final else, sets HL = 0 and falls out with result HBR_SUCCESS. The set path handles only 0xFF, 1 and 3; everything else logs and falls out with HBR_SUCCESS having written nothing.
- **A guest would see:** A caller cannot distinguish "switch does not exist" from "switch is zero": on real RomWBW `LD D,4 / LD BC,$F8C0 / RST 08` comes back A=$FF NZ, on the emulator A=0 Z with HL=0. The set direction is the worse half - a write to a rejected switch number is acknowledged and lost. SYSCONF, the only shipped caller I found, uses D=1 and D=3 only (sysconf.asm:323, 389, 426, 472), so this bites probing or future code rather than SYSCONF today.
- **Upstream:** `hbios.asm:6490-6521 (SWITCH_RES; SWITCH_LEN = 3, so D of 0..3 is accepted and D >= 4 falls to SWITCH_RES1 -> OR $FF, NZ) with hbios.asm:6171-6199 (SYS_GETSWITCH) and hbios.asm:6457-6482 (SYS_SETSWITCH)`
- **Here:** `hbios_dispatch.cc:1969-1974 (get) and hbios_dispatch.cc:2196-2202 (set)`
- **Smallest fix:** Mirror SWITCH_RES: accept D of 0..3 and index nvram_switches[D] directly (byte for D=0,2,3; word L=[1],H=[2] for D=1), and return HBR_FAILED (0xFF, NZ) for any other D on both the get and the set path.
- **Status:** FIXED

### BF_SNDDEVICE (0x57) is not implemented at all and answers ERR_NOFUNC for a unit the emulator says exists  — FIXED

- **Function:** `BF_SNDDEVICE (0x57)`
- **RomWBW:** Returns C := device attributes (0), D := device type, E := device number, H := device unit mode, L := device I/O base address, A := 0. It is the sound-side twin of BF_VDADEV, which the emulator does implement.
- **This dispatcher:** HBR_NOFUNC (0xFD) with every register left as the caller passed it, while handleSYS (hbios_dispatch.cc:1889-1890, SYSGET_SNDCNT) reports that one sound unit exists.
- **A guest would see:** A program enumerating sound hardware through 0x57 — the same pattern the emulator supports for video through 0x43 — is told the unit exists and then that the function does not. On real RomWBW it gets a device type and a port address. At least the error is honest rather than a silent success.
- **Upstream:** `sn76489.asm:426-434 (SN7_DEVICE: D := device type, E := physical unit, C := $00 attributes, H := 0 mode, L := base I/O, XOR A) and ay38910.asm:499-507; both are entry 8 of the 9-entry driver table (hbios.asm:5155 SND_FNCNT .EQU 9, sn76489.asm:438-448); SystemGuide.md:2457-2480`
- **Here:** `hbios_dispatch.cc:2532-2660 — HBF_SNDDEVICE is defined in hbios_dispatch.h:128 but has no case, so it reaches the default at 2650-2657 and returns HBR_NOFUNC`
- **Smallest fix:** Mirror the VDADEV case: set D := the SNDDEV_* code chosen for the SNDQ_DEV fix, E := 0, C := 0, H := 0, L := 0, result stays HBR_SUCCESS.
- **Status:** FIXED

### BF_VDAKRD sets only E; C (Scancode) and D (Keystate) come back holding the caller's own input  — FIXED

- **Function:** `BF_VDAKRD (0x4E)`
- **RomWBW:** Returns three values: Keycode in E, Keystate bitmap in D (bit 0 Shift, bit 1 Ctrl, bit 2 Alt, bit 4 ScrollLock, bit 5 NumLock, bit 6 CapsLock, bit 7 numpad), Scancode in C, and A=0. A driver with no scancode support must return 0 in C, not leave it alone.
- **This dispatcher:** D and C are never written. C comes back as the video unit number the caller put there for the call itself, and D as whatever the caller happened to have in it — so the "keystate" changes with the call site rather than with the keyboard.
- **A guest would see:** A program that reads the keyboard through the video unit and tests D bit 1 to distinguish Ctrl-key chords, or reads C to tell an arrow key from its ASCII escape, gets stable garbage: C is always the unit number (0), and D is uninitialised. On real hardware both are meaningful.
- **Upstream:** `ppk.asm:135-152 (PPK_READ: C := scancode with the extended bit, D := PPK_STATE modifier flags, E := keycode, XOR A) and the identical kbd.asm:239-258; SystemGuide.md:2151-2195, including "If the driver does not implement this, it should return 0 in C" and the Keystate bit table`
- **Here:** `hbios_dispatch.cc:2467-2505 — the only register written is cpu->regs.DE.set_low(ch) at 2500`
- **Smallest fix:** Before the break, add cpu->regs.BC.set_low(0); and cpu->regs.DE.set_high(0); — the honest "no scancode, no modifier information" answer the manual prescribes.
- **Status:** FIXED

### BF_VDARDC answers SUCCESS with a hard-coded space and leaves B and C unset  — FIXED

- **Function:** `BF_VDARDC (0x4F)`
- **RomWBW:** E := the character actually stored at the cursor position, B := the colour ($F0 on a monochrome unit), C := the attribute byte ($00), A := 0.
- **This dispatcher:** E := 0x20 unconditionally — the in-line comment says "not implemented" but A is still 0. B is left holding 0x4F (the function code the caller put in B for this very call) and C the unit number, so both are read back as data.
- **A guest would see:** A screen-scraping or save-under routine — a menu that restores the text it covered, or a program that reads a field back off the display — writes spaces over live text on the emulator and takes 0x4F for a colour byte. Because A=0 the caller cannot defend against it; this is the answer-success-while-doing-nothing case.
- **Upstream:** `vdu.asm:285-311 (VDU_VDARDC: reads the cell from video RAM into E, then LD B,$F0 ; WHITE FG. BLACK BG / LD C,$00 ; NO ATTRIBUTES / XOR A); SystemGuide.md:2196-2214 ("If the display does not support colors or attributes then this function will return color white on black with no attributes")`
- **Here:** `hbios_dispatch.cc:2506-2510 (cpu->regs.DE.set_low(' '); with result left at HBR_SUCCESS)`
- **Smallest fix:** At minimum set cpu->regs.BC.set_high(0xF0); cpu->regs.BC.set_low(0x00);. Then either add a read-back to emu_io.h (it has emu_video_write_char_at but no emu_video_read_char_at) and return the real cell, or set result = HBR_NOTIMPL instead of reporting success.
- **Status:** FIXED

### BF_VDASAT stores the attribute byte in the same variable as the colour and hands it to the front end as a colour  — FIXED

- **Function:** `BF_VDASAT (0x46)`
- **RomWBW:** E is a three-bit Reverse/Underline/Blink bitmap. It is state distinct from the character colour: setting the attribute does not disturb the colour and setting the colour does not disturb the attribute.
- **This dispatcher:** E is stored into vda_attr, the same byte VDASCO writes, and passed to emu_video_set_attr(), which the front ends treat as a CGA colour byte. So VDASAT with E=0x04 (reverse video) becomes colour 0x04 (red on black), and a subsequent VDASCO clears the reverse/underline state entirely.
- **A guest would see:** A program that highlights a menu line by asking for reverse video gets red text instead, and loses whatever colour was in force. Conversely a program that sets a colour silently drops the reverse attribute it set a moment earlier.
- **Upstream:** `vga.asm:259-266 (VGA_VDASAT: "; INCOMING IS: -----RUB (R=REVERSE, U=UNDERLINE, B=BLINK)", saved to VGA_RUB, which is separate state from VGA_ATTR, the colour); cvdu.asm:225-231 keeps the colour in the low nibble and the attribute bits in the high nibble of one byte; SystemGuide.md:2020-2034 and the attribute bit table at SystemGuide.md:1857-1864 (bit 2 Reverse, bit 1 Underline, bit 0 Blink, bits 3-7 n/a)`
- **Here:** `hbios_dispatch.cc:2370-2375 (vda_attr = cpu->regs.DE.get_low(); emu_video_set_attr(vda_attr);), and the same vda_attr is overwritten by VDASCO at 2382`
- **Smallest fix:** Keep two variables, vda_color (VDASCO) and vda_rub (VDASAT), and combine them when handing state to the front end — or add a separate emu_video_set_rub() to emu_io.h so reverse/underline/blink are expressible at all.
- **Status:** FIXED

### Neither VDA nor SND validates the unit number in C, so unit 7 answers as though it existed  — FIXED

- **Function:** `all BF_VDA* (0x40-0x4F) and BF_SND* (0x50-0x58)`
- **RomWBW:** Every VDA and SND call with C greater than or equal to the unit count returns ERR_NOUNIT before the driver is reached. With one video unit and one sound unit configured, only C=0 succeeds.
- **This dispatcher:** C is never range-checked. BF_VDAQRY with C=9 returns A=0 and a valid 25x80 geometry. BF_SNDVOL with C=7 falls through the `channel < 4` guard, writes nothing, and still returns A=0 — success while doing nothing. Meanwhile handleSYS reports VDACNT=1 (hbios_dispatch.cc:1885-1887) and SNDCNT=1 (1889-1891).
- **A guest would see:** A program that enumerates units the usual way — call BF_VDAQRY with C=0,1,2,... until A is negative — never terminates on the emulator and reports 256 identical displays, where on real hardware it stops after unit 0. And a sound program that mistakenly addresses unit 7 is told its volume was set.
- **Upstream:** `hbios.asm:7448-7452 (HB_DISPCALC: "CHECK INCOMING UNIT INDEX IN C FOR VALIDITY / LD A,C / CP (IY-1) ; COMPARE TO COUNT / JR NC,HB_UNITERR"), reached from VDA_DISPATCH at hbios.asm:5086-5092 and SND_DISPATCH at hbios.asm:5129-5135`
- **Here:** `hbios_dispatch.cc:2299-2530 (handleVDA never reads C except to echo it back at 2331) and 2532-2660 (handleSND uses C only as an array index, guarded by `channel < 4` at 2559/2565/2577)`
- **Smallest fix:** At the top of handleVDA and handleSND, `if (cpu->regs.BC.get_low() != 0) { setResult(HBR_NOUNIT); doRet(); return; }` — matching the single unit each group reports through SYSGET.
- **Status:** FIXED

### Three VDA functions RomWBW implements are absent: VDASCS (0x44), VDACPY (0x4A) and VDAKFL (0x4D)  — FIXED

- **Function:** `BF_VDASCS (0x44), BF_VDACPY (0x4A), BF_VDAKFL (0x4D)`
- **RomWBW:** All three return A=0. VDASCS sets the cursor shape from the nibbles of D (drivers with no adjustable cursor, e.g. tvga.asm, still succeed). VDACPY copies Count (L) cells from source row/col D/E to the cursor position without moving the cursor. VDAKFL purges the keyboard buffer.
- **This dispatcher:** HBR_NOFUNC (0xFD) with no side effect. This is at least an honest error rather than a silent success, but real RomWBW succeeds on every unit it ships.
- **A guest would see:** VDAKFL is the one that bites: a program that flushes type-ahead before an "Are you sure?" prompt gets an error and then reads the buffered keystroke as the answer. VDACPY is the block move a full-screen editor uses to scroll a window region (RomWBW's own ansi.asm:1112 and 1163 call it); VDASCS is the cursor shape an editor toggles between insert and overwrite mode.
- **Upstream:** `vdu.asm:213-230 (VDU_VDASCS programs the 6845 cursor start/end registers and returns XOR A), vdu.asm:259-266 (VDU_VDACPY -> VDU_BLKCPY block move), ppk.asm:158-162 (PPK_FLUSH clears PPK_STATUS, XOR A); all sixteen codes are valid on every unit because VDA_FNCNT is 16 (hbios.asm:5112) and each driver's table has exactly sixteen entries (vdu.asm:161-179); SystemGuide.md:1982-2004, 2086-2103, 2140-2149`
- **Here:** `hbios_dispatch.cc:2306-2511 has no case for 0x44, 0x4A or 0x4D, so all three reach the default at 2512-2521 and return HBR_NOFUNC`
- **Smallest fix:** VDAKFL: drain emu_console pending input and return HBR_SUCCESS. VDASCS: accept and ignore D/E and return HBR_SUCCESS (tvga.asm is the precedent for a device with no adjustable cursor). VDACPY needs a cell-level read-back in emu_io.h; until that exists, HBR_NOTIMPL describes it better than HBR_NOFUNC, which claims the function code does not exist.
- **Status:** FIXED

### A function number in no group returns 0xFF (ERR_UNDEF) where RomWBW returns 0xFD (ERR_NOFUNC)  — FIXED

- **Function:** `HB_DISPATCH / getTrapTypeFromFunc — function numbers $60-$DF`
- **RomWBW:** A=ERR_NOFUNC = -3 = 0xFD, flags from `OR A` (NZ, S set, carry clear).
- **This dispatcher:** A=0xFF = ERR_UNDEF (-1), Z cleared.
- **A guest would see:** Both are non-zero, so a caller that only tests Z behaves the same. A caller that distinguishes the codes — the documented set in SystemGuide's Result Codes table, where -1 is "undefined error" and -3 is "invalid function" — reads a probe of an unimplemented group as a device malfunction rather than as "this HBIOS does not have that function", and may retry or abort instead of falling back. The same 0xFF is the emulator's own generic-failure code, so the two become indistinguishable.
- **Upstream:** `hbios.asm:4508-4526 (the CP ladder; `CP BF_EXT / JR C,HB_DISPERR` sends $60-$DF to HB_DISPERR) and hbios.asm:4528-4530 (HB_DISPERR: SYSCHKERR(ERR_NOFUNC) / RET)`
- **Here:** `hbios_dispatch.cc:596-607 (getTrapTypeFromFunc returns -1 for $60-$DF) and hbios_dispatch.cc:626-628 (`setResult(HBR_FAILED)`, HBR_FAILED = 0xFF)`
- **Smallest fix:** hbios_dispatch.cc:627: `setResult(HBR_NOFUNC);` — the constant already exists at hbios_dispatch.h:31.
- **Status:** FIXED

### EXTSLICE returns B = 0x00 instead of the unit's Device Attributes  — FIXED

- **Function:** `BF_EXTSLICE (0xE0)`
- **RomWBW:** B carries the same attribute byte BF_DIODEVICE reports for that unit: for a hard disk 0x30 (bit 5 high-capacity, bit 4 LBA capable), for the ROM/RAM memory disks 0x14/0x15 (bit 4 LBA capable, media type 4=ROM / 5=RAM).
- **This dispatcher:** B is always 0x00 on every path — success, ERR_RANGE and ERR_NOUNIT alike. 0x00 decodes as: not a floppy, not removable, not high-capacity, NOT LBA-capable, media type 0 (plain hard disk).
- **A guest would see:** A caller that takes Device Attributes from EXTSLICE — the manual names it as the source, so it need not call DIODEVICE separately — sees every unit as non-LBA and non-high-capacity. That is the same bit-4 failure the emulator's own DIODEVICE comment records at hbios_dispatch.cc ("bit 4 says LBA capable, which is what CBIOS requires before it will put a unit in the drive map at all ... ASSIGN /B= silently rebuilt the drive map without them"), and the emulator now contradicts itself: DIODEVICE says 0x30 for the same unit that EXTSLICE calls 0x00.
- **Upstream:** `hbios.asm:5405-5413 (EXT_SLICE calls BF_DIODEVICE and stores C into SLICE_DEVATT), 5641-5643 (error return loads SLICE_DEVATT into B) and 5669-5672 (EXT_SLICE6Z success return does the same); documented at Doc/SystemGuide.md:2507 and 2529 ("The Device Attributes (B) are the same as defined in Function 0x17 -- Disk Device (DIODEVICE)")`
- **Here:** `hbios_dispatch.cc:2823 (`uint8_t dev_attrs = 0x00;`) and hbios_dispatch.cc:2988 (`cpu->regs.BC.set_high(dev_attrs)`) — dev_attrs is never assigned again on any path`
- **Smallest fix:** Set dev_attrs from the same expressions handleDIO's HBF_DIODEVICE uses — 0x14/0x15 for md_disks[md_unit].is_rom, 0x30 for a hard disk — and leave it 0x00 only on the no-unit path. Better still, factor the attribute byte into one helper both cases call, as RomWBW does by dispatching to BF_DIODEVICE.
- **Status:** FIXED

### EXTSLICE rejects slice 0 on an hd512 medium smaller than 16640 sectors, where RomWBW always succeeds  — FIXED

- **Function:** `BF_EXTSLICE (0xE0)`
- **RomWBW:** On a medium with no 0x55AA/0x2E partition table, slice 0 skips the fit check entirely: A=0, C=MID_HD, DE:HL=0, whatever the medium's size.
- **This dispatcher:** slice_size defaults to 16640 (SPS_HD512), so slice 0 needs 16640 sectors (8.5 MB) to pass. On a smaller image the call returns A=ERR_RANGE with C=0 (MID_NONE).
- **A guest would see:** A hard-disk image smaller than about 8.5 MB that is not exactly 8388608 bytes (the exact-8MB case is caught by the hd1k single-slice branch at hbios_dispatch.cc:2917) becomes unbootable: romldr's slice inventory (Source/HBIOS/invntslc.asm:132-137, which does `rst 08` on BF_EXTSLICE and `ret NZ`) reports nothing for the unit, and MID_NONE reads as "no media" rather than "small disk". On real RomWBW the same image boots from slice 0.
- **Upstream:** `hbios.asm:5573-5577 (EXT_SLICE3C: "IF SLICE = 0, WE BOOT THE DISK ITSELF. IGNORE SLICE(S) ... BYPASS ALL CALCS / CHECKS" → JR Z,EXT_SLICE5Z) and 5648-5650 (EXT_SLICE5Z sets DE:HL=0 and falls into EXT_SLICE6, where SLICE_LBAOFF is zero so the capacity comparison is 0 vs MEDSIZ and always passes)`
- **Here:** `hbios_dispatch.cc:2944-2965 — slice_end_sector is computed as start + slice_size unconditionally, including for slice 0, and compared against the medium at hbios_dispatch.cc:2953`
- **Smallest fix:** Skip the fit check when `slice == 0` and no 0x2E partition was found, matching EXT_SLICE3C's `JR Z,EXT_SLICE5Z`; return LBA 0 with MID_HD.
- **Status:** FIXED

### SYSGET_SWITCH and SYSSET_SWITCH answer an unknown switch number with SUCCESS instead of an error  — FIXED

- **Function:** `BF_SYSGET (0xF8) subfunction 0xC0 and BF_SYSSET (0xF9) subfunction 0xC0, for switch numbers outside 0x01/0x03/0xFF`
- **RomWBW:** SWITCH_RES rejects any switch number above the table length (3), returning A=0xFF with NZ, which SYS_GETSWITCH and SYS_SETSWITCH propagate straight out as the call's status.
- **This dispatcher:** Get sets HL=0 and reports A=0 success; Set changes nothing and reports A=0 success. Both only log under debug_log.
- **A guest would see:** SystemGuide.md:2759-2761 reserves switches 0x04-0xFE for "future general usage". A configuration tool that probes for a newer switch reads "present, value 0" instead of "no such switch", and a tool that writes one is told the write stuck. On real RomWBW both calls fail cleanly. Lower than the SYSGET default-arm finding because no shipped program uses a switch outside 0x01/0x03 today.
- **Upstream:** `hbios.asm:6490-6507 (SWITCH_RES: `LD A,SWITCH_LEN / CP D / JR C,SWITCH_RES1` and SWITCH_RES1 does `OR $FF ; signal failure`); 6171-6177 (SYS_GETSWITCH calls SWITCH_RES and `RET NZ`); 6457-6480 (SYS_SETSWITCH refuses when CB_SWITCHES is 0, and otherwise runs the same SWITCH_RES validation)`
- **Here:** `hbios_dispatch.cc:2088-2093 (get: "Unknown switch %d, returning 0", HL=0, result untouched); hbios_dispatch.cc:2316-2321 (set: "Unknown switch %d, ignoring", result untouched)`
- **Smallest fix:** Return HBR_UNDEF (0xFF, what SWITCH_RES produces) for a switch number that is not 0x01, 0x03 or 0xFF, in both the get and the set path.
- **Status:** FIXED


## Low severity (18)

### BF_EXTSLICE returns B = 0 instead of the device attributes  — FIXED

- **Function:** `BF_EXTSLICE (0xE0)`
- **RomWBW:** EXT_SLICE begins by calling BF_DIODEVICE, stores the attribute byte in SLICE_DEVATT, and returns it in B on every exit path - the success path at 5697 and the ERR_RANGE path at 5642 alike. For a hard disk that is %00110000 (0x30), for a memory disk 0x14 or 0x15.
- **This dispatcher:** dev_attrs is initialised to 0x00 and never assigned; B is set from it unconditionally at the end of the handler. The emulator's own BF_DIODEVICE (hbios_dispatch.cc:1414, 1423) gets these bytes right, so the value is available and simply is not used here.
- **A guest would see:** A caller that reads B from EXTSLICE rather than making a second DIODEVICE call is told the unit is not a floppy (bit 7, correct by luck), not removable, and not high-capacity (bit 5 clear) - so a 49MB hard disk fails exactly the "bit 5,C / high capacity?" test that invntslc.asm:89-92 and romldr.asm:734 apply to the DIODEVICE answer. Neither of those two reads B from EXTSLICE, so I found no stock caller that trips on it; it is a documented return value that is wrong.
- **Upstream:** `Source/HBIOS/hbios.asm:5383 ("B: DEVICE ATTRIBUTES, as reported by DIODEVICE"), captured at 5414 and returned at 5642 and 5697 ("LD A,(SLICE_DEVATT) / LD B,A"); contract at Doc/SystemGuide.md:2507`
- **Here:** `src/hbios_dispatch.cc:2704 (dev_attrs fixed at 0x00) and 2869 (returned in B)`
- **Smallest fix:** Set dev_attrs from the same source BF_DIODEVICE uses: 0x30 for a hard disk, md_disks[idx].is_rom ? 0x14 : 0x15 for a memory disk, 0x00 only on the no-unit path.
- **Status:** FIXED

### Unit numbers RomWBW answers with ERR_NOUNIT are served as real disks  — FIXED

- **Function:** `all BF_DIO* (unit mapping)`
- **RomWBW:** The disk unit number in C is an index into DIO_TBL, bounds-checked against the live entry count before any driver is reached. Units are plain 0..n-1 (DIO_MAX = 16). Anything at or above the count returns ERR_NOUNIT from the dispatcher; there is no high-bit or nibble encoding of unit numbers anywhere in HBIOS.
- **This dispatcher:** map_md_unit additionally accepts 0x80-0x8F (low nibble, capped at MD1) and 0xC0-0xCF (mapped to MD1), and map_hd_unit additionally accepts 0x90-0x9F (low nibble as the disk index). So unit 0x80 reads MD0, 0xC3 reads MD1, and 0x92 reads hard disk 2 - all with A = 0.
- **A guest would see:** A guest that walks unit numbers past the count BF_SYSGET/DIOCNT reported, or that corrupts C, finds phantom disks that answer successfully and alias real ones: writing to unit 0x92 and to unit 4 hit the same image. On real RomWBW every one of those calls returns ERR_NOUNIT. The reverse risk is the one that matters - a program that would have stopped instead proceeds.
- **Upstream:** `Source/HBIOS/hbios.asm:7448-7452 (HB_DISPCALC: "LD A,C / CP (IY-1) ; COMPARE TO COUNT / JR NC,HB_UNITERR") and 7495-7497`
- **Here:** `src/hbios_dispatch.cc:986-999 (map_md_unit) and 1003-1013 (map_hd_unit)`
- **Smallest fix:** Drop the 0x80-0x8F, 0x90-0x9F and 0xC0-0xCF ranges from the two mappers and return 0xFF for anything at or above the unit count the dispatcher reports, so the emulator's unit space is exactly RomWBW's 0..n-1.
- **Status:** FIXED

### BF_DIOSTATUS writes E, which RomWBW leaves alone, and always reports ready  — FIXED

- **Function:** `BF_DIOSTATUS (0x10), with BF_DIOMEDIA's error path`
- **RomWBW:** DIOSTATUS returns its answer in A only; neither driver touches D or E. HDSK also returns a per-unit stored status, which HDSK_INITDEV (hdsk.asm:113-121) sets to HDSK_STNOTRDY (-1) at init and which stays -1 until a reset or a successful transfer clears it.
- **This dispatcher:** The handler writes E = 0x00 on the ready path and E = 0xFF with HBR_NOUNIT otherwise, so E is destroyed on a function documented to return only A. It also answers 0 (ready) unconditionally for an open unit.
- **A guest would see:** A caller that keeps a value in E across a status poll loses it - the register is not in the function's return set, so nothing warns it. The always-ready part is the weaker half of this: it matches md.asm exactly, and differs from hdsk.asm only in the window between boot and the first I/O, and since the emulator has no interface to be not-ready I am not confident the -1 is worth reproducing. The E clobber I am confident about.
- **Upstream:** `Source/HBIOS/md.asm:186-194 (MD_STATUS falls straight into MD_RESET: "XOR A / RET") and hdsk.asm:152-155 ("LD A,(IY+HDSK_STAT) / OR A / RET"); contract at Doc/SystemGuide.md:1136-1150`
- **Here:** `src/hbios_dispatch.cc:1054 and 1057 (and the same clobber on BF_DIOMEDIA's no-unit path at 1448)`
- **Smallest fix:** Leave DE untouched in BF_DIOSTATUS - set only the result code - and likewise on BF_DIOMEDIA's no-unit path, where RomWBW never reaches the driver and so never writes E.
- **Status:** FIXED

### SYSRESET accepts any subfunction code; RomWBW rejects anything above 3, and subfunction 3 has a side effect the emulator does not perform  — FIXED

- **Function:** `BF_SYSRESET ($F0) - BF_SYSRES_USER $03 and out-of-range codes`
- **RomWBW:** C must be 0, 1, 2 or 3; any other value returns ERR_NOFUNC (-3) with NZ. C=3 (user reset) resets the active video display via TERM_RESET and returns with HL still holding the user reset vector the caller passed in.
- **This dispatcher:** The handler acts on 0, 1 and 2 and then falls out of the switch with `result` still HBR_SUCCESS, so C=3 and C=4..255 alike return A=0 having done nothing. No terminal/VDA reset is performed for C=3.
- **A guest would see:** Two small divergences. A caller passing a bad subfunction is told it succeeded instead of ERR_NOFUNC. And the user-reset path (what a jump to $0000 turns into) does not reset the terminal, so a program that died with the display left in a graphics or odd-attribute state leaves it that way on the emulator where real RomWBW clears it. I am marking this low because I could not confirm which guest reaches C=3 on this build - the Z180 invalid-opcode branch above it is compiled out on a Z80 ROM, leaving only the plain TERM_RESET tail.
- **Upstream:** `hbios.asm:5721-5732 (SYS_RESET compares C against exactly $00/$01/$02/$03, then SYSCHKERR(ERR_NOFUNC)), 5837-5845 (SYS_RESUSER3: "CALL TERM_RESET" then RET)`
- **Here:** `hbios_dispatch.cc:1679-1707`
- **Smallest fix:** Add `else if (reset_type != 0x03) { result = HBR_NOFUNC; }` after the existing arms, and have the 0x03 case call whatever the emulator uses for a VDA/terminal reset.
- **Status:** FIXED

### SYSALLOC omits the 4-byte block header RomWBW charges to the heap  — FIXED

- **Function:** `BF_SYSALLOC ($F6)`
- **RomWBW:** Each allocation consumes size + 4 bytes of heap and returns a pointer just past a 4-byte header holding the requested size and the caller's return address. The out-of-space test is made against size + 4, and also fails if the new top reaches $8000 (BIT 7,H).
- **This dispatcher:** heap_ptr advances by exactly `size`, with no header written and none charged.
- **A guest would see:** The emulator's heap holds 4 more bytes per allocation than the real one, so the point at which allocation starts failing differs. The direction is permissive, which is why this is low: a guest that fits on real hardware also fits here. It matters the other way round - the emulator will not reproduce an "*** Insufficient HBIOS Heap Memory ***" that a user hits on real hardware, so that class of bug cannot be diagnosed here. I am not claiming any guest reads the header itself; nothing in this tree does.
- **Upstream:** `hbios.asm:7541-7600 (HB_ALLOC: "LD DE,4 ; SIZE OF HEADER / ADD HL,DE" then writes size LSB/MSB and the caller's return address into the four bytes before the returned pointer)`
- **Here:** `hbios_dispatch.cc:1808-1840 (specifically the `heap_ptr + size <= heap_end` test at 1823 and `heap_ptr += size`)`
- **Smallest fix:** Charge size + 4 in both the bounds test and the bump, return heap_ptr + 4, and write the size word into the two bytes before it (the reference-address word has no meaningful value in the emulator; zero is fine).
- **Status:** FIXED

### BF_RTCGETBLK/SETBLK move 5 bytes and report success; no 3.6.0 driver behaves that way  — FIXED

- **Function:** `BF_RTCGETBLK (0x24) / BF_RTCSETBLK (0x25)`
- **RomWBW:** On eleven of the twelve 3.6.0 RTC drivers the call returns A = ERR_NOTIMPL (-2) with NZ and touches nothing. On the twelfth it transfers the whole 256-byte NVRAM through HL.
- **This dispatcher:** GETBLK writes exactly NVRAM_SIZE = 5 bytes to the buffer at HL and returns HBR_SUCCESS; SETBLK reads exactly 5 and returns HBR_SUCCESS. There is no size in any register, so the caller has no way to learn it got 5.
- **A guest would see:** A guest that sizes its buffer the way the one real implementation does gets 5 bytes written and 251 bytes of its own stale memory, with A=0 telling it the read was complete. No .COM on the three published 3.6.0 images calls it (no aligned `06 24 CF` or `06 25 CF`), so I have no shipped victim to name - hence low.
- **Upstream:** `dsrtc.asm:297-302 (GETBLK/SETBLK -> SYSCHKERR(ERR_NOTIMPL)); same in pcrtc.asm:194, ds7rtc.asm:291, ds12rtc.asm:153, bqrtc.asm:193, simrtc.asm:86, intrtc.asm:85, rp5rtc.asm:271, ez80rtc.asm:164, mmrtc.asm:146, ds5rtc.asm:295. The single exception, ds1501rtc.asm:326-350, moves 256 bytes (LD B,0 / INIR).`
- **Here:** `hbios_dispatch.cc:1611-1643`
- **Smallest fix:** Return HBR_NOTIMPL (-2) for both, matching the driver the emulator claims to be in BF_RTCDEVICE. If block access is wanted later, move the whole modelled NVRAM, not just the switch bytes.
- **Status:** FIXED

### The alarm functions return ERR_NOFUNC where every RomWBW driver returns ERR_NOTIMPL  — FIXED

- **Function:** `BF_RTCGETALM (0x26) / BF_RTCSETALM (0x27)`
- **RomWBW:** 0x26 and 0x27 are defined functions that the driver declines: A = -2 (ERR_NOTIMPL), NZ. 0x29-0x2F are undefined: A = -3 (ERR_NOFUNC).
- **This dispatcher:** The default arm returns HBR_NOFUNC (-3) for 0x26, 0x27 and 0x29-0x2F alike, so a defined-but-unimplemented function is reported as a nonexistent one.
- **A guest would see:** A caller that distinguishes "this RTC has no alarm" (-2) from "your HBIOS is too old to know this function" (-3) draws the wrong conclusion. Both are errors and both are NZ, so nothing silently succeeds; that is why this is low. No shipped caller found on the 3.6.0 images.
- **Upstream:** `dsrtc.asm:299-302 (GETALM/SETALM -> SYSCHKERR(ERR_NOTIMPL) = -2); ERR_NOTIMPL and ERR_NOFUNC are hbios.inc:221-222. ERR_NOFUNC is what the driver's dispatcher returns only for a subfunction outside 0-8 (dsrtc.asm:292).`
- **Here:** `hbios_dispatch.cc:1653-1660`
- **Smallest fix:** Give HBF_RTCGETALM and HBF_RTCSETALM their own case returning HBR_NOTIMPL, and leave the default arm returning HBR_NOFUNC for 0x29-0x2F.
- **Status:** FIXED

### RTCSETBYT and RTCSETBLK silently rewrite NVRAM byte 4, a side effect real NVRAM does not have  — FIXED

- **Function:** `BF_RTCSETBYT (0x23) / BF_RTCSETBLK (0x25)`
- **RomWBW:** A write to NVRAM index 1 changes index 1. The checksum in index 4 is whatever was last written there; HBIOS recomputes it in NVSW_UPDATE and writes it as a fifth, separate RTCSETBYT call, so an inconsistent block is a state real NVRAM can hold.
- **This dispatcher:** Writing any of indexes 0-3 recomputes index 4 from the array and the ROM's two version bytes, and RTCSETBLK overwrites the guest's byte 4 with a recomputed one after copying all five. The block can therefore never be inconsistent.
- **A guest would see:** A guest that writes one switch byte and reads index 4 back sees a value it did not write. The practical consequence is that NVRAM cannot be left deliberately corrupt: on real hardware a bad checksum makes HBIOS fall back to defaults on the next boot (hbios.asm:3733-3741), on the emulator it repairs itself. HBIOS's own NVSW_WRITE writes bytes 0,1,2,3,4 in order and ends with its own checksum, so the normal path lands on the same value and is unaffected - which is why this is low, and I have not found a program that depends on the difference.
- **Upstream:** `dsrtc.asm:399-409 (SETBYT writes the one indexed cell and nothing else) and hbios.asm:8156-8174 (NVSW_WRITE, which is what computes and writes the checksum byte - in the caller, not in the driver; the checksum itself is hbios.asm:8108-8120)`
- **Here:** `hbios_dispatch.cc:1596-1598 and hbios_dispatch.cc:1636, both calling recalcNvramChecksum() at hbios_dispatch.cc:697-721`
- **Smallest fix:** Store exactly what RTCSETBYT/RTCSETBLK were given. Move the checksum recomputation to the places that own the switch block - the SYSSET_SWITCH handler, which is HBIOS's NVSW_UPDATE equivalent, and the --boot/persisted-setting entry point - not to the raw byte writers.
- **Status:** FIXED

### SYSSET_SWITCH forces the NVRAM signature to 'W'; real HBIOS refuses the write until NVRAM is initialised  — FIXED

- **Function:** `BF_SYSSET/SWITCH (0xF9/0xC0)`
- **RomWBW:** Only D=$FF (reset to defaults) may run against an uninitialised block. A set of switch 1 or 3 while CB_SWITCHES is 1 ("NVRAM present, not configured") returns A=1 with NZ and writes nothing; the caller must reset first.
- **This dispatcher:** Both the BOOTOPTS and AUTOBOOT arms assign nvram_switches[0] = 'W' before storing, so the first set of any switch also initialises the block, and the call always succeeds.
- **A guest would see:** On a first run with fresh NVRAM (nvram_switches starts {0,'H',BOPTS_ROM,0,0}, hbios_dispatch.h:737, so the status byte is 0 = uninitialised), SYSCONF's set commands succeed on the emulator and fail on real RomWBW until the user runs the reset. A user following the emulator's behaviour would find the same sequence rejected on hardware. Low because on real systems NVRAM is normally already initialised.
- **Upstream:** `hbios.asm:6457-6465 (SYS_SETSWITCH: if CB_SWITCHES == 0 -> SWITCH_RES1, A=$FF NZ; then CALL SYS_GETSWITCH3 / RET NZ, so anything other than 'W' aborts the set) with hbios.asm:6201-6204`
- **Here:** `hbios_dispatch.cc:2172 and hbios_dispatch.cc:2186 (`nvram_switches[0] = 'W'; // Ensure initialized`)`
- **Smallest fix:** Return HBR_FAILED (0xFF, NZ) from the switch-1 and switch-3 arms when nvram_switches[0] != 'W', and leave the 0xFF reset arm as the only path that writes the signature.
- **Status:** FIXED

### SYSSET_SECS multiplies seconds by 50 in 32 bits and overflows  — FIXED

- **Function:** `BF_SYSSET/SECS (0xF9/0xD1)`
- **RomWBW:** The full 32-bit seconds value the guest passes is stored, and SYSGET_SECS returns it back unchanged.
- **This dispatcher:** want is uint32_t and TICKFREQ is 50, so the product wraps for any want above 85,899,345 (about 2.7 years of seconds). setTicks then also computes ticks*1000/50 as a long long from the wrapped value, so SYSGET_SECS afterwards returns a number unrelated to what was set.
- **A guest would see:** A guest that seeds the seconds counter from a Unix-style epoch value, or from any value above ~86 million, reads back a different number. TIMER.COM only ever sets zero, so nothing shipped trips this - it is a latent wrong-width bug rather than an observed one, hence low.
- **Upstream:** `hbios.asm:6561-6567 (SYS_SETSECS stores the caller's 32-bit DE:HL into HB_SECS verbatim, no arithmetic)`
- **Here:** `hbios_dispatch.cc:2146-2151 (`setTicks(want * TICKFREQ);`)`
- **Smallest fix:** Hold the seconds counter separately from the tick counter (the same change that fixes the TIMER/SECS coupling finding) so no multiplication is needed; if a product is kept, compute it in uint64_t and clamp.
- **Status:** fixed - seconds are their own offset, with no multiply to overflow

### setResult writes A and the Z flag only; HBIOS also sets S and clears C on every return  — FIXED

- **Function:** `all RTC and SYS returns (shared helper)`
- **RomWBW:** An error return ends with OR A, which sets S from bit 7 of the negative code, clears C, and sets Z per the value. A success return is XOR A: A=0, Z=1, S=0, C=0. Either way C comes back 0 and S matches the sign of A.
- **This dispatcher:** setResult sets AF high to the result and then only sets or clears the Z bit; S, C, P/V and H keep whatever the guest had when it executed RST 08.
- **A guest would see:** A caller that tests the documented "negative A means error" with JP M, or that tests carry, reads a stale flag instead of one derived from the result. Every shipped caller I looked at tests Z (LDDS.COM does `JR NZ` right after RST 08), which is why this is low - but the flags are wrong on every RTC return, not just an odd one. I am not certain no app uses JP M; I did not scan for it.
- **Upstream:** `hbios.asm:229-232 (#DEFINE SYSCHKERR: CALL SYSCHKA / LD A,HB_ERR / OR A) and the success convention XOR A used by every driver, e.g. dsrtc.asm:392, 408, 418`
- **Here:** `hbios_dispatch.cc:685-695 (setResult)`
- **Smallest fix:** In setResult, derive the whole flag byte from the result the way OR A would: Z from result==0, S from bit 7, P/V from parity, and clear H, N and C.
- **Status:** FIXED

### BF_SNDBEEP is 100 ms and non-blocking; RomWBW's is one third of a second at ~987 Hz and blocks  — FIXED

- **Function:** `BF_SNDBEEP (0x58)`
- **RomWBW:** Blocks the caller for roughly 333 ms while a ~987 Hz tone sounds on channel 0, then silences the chip and returns A=0.
- **This dispatcher:** Calls the front end's beep with a 100 ms duration and returns immediately; the pitch is whatever the port's beep is, and the call does not block.
- **A guest would see:** A program that beeps twice in succession to signal an error hears one run-together beep instead of two separated ones, and a program that leans on the beep as a crude ~1/3 second delay runs through it. Small, but it is a documented duration and pitch.
- **Upstream:** `hbios.asm:5173-5201 (SND_BEEP: reset, volume $FF, note 244 = B5, SNDPLAY on channel 0, then LD DE,23436 / CALL VDELAY "PLAY FOR 1/3 SECOND", then a second reset); sn76489.asm SN7_BEEP and ay38910.asm AY_BEEP both defer to it; SystemGuide.md:2483-2492 ("about 1/3 second in duration and the tone will be approximately B5")`
- **Here:** `hbios_dispatch.cc:2614-2617 (emu_dsky_beep(100); and return)`
- **Smallest fix:** emu_snd_emit_tone(0, 987, 255, 333) through the same path SNDPLAY uses, so a port with a tone renderer gets the real pitch, and fall back to emu_dsky_beep(333) otherwise.
- **Status:** FIXED

### HBIOS returns set only the Z flag; RomWBW always returns with carry cleared and sign set from A  — FIXED

- **Function:** `all functions — setResult()`
- **RomWBW:** Every HBIOS return passes through `OR A` or `XOR A`, so on return carry is CLEAR, N and H are clear, S reflects the sign of A (set for every negative error code) and P/V is the parity of A.
- **This dispatcher:** A and Z are correct. Carry, S, P/V, N and H are whatever the guest happened to leave set before the RST 08 / CALL $FFF0 — the OUT that triggers dispatch does not touch flags and the handler runs in C++.
- **A guest would see:** A caller that tests `JP M` or `JR C` immediately after the call instead of re-deriving the flags with its own `OR A` reads a stale condition. I am marking this low and stating the uncertainty plainly: I grepped Source/HBIOS/*.asm for a carry or sign test in the two instructions following an RST 08 and found none, so I cannot name a caller that breaks. The divergence from the asm is nonetheless real and cheap to remove.
- **Upstream:** `hbios.asm:229-232 (the SYSCHKERR macro ends `LD A,HB_ERR / OR A`) and every success return, e.g. hbios.asm:5674 (EXT_SLICE6Z `XOR A`) and 5201 (`XOR A ; SIGNAL SUCCESS`)`
- **Here:** `hbios_dispatch.cc:686-696 — setResult writes A and then only sets or clears qkz80_cpu_flags::Z`
- **Smallest fix:** In setResult, additionally clear carry, N and H and set S and P/V from the value written to A — i.e. emulate `OR A` on the result byte rather than only Z.
- **Status:** FIXED

### closeDisk and the MBR re-probe never reset partition_sectors, so a swapped image keeps the previous one's partition bound  — FIXED

- **Function:** `BF_EXTSLICE (0xE0)`
- **RomWBW:** RomWBW zeroes its whole working block at the top of every EXT_SLICE call, so a partition size from a previous medium cannot survive into the next one's bounds check.
- **This dispatcher:** partition_sectors persists across closeDisk() and across the re-probe. Since the bound is `limit = min(disk_sectors, base + partition_sectors)`, a stale value can only make the limit smaller.
- **A guest would see:** Mount an hd1k image with a small 0x2E partition on a unit, unmount it, mount a larger hd512 image on the same unit index, and slices that fit the new medium are refused with ERR_RANGE and MID_NONE. Requires a runtime image swap on one unit, which is why this is low.
- **Upstream:** `hbios.asm:5449-5456 (EXT_SLICE1B clears the whole SLICE_WRKSTA block, SLICE_LBASIZ included, on every call before any partition scan)`
- **Here:** `hbios_dispatch.cc:248-254 (closeDisk resets partition_probed, partition_base_lba, slice_size and is_hd1k but not partition_sectors) and hbios_dispatch.cc:2856-2859 (the re-probe sets partition_base_lba, slice_size and is_hd1k, again not partition_sectors); the stale value is then used at hbios_dispatch.cc:2948-2951`
- **Smallest fix:** Add `disks[unit].partition_sectors = 0;` to closeDisk beside the other resets, and to the `if (!disk.partition_probed)` block that already clears partition_base_lba.
- **Status:** FIXED

### CIO ignores the unit number entirely; no ERR_NOUNIT is ever returned, and the unit check ordering is inverted  — FIXED

- **Function:** `CIO_DISPATCH / HB_DISPCALC — dispatcher-level unit validation`
- **RomWBW:** C is validated against CIO_CNT before anything else. With one character unit configured, C=1..0x7F returns A=ERR_NOUNIT (-4); C=0x80 is remapped to the active console (CIO_SPECIAL, hbios.asm:4548-4553). An invalid unit combined with an invalid function reports ERR_NOUNIT, not ERR_NOFUNC.
- **This dispatcher:** Every unit value is accepted. C=5 gets unit 0's console with A=0; C=0x80 works only because the unit is ignored. An out-of-range function on an out-of-range unit reports HBR_NOFUNC where RomWBW reports ERR_NOUNIT.
- **A guest would see:** The emulator reports one character unit via SYSGET_CIOCNT, so an enumerator only ever asks about unit 0 and sees no difference — that is why this is low. A utility given an explicit unit (a terminal program told to use COM1:) writes to the console and is told it worked, where real RomWBW would have said ERR_NOUNIT and the utility would have reported the missing device.
- **Upstream:** `hbios.asm:7448-7458 (HB_DISPCALC checks C against the unit count FIRST and jumps to HB_UNITERR, then checks the low nibble of B against the group's function count and jumps to HB_FUNCERR) with HB_UNITERR at hbios.asm:7495-7497 (ERR_NOUNIT) and HB_FUNCERR at 7491-7493 (ERR_NOFUNC)`
- **Here:** `hbios_dispatch.cc:821-828 — handleCIO reads C into `unit` and then never uses it except in log messages; every function acts on the single host console`
- **Smallest fix:** Reject C >= 1 (after mapping C=0x80 to 0) with HBR_NOUNIT at the top of handleCIO, before the function switch, so the ordering matches HB_DISPCALC.
- **Status:** FIXED

### The ROM signature pointer sits at 0x0005 instead of the fixed 0x0004, because a DI was added ahead of the reset jump

- **Function:** `ROM_SIG pointer in page zero of the HBIOS bank (not a BF_* function; a fixed published address)`
- **RomWBW:** Word at 0x0004 = address of ROM_SIG (0x0070). Byte 0x0003 is the filler 0.
- **This dispatcher:** The extra `di` pushes everything one byte: byte 0x0004 = 0x00 (the filler) and the pointer word lives at 0x0005. Reading the word at 0x0004 yields 0x7000, which is not the signature block. The signature block itself at 0x0070 is correct.
- **A guest would see:** Any tool that follows the RomWBW/UNA ROM-directory convention - read the word at 0x0004, check for 0x76 0xB5 there, then print the ROM name/author/description strings - reads 0x7000, finds no signature, and reports the ROM as unidentifiable. I grepped all of Source/HBIOS and found no in-tree consumer (hbios.asm only writes it), so I cannot name a shipped .COM; the address is nonetheless fixed by hbios.asm's own comment, which is why I am reporting it as low rather than dropping it.
- **Upstream:** `hbios.asm:428-431 — `JP HB_START` at 0x0000, then `.DB 0 ; SIG PTR STARTS AT $0004`, then `.DW ROM_SIG` at 0x0004-0x0005; hbios.asm:456-462 (ROM_SIG itself: 0x76 0xB5, structure version, size, three string pointers)`
- **Here:** `src/emu_hbios.asm:70-74 (`RST00: di / jp HB_START / db 0 / dw ROM_SIG`) — not hbios_dispatch.cc; verified against the shipped binary: web/catalog/v0/3.6.0/emu_avw-v0-3.6.0.rom bytes 0x0000-0x0006 are `F3 C3 00 02 00 70 00``
- **Smallest fix:** Move the `di` after the jump target, i.e. make 0x0000 `jp HB_START` and put the `di` at HB_START, so `db 0` lands at 0x0003 and `dw ROM_SIG` at 0x0004. Note this changes every published ROM's sha256 and therefore each release's catalog generation - see the warning at the head of src/emu_hbios.asm.
- **Status:** CONFIRMED and OPEN - the only one. Filed as `DECISIONS.md` #8, because both arms are minutes of work and the choice between them is about the release channel, not the code. Note also a smaller fix than the one above: `di` + `jp` already occupy four bytes, so DELETING the `db 0` filler puts `dw ROM_SIG` at 0x0004 with the `di` left where it is, at the reset instruction.

### Function codes in the 0x60-0xDF gap return ERR_UNDEF (0xFF) where RomWBW returns ERR_NOFUNC (0xFD)  — FIXED

- **Function:** `any B in 0x60-0xDF (the reserved gap between BF_SND and BF_EXT)`
- **RomWBW:** A = 0xFD (ERR_NOFUNC, -3) with NZ.
- **This dispatcher:** A = 0xFF (ERR_UNDEF, -1) with NZ.
- **A guest would see:** Both are negative and both fail the standard A<0 / NZ test, so any conventional caller behaves identically. It only differs for a caller that compares the exact code - e.g. one distinguishing "no such function" from "unspecified failure" to decide whether to retry. I have not found such a caller; reporting it because the correct constant is one character away and the two codes are distinct in hbios.inc.
- **Upstream:** `hbios.asm:4517-4526 (HB_DISPATCH: `CP BF_EXT / JR C,HB_DISPERR`), hbios.asm:4528-4530 (`HB_DISPERR: SYSCHKERR(ERR_NOFUNC) / RET`)`
- **Here:** `hbios_dispatch.cc:604-607 (getTrapTypeFromFunc returns -1 for 0x60-0xDF) and hbios_dispatch.cc:624-629 (`setResult(HBR_FAILED)`, where HBR_FAILED is 0xFF per hbios_dispatch.h:41)`
- **Smallest fix:** `setResult(HBR_NOFUNC);` at hbios_dispatch.cc:627.
- **Status:** FIXED

### CIO, VDA and SND never range-check the unit number in C, so calls to units that do not exist succeed  — FIXED

- **Function:** `all of BF_CIO 0x00-0x06, BF_VDA 0x40-0x4F, BF_SND 0x50-0x58`
- **RomWBW:** Every CIO/VDA/SND call with C >= the registered unit count for that group returns ERR_NOUNIT (0xFC) before any driver runs. DIO gets the same check, and the emulator does implement it there (HBR_NOUNIT at hbios_dispatch.cc:1120, 1500).
- **This dispatcher:** handleVDA and handleCIO ignore C entirely: unit 0, unit 7 and unit 0xFF are all serviced as the one console/display. The emulator simultaneously reports CIOCNT=1 (hbios_dispatch.cc:1862), VDACNT=1 and SNDCNT=1, so it claims those units do not exist while answering for them.
- **A guest would see:** A program that discovers devices by calling upward until ERR_NOUNIT - rather than by asking SYSGET_CIOCNT first - never terminates, or concludes there are 256 character devices. Shipped RomWBW code takes the count first (romldr.asm:210-214, invntdev), so I could not name a program that breaks; the divergence is between what the emulator's own unit counts say and what its dispatchers accept.
- **Upstream:** `hbios.asm:7444-7452 (HB_DISPCALC: `LD A,C / CP (IY-1) ; COMPARE TO COUNT / JR NC,HB_UNITERR`), hbios.asm:7499-7501 (`HB_UNITERR: SYSCHKERR(ERR_NOUNIT)`); reached from CIO_DISPATCH (hbios.asm:4540-4551), VDA_DISPATCH (5086-5093) and SND_DISPATCH (5129-5136)`
- **Here:** `hbios_dispatch.cc:2402-2407 (handleVDA does not read C at all); hbios_dispatch.cc:824-825 (handleCIO reads `unit` but uses it only in the error log at 966-967); hbios_dispatch.cc:2639 (handleSND reads C as a channel, see the SND finding above)`
- **Smallest fix:** At the top of handleCIO, handleVDA and handleSND, reject C >= the count that SYSGET reports for that group (1, 1 and 1) with HBR_NOUNIT, honouring the bit-7 console substitution for CIO the way CIO_SPECIAL does (hbios.asm:4554-4558).
- **Status:** FIXED


## Fixed while the audit was running (16)

Raised by a reader, and by the time the refutation pass reached them the
fix was already in the tree - so each verdict reads "the emulator already
does the right thing". They are real findings that were acted on, not
findings that failed. `CHANGELOG.md` carries the reasoning for each.

- **BF_DIOREAD counts a short host read as a full sector and hands the guest uninitialised stack bytes** (`BF_DIOREAD (0x13)`, high)
- **Unhandled SYSGET subfunctions return success with E=0 instead of ERR_NOFUNC; the four xxxFN lookups are among them** (`BF_SYSGET ($F8) subfunctions CIOFN $01, DIOFN $11, VDAFN $41, SNDFN $51, and every other unlisted value`, high)
- **SYSGET subfunction $12, the legacy slice call OS boot loaders still make, is not routed to EXT_SLICE** (`BF_SYSGET ($F8) subfunction $12 (was BF_SYSGET_DIOMED)`, high)
- **BF_RTCSETTIM discards the time and reports success** (`BF_RTCSETTIM (0x21)`, high)
- **BF_SNDNOTE: note is a 16-bit HL value on a scale whose zero is A#0; the emulator reads only L and lands 10 semitones flat** (`BF_SNDNOTE (0x53)`, medium)
- **BF_SNDRESET and a zero-volume BF_SNDPLAY never silence a sounding channel** (`BF_SNDRESET (0x50), BF_SNDPLAY (0x54)`, medium)
- **BF_SNDQUERY subfunctions 0x02 (volume) and 0x03 (period) answer ERR_NOFUNC; every RomWBW driver answers them** (`BF_SNDQUERY (0x55), SNDQ_VOLUME=0x02, SNDQ_PERIOD=0x03`, medium)
- **SYSGET subfunction $12, the legacy EXT_SLICE alias upstream keeps on purpose, is unimplemented AND answers success** (`BF_SYSGET ($F8) subfunction $12 / BF_EXTSLICE`, high)
- **An unknown SYSSET subfunction is reported as success instead of ERR_NOFUNC** (`BF_SYSSET ($F9)`, medium)
- **The emulator proxy's alternate invoke entry at 0xFE04 routes only CIO/DIO/RTC/SYS and rejects VDA, SND, DSKY and the whole EXT block** (`HB_INVOKE (the emu ROM-bank router reached from proxy offset +4)`, low)
- **SYSINT (0xFC) answers SUCCESS for every subfunction and sets no registers, so an installed interrupt handler is silently discarded** (`BF_SYSINT / HBF_SYSINT (0xFC), subfunctions INTINFO 0x00, INTGET 0x10, INTSET 0x20`, high)
- **SYSGET's default arm returns SUCCESS with E=0, so every unimplemented subfunction is answered as a valid result** (`BF_SYSGET / HBF_SYSGET (0xF8), subfunctions with no case: 0x01 CIOFN, 0x11 DIOFN, 0x12 (legacy slice), 0x41 VDAFN, 0x51 SNDFN, and all undefined values`, high)
- **SYSGET subfunction 0x12, the compatibility alias RomWBW explicitly keeps for OS boot loaders, is not routed to EXT_SLICE** (`BF_SYSGET (0xF8) subfunction 0x12 -> EXT_SLICE (the pre-3.3 spelling of BF_EXTSLICE 0xE0)`, high)
- **SYSGET CIOFN/DIOFN/VDAFN/SNDFN report success without returning the function or data address the caller is about to call through** (`BF_SYSGET_CIOFN 0x01, BF_SYSGET_DIOFN 0x11, BF_SYSGET_VDAFN 0x41, BF_SYSGET_SNDFN 0x51`, high)
- **SND functions take the channel from C, which is RomWBW's UNIT register; the channel register D is never read** (`BF_SNDVOL 0x51, BF_SNDPRD 0x52, BF_SNDNOTE 0x53, BF_SNDPLAY 0x54`, medium)
- **SYSSET's default arm returns SUCCESS, so SETCPUSPD and SETPANEL silently do nothing and report that they worked** (`BF_SYSSET / HBF_SYSSET (0xF9), subfunctions with no case: 0xF3 SETCPUSPD, 0xF4 SETPANEL, and all undefined values`, medium)

## Raised and refuted (14)

Kept deliberately. A finding that was looked at and did not hold is worth
as much as one that did - it is the reason nobody needs to look again. The
reason each fell is given; several fell on the emulator half while their
reading of RomWBW was exactly right, which is worth knowing before
re-raising one of them.

- **BF_CIOINIT accepts any configuration and returns success while doing nothing, including the DE=$FFFF reset that is defined to flush buffers** (`BF_CIOINIT (0x04)`) — REFUTED — the cited lines are quoted accurately, but the conclusion does not follow: "accept any DE and return success" is what RomWBW's own non-serial console driver does, and the residual flush diff.
- **SYSSET PANEL answers success on a machine with no front panel; RomWBW returns ERR_NOHW** (`BF_SYSSET ($F9) subfunction PANEL $F4`) — REFUTED.
- **BF_SYSINT answers success for every subfunction and installs nothing; no periodic tick interrupt is ever delivered** (`BF_SYSINT (0xFC), subfunctions INFO/GET/SET`) — The emulator no longer does what the finding claims.
- **BF_VDASCO: D is the Scope, not a foreground colour, and E is the whole colour byte** (`BF_VDASCO (0x47)`) — REFUTED on guest-visibility and on applicability, though the finding's reading of the HBIOS contract is correct.
- **BF_VDASCR: E is signed; a negative E means reverse scroll, and the emulator scrolls up 255 lines instead** (`BF_VDASCR (0x4B)`) — The upstream half checks out: vdu.
- **BF_SNDQUERY subfunction 0x04 returns B=0, which is SNDDEV_SN76489 - the exact claim the code's own comment says it is avoiding** (`BF_SNDQUERY (0x55), SNDQ_DEV=0x04`) — REFUTED on the emulator side.
- **BF_VDAFIL scrolls the display and advances the cursor; no RomWBW driver does either** (`BF_VDAFIL (0x49)`) — Refuted on three independent grounds, after reading both sources.
- **BF_VDAWRC scrolls at the bottom of the screen and turns characters 0x0D and 0x0A into cursor motion** (`BF_VDAWRC (0x48)`) — Upstream half verified and accurate: vdu.
- **BF_SNDPRD accepts any 16-bit period and reports success, and treats a chip divisor as microseconds** (`BF_SNDPRD (0x52)`) — Refuted on two independent grounds, both verified by reading both sources.
- **BF_VDAINI does not flush the keyboard buffer** (`BF_VDAINI (0x40)`) — REFUTED — no VDA driver in RomWBW v3.
- **SYSINT (0xFC) answers every subfunction with success and sets no return registers** (`BF_SYSINT (0xFC), subfunctions BF_SYSINT_INFO=$00 / _GET=$10 / _SET=$20`) — Refuted on the emulator side; the asm side of the finding is correct but no longer describes a divergence.
- **HBF_HOST_GETARG answers SUCCESS for an argument index that does not exist** (`HBF_HOST_GETARG (0xE7) — private block, checked against hbios_dispatch.h`) — REFUTED on guest-visibility, though the code mechanism is real.
- **CB_HEAP and CB_HEAPTOP in the HCB are never written, so the heap the guest can read is permanently empty while SYSALLOC hands out memory** (`BF_SYSALLOC 0xF6 and BF_SYSRESET 0xF0/RES_INT, via HCB fields CB_HEAP (BIOS bank 0x0120) and CB_HEAPTOP (0x0122)`) — REFUTED — the asm half checks out, but the emulator half is miscited and the stated guest-visible consequence is factually wrong.
- **CB_CONDEV and CB_CRTDEV stay at 0xFF, a value hbios.asm documents as existing only before init completes** (`HCB fields CB_CONDEV (BIOS bank 0x0112) and CB_CRTDEV (0x0111)`) — REFUTED on its central claim, and the second half is not guest-visible.

## What was read

### CHARACTER I/O (BF_CIOIN, BF_CIOOUT, BF_CIOIST, BF_CIOOST, BF_CIOINIT, BF_CIOQUERY, BF_CIODEVICE)

READ IN FULL. Emulator: src/hbios_dispatch.cc:821-980 (handleCIO, all seven cases plus the default), :596-630 (getTrapTypeFromFunc / handleMainEntry, confirming func 0x00-0x0F routes to handleCIO), :686-696 (setResult, incl. the Z-flag convention), :734-752 (doRet), :110-125 (getOutputChars), :1858-1862 (SYSGET_CIOCNT = 1), hbios_dispatch.h:28-57 (HBR_*/HBF_* constants — all seven CIO function numbers verified against hbios.inc:5-12), emu_io.h:79-115 (console API), hbios_cpu.cc port handlers (confirmed no CIO fast path — the console reads there are Altair-style direct port emulation), romwbw_emu.cc:775-782 and :1630-1657 (output is flushed every instruction, so the CIOOUT buffering is not guest-visible).

RomWBW v3.6.0: hbios.asm:4506-4560 (HB_DISPATCH1, CIO_DISPATCH, CIO_SPECIAL), :4563-4605 (CIO table, CIO_FNCNT=7, CIO_MAX=32, CIO_CNT), :7435-7490 (HB_DISPCALL/HB_DISPCALC unit-then-function range checks), :7191-7198 (CIO_IDLE — confirms A and the Z flag survive the idle path, so the emulator's A=0/Z-set on 'no input' matches), :3140-3205 (HBIOS's own CIOQUERY/CIOINIT use at boot), :8870-8930 (HB_PRTSUM). Drivers: uart.asm CIO function table and all seven handlers (IN/OUT/IST/OST/INITDEV/QUERY/DEVICE), tty.asm:70-146, and the QUERY and DEVICE handlers of acia, asci, sio, duart, sser, esp (both), lpt, pio. invntdev.asm:191-260 (how C, D, E and the CIOQUERY word are decoded and printed). romldr.asm:960-1010 (the CONSOLE command's CIOQUERY/CIOINIT sequence). SystemGuide.md:820-1063 (invocation and register-preservation rules, result codes, the whole CIO chapter including the line-characteristics word layout).

NOT READ / LIMITS. (1) MODE.COM's source is not in the download (Source/Apps does not exist in this tree), so every claim about what MODE.COM prints comes from the in-tree comment at hbios_dispatch.cc:947-951, not from my own reading — flagged inside findings 1 and 3. (2) I did not read the interrupt-driven UART variants beyond UART_INTIST/UART_INTIN, since the emulator models no interrupt-driven receive buffer. (3) I did not audit the VDA twins BF_VDAKST/BF_VDAKRD at hbios_dispatch.cc:2437-2508, which are another group's. (4) I did not check the CP/M BIOS's character routing (cbios is not in the download), which is where a guest-visible consequence for finding 4's buffer flush would most likely live.

CHECKED AND CLEAN, so that the next pass does not redo it: BF_CIOIST no longer returns $FF — it returns a count of 0 or 1, non-negative, with the Z flag matching UART_IST1/CIO_IDLE, so the HTALK.COM class of bug is gone from that function. BF_CIOOST returns A=1 (space for one character), which is what UART_OST and TTY_OST both return, also non-negative. No other CIO case returns a value with bit 7 set except as a genuine error code (HBR_NOTIMPL=$FE, HBR_NOFUNC=$FD), and the default case returns HBR_NOFUNC, which matches the ERR_NOFUNC that HB_DISPCALC gives for function indices >= CIO_FNCNT. BF_CIOIN returns the character in E with A=0, and BF_CIOOUT takes it from E, both correct. The extra E writes in CIOIST and CIOOST are harmless: SystemGuide.md:822-826 states HBIOS does not preserve unused registers.

### DISK I/O (BF_DIOSTATUS … BF_DIOGEOM), plus the slice arithmetic in BF_EXTSLICE

READ, RomWBW v3.6.0 side: hbios.inc (BF_DIO* 14-26, ERR_* 218-232, MID_*, DIODEV_*, SPS_HD512/SPS_HD1K 379-380); Doc/SystemGuide.md lines 1075-1419 (the whole DIO chapter, all twelve functions) and 2502-2530 (EXTSLICE); hbios.asm DIO_DISPATCH/DIO_DISPCALL (4613-4665), HB_DSKREAD/HB_DSKWRITE/HB_DSKIO/HB_DSKIOX/HB_DSKFN (4683-4925), HB_CHS2LBA (7223-7235), HB_DISPCALL/HB_DISPCALC/HB_UNITERR/HB_FUNCERR (7435-7500), EXT_SLICE end to end (5370-5710); md.asm in full for the driver entry points (attributes 24-26, cfg table 48-70, fn table 160-171, MD_STATUS/RESET/CAP/GEOM/DEVICE/MEDIA/SEEK/READ/WRITE/RW 178-346); hdsk.asm in full (1-330); ide.asm IDE_GEOM (791-799) and sd.asm SD_GEOM (1178-1185) as cross-checks on the geometry convention; invntdev.asm PS_DISK/PS_PRTDC (80-165); invntslc.asm prtslc2/prtslc3 (70-165); romldr.asm fp_hdboot/fp_flopboot (700-790) and diskboot (1360-1400); the driver #INCLUDE order in hbios.asm (9320-9355) to confirm MD takes units 0-1.

READ, emulator side: hbios_dispatch.cc lines 980-1531 (map_md_unit, map_hd_unit, is_md_unit, the whole of handleDIO), 2673-2858 (handleEXT's HBF_EXTSLICE), 175-250 (loadDisk/loadDiskFromFile/closeDisk, to check that HBDisk::size is always populated); hbios_dispatch.h 28-72 (HBR_*, HBF_DIO*), 298-308 (MID_*), 325-390 (MemDiskState, HBDisk, total_sectors); emu_io.h 210-240 and emu_io_common.cc 225-266 (emu_disk_read/write semantics); romwbw_mem.h banking model (COMMON_BANK 0x8F) to check the read/write bank lambdas.

NOT READ, inside my group: the remaining DIO drivers beyond their GEOM/CAP entry points (fd.asm, rf.asm, ppide.asm, prp.asm, ppp.asm, ch*.asm, espsd.asm, scsi.asm, ppa/imm/syq) - I checked the geometry convention against md/hdsk/ide/sd and assumed the rest follow, which the identical comment text in all four suggests but I did not verify; the floppy CHS paths in fd.asm, which the emulator has no equivalent for; HBX_BNKCPY's internals (I took the bounce-buffer behaviour from HB_DSKREAD/HB_DSKWRITE themselves); the emulator's DISKUT/BF_SYSGET_DIOCNT unit-inventory code at hbios_dispatch.cc:435-475 and 1865-1880 beyond confirming the unit numbering, since that is the SYS group. I did not exercise the emulator - every finding is read from the two sources.

### SYS group (BF_SYSRESET $F0 … BF_SYSINT $FC and all SYSGET/SYSSET/SYSINT/SYSRESET subfunctions)

READ IN FULL, both sides:

RomWBW v3.6.0 asm (scratchpad .../romwbw-src/Source/HBIOS/):
- hbios.inc:93-181 - every BF_SYS* / BF_SYSGET_* / BF_SYSSET_* / BF_SYSINT_* / BF_SYSRES_* equate, and ERR_* at 218-232.
- hbios.asm:4477-4530 - HB_DISPATCH, to confirm the $F0-$FF range reaches SYS_DISPATCH.
- hbios.asm:5229-5258 - SYS_DISPATCH itself (AND $0F, then twelve DEC A/JP Z arms, $FD-$FF -> HB_DISPERR). The emulator's getTrapTypeFromFunc (hbios_dispatch.cc:595-605) plus its switch on the whole B byte is equivalent here; no finding.
- hbios.asm:5721-7062 - the whole SYS API body: SYS_RESET/RESINT/RESWARM/RESCOLD/RESUSER, SYS_VER, SYS_SETBNK, SYS_GETBNK, SYS_SETCPY, SYS_BNKCPY, SYS_ALLOC, SYS_FREE, the SYS_GET dispatch and all of CIOCNT/CIOFN/DIOCNT/DIOFN/$12/RTCCNT/DSKYCNT/VDACNT/VDAFN/SNDCNT/SNDFN/GETFN/SWITCH/TIMER/SECS/BOOTINFO/CPUINFO/MEMINFO/BNKINFO/CPUSPD/PANEL/APPBNKS, the SYS_SET dispatch and SWITCH/TIMER/SECS/BOOTINFO/CPUSPD/PANEL, SWITCH_RES + SWITCH_TAB, SYS_PEEK, SYS_POKE, SYS_INT + INTINFO/INTVECADR/INTGET/INTSET.
- hbios.asm:1030-1130 (HBX_BNKCPY, HBX_BC_ITER) and the HBX_PEEK/HBX_POKE pair, for the proxy-side register effects.
- hbios.asm:7541-7602 - HB_ALLOC.
- Config/SBC_simh_std.asm, cfg_SBC.asm, cfg_RCZ80.asm, cfg_MASTER.asm - INTMODE, TICKFREQ, CPUSPDCAP, FPSW_ENABLE, FPLED_ENABLE, PLT_SBC, for the build the published ROMs actually are.
- Doc/SystemGuide.md:2625-3340 - the documented contract for $F2 through $FC including every SYSGET/SYSSET/SYSINT subfunction table.

Emulator: hbios_dispatch.cc:1671-2277 (all of handleSYS), plus setResult/doRet/getTrapTypeFromFunc at 595-745, handleEXT's HBF_EXTSLICE arm at 2675-2720, the constants in hbios_dispatch.h:28-290 and 662-730, and hbios_cpu.cc and emu_hbios.asm to confirm nothing in the SYS range is handled outside the C++ dispatcher (emu_hbios.asm has no SYS logic; hbios_cpu.cc only does port I/O).

CHECKED AND FOUND CORRECT - not findings, listed so a later pass does not redo them: SYSVER (D/E nibble packing from the loaded ROM, L=PLT_SBC=1 per hbios.inc:188); SYSSETBNK and SYSGETBNK (previous/current bank in C, not L); SYSSETCPY (D=dest, E=source, HL=count - not swapped); SYSFREE (correctly HBR_NOTIMPL, matching SYSCHKERR(ERR_NOTIMPL)); SYSPEEK/SYSPOKE (bank in D, byte in E, HL address; the emulator's >=$8000 shortcut is equivalent because high memory is common); SYSGET TIMER and SECS (DE:HL 32-bit, C=TICKFREQ=50 which matches cfg_SBC.asm:60, C=ticks-within-second); SYSGET CIOCNT/DIOCNT/RTCCNT/DSKYCNT/VDACNT/SNDCNT (all return the count in E); SYSGET SWITCH keys $FF/1/3 (the $FF path correctly returns early to preserve A and set/clear Z); SYSGET CPUINFO (H/L/DE/BC all four set); SYSGET MEMINFO and BNKINFO (D/E only - the "H: FIRST APP BANK ID / L: APP BANK COUNT" lines in the RomWBW comment at hbios.asm:6289-6294 are stale; the code at 6296-6302 sets only D and E, so the emulator matches and there is no finding here); SYSGET APPBNKS (H/L/E); SYSGET PANEL (HL=0 with HBR_NOHW, correct for FPSW_ENABLE FALSE); SYSSET TIMER/SECS; SYSSET CPUSPD - the emulator's do-nothing-and-succeed happens to be exactly right, because on a SPD_FIXED Z80 build every conditional arm of SYS_SETCPUSPD is compiled out and control falls straight into SYS_SETCPUSPD_OK (hbios.asm:6842-6849) which returns A=0 after doing nothing; SYSRESET $00 heap reset (heap_ptr = heap_curb matches "LD HL,(HEAPCURB) / LD (CB_HEAPTOP),HL").

NOT READ / OUT OF SCOPE: the CIO, DIO, RTC, DSKY, VDA, SND and EXT handlers in hbios_dispatch.cc, except handleEXT's EXTSLICE arm which the $12 finding depends on. One thing I noticed at the top level and am NOT reporting as a SYS finding because it belongs to the dispatcher as a whole: getTrapTypeFromFunc returns -1 for the $60-$DF gap and handleMainEntry answers HBR_FAILED ($FF), where HB_DISPERR (hbios.asm:4528) answers ERR_NOFUNC (-3, $FD). Also not verified: whether any real RomWBW .COM exercises the xxxFN or SYSINT paths - the RomWBW Apps sources are not in the downloaded package, only Source/HBIOS and Source/Doc, so every guest_visible field above is argued from the SystemGuide contract and from RomWBW's own in-tree callers rather than from a named binary.

### RTC group (BF_RTCGETTIM/SETTIM/GETBYT/SETBYT/GETBLK/SETBLK/DEVICE/alarm) plus TIME: the periodic tic

READ IN FULL. Emulator: src/hbios_dispatch.cc handleRTC() (lines 1536-1663) end to end; the SYSGET arm's RTCCNT/SWITCH/TIMER/SECS cases (1860-2010); the SYSSET arm's TIMER/SECS/SWITCH cases (2133-2205); HBF_SYSINT (2233-2236); setResult/recalcNvramChecksum/settleNvramChecksum (685-732); hbios_dispatch.h TICKFREQ, tick_origin, currentTicks/setTicks (662-681) and NVRAM_SIZE/nvram_switches/nvram_checksum_provisional (736-743); emu_io.h emu_time and emu_io_common.cc:295-306 emu_get_time (localtime, so the wall clock is right); emu_init.h/emu_init.cc:23-61 for the version bytes that seed the checksum; emu_hbios.asm HB_INVOKE and RTC_DISPATCH (255-320); romwbw_emu.cc:1660-1690 to confirm the only interrupt sources are the --nmi/--mask-interrupt test flags.

READ IN FULL. RomWBW 3.6.0: hbios.asm SYS_GETSWITCH/SYS_GETSWITCH3/SYS_GETTIMER/SYS_GETSECS (6158-6245), SYS_SETSWITCH/SWITCH_RES/SWITCH_TAB (6437-6521), SYS_SETTIMER/SYS_SETSECS (6535-6572), HB_TIMINT/VEC_TICK/HB_TICK/HB_SECOND (7353-7396), the whole NVSW_* block (8079-8186), NVR_INIT (3719-3742), the HCB switch shadow and CB_VERSION (478-540), HB_DISPATCH's function routing (4503-4530), RTC_DISPATCH/RTC_DISPERR (5015-5043), SYSCHKERR (229-232), HB_TICKS/HB_SECTCK/HB_SECS storage (9527-9529). Siblings: dsrtc.asm dispatcher and every handler; the GETBLK/SETBLK/GETALM/SETALM arm of all twelve RTC drivers; ds1501rtc.asm:326-375 (the only real block implementation, 256 bytes); hbios.inc BF_RTC*/BF_SYSGET_*/BF_SYSSET_*/BF_SYSINT_*/ERR_* equates; SystemGuide.md 1427-1570 (the whole RTC chapter) and 3290-3330 (SYSINT); API.txt RTCCNT/TIMER/SECONDS. TICKFREQ confirmed 50 in every cfg_*.asm in the tree.

EVIDENCE GATHERED. Extracted the .COM files from all six slices and all sixteen user areas of romwbw_disks/build/v0-romwbw-3.6.0/hd1k_combo-v0-3.6.0.img with cpmemu/util/cpm_disk.py (730 files) and scanned for aligned HBIOS call sequences, which is where the LDDS/LDNZT/LDP2D BF_RTCSETTIM citation and the TIMER.COM SYSSET_SECS-0 citation come from. RTC.COM is on the images but bit-bangs the DS1302 latch port directly and never enters HBIOS, so it is not a witness for anything here.

NOT COVERED. I did not verify RomWBW's ez80 variants of GETTIMER/SETTIMER (ez80drv.asm is #IF'd in for CPU_EZ80 and is not a configuration these ROMs are built for). I did not check the interaction between the NVRAM persisted-setting path in romwbw_emu.cc (setNvramSetting, --boot) and the RTC handlers beyond reading the comments at hbios_dispatch.cc:697-732 - the provisional-checksum reseeding logic looked correct against NVSW_CHECKSUM and I found nothing to report there. I did not scan the 3.5.1 images, only 3.6.0. Groups outside mine - CIO, DIO, VDA, SND, the rest of SYSGET/SYSSET, bank management - I did not read except where a shared helper (setResult, doRet) was on an RTC return path.

### VIDEO (BF_VDAINI/QRY/RES/DEV/SCS/SCP/SAT/SCO/WRC/FIL/CPY/SCR/KST/KFL/KRD/RDC, 0x40-0x4F) and SOUND (

READ IN FULL, RomWBW 3.6.0 side: Source/HBIOS/vdu.asm (all 768 lines - the VDA driver whose device type the emulator reports, VDADEV_VDU); Source/HBIOS/ppk.asm keyboard entry points (PPK_STAT/PPK_READ/PPK_FLUSH, lines 105-165) and the kbd.asm equivalents; Source/HBIOS/audio.inc (all 102 lines, AUD_NOTE and the eighth-tone scale); Source/HBIOS/sn76489.asm (lines 1-400 and 426-556: every SND function, the query subfunctions, the device record and the note table); Source/HBIOS/ay38910.asm (AY_VOLUME through AY_BEEP, lines 145-510); Source/HBIOS/hbios.asm VDA_DISPATCH/SND_DISPATCH and their tables (5080-5165), SND_BEEP (5165-5210), HB_DISPCALL/HB_DISPCALC (7435-7490), and the 0x40/0x50 range check at 4516-4519; Source/HBIOS/hbios.inc BF_VDA*/BF_SND*/BF_SNDQ_*/VDADEV_*/SNDDEV_* (52-86, 458-473); Source/HBIOS/invntdev.asm PS_VIDEO and PS_SOUND (330-440, how VDADEV/VDAQRY/SNDQUERY answers are consumed); Source/Doc/SystemGuide.md the whole VDA section (1800-2215) and the whole SND section (2220-2495); spot-read VDASCO/VDASAT/FILL in vga.asm, cvdu.asm and tvga.asm to confirm the conventions are driver-wide and not a vdu.asm quirk; ansi.asm and tty.asm at their VDASCR/VDAFIL/VDAWRC call sites to see how RomWBW itself passes the arguments.\n\nREAD IN FULL, emulator side: hbios_dispatch.cc handleVDA (2299-2530) and handleSND (2532-2660) line by line, plus getTrapTypeFromFunc (2596-2608 region), setResult (686-695), the SYSGET_VDACNT/SNDCNT answers (1885-1891) and the initial VDA/SND state (83-93); hbios_dispatch.h HBF_VDA*/HBF_SND*/SNDQ_*/HBiosResult (27-129, 255-265); emu_io.h the video block (265-290) and the whole sound block (296-331).\n\nNOT READ in my group: gdc.asm, tms.asm (beyond TMS_VDASCO), vrc.asm, ef.asm and xosera.asm - the five remaining VDA drivers. I read enough of them via vga/cvdu/tvga to confirm the register conventions are shared, but if one of them diverges on a function I called settled I would not have seen it. ym2612.asm and spk.asm, the other two SND drivers, likewise unread; the SNDQ subfunction set and the D-is-the-channel convention are confirmed from the two PSG drivers plus the manual. hbios_cpu.cc I only skimmed for the OUT-trap/RET mechanics behind the VDAKRD rewind and did not audit.\n\nONE THING I DELIBERATELY DID NOT FILE: BF_VDADEV returns E := the unit number from C (hbios_dispatch.cc:2331) where vdu.asm:206 returns E := 0, \"PHYSICAL UNIT IS ALWAYS ZERO\". With one video unit the two are identical, so it is unobservable today and only becomes visible if the unit-validation finding is fixed by accepting more units. Worth folding into that fix rather than tracking on its own.\n\nTWO THINGS I CHECKED AND FOUND CORRECT, since the brief says several of these once returned success with the caller's registers untouched: (1) both group defaults now set result = HBR_NOFUNC (hbios_dispatch.cc:2520 and 2656) rather than falling through at HBR_SUCCESS, and (2) BF_SNDQUERY subfunction 0x00 (SNDQ_STATUS) correctly returns an error - sn76489.asm:230-243 and ay38910.asm:446-460 both fall through to OR $FF for it, so the emulator's default branch matches. BF_VDAQRY's D=rows/E=cols order, its C:=0 mode and HL:=0 font-bitmap answer, and BF_VDARES being a no-op are all correct against vdu.asm:194-203 and SystemGuide.md:1911-1953.

### EXT group, private HBF_HOST_* block (0xE1-0xEA), and the dispatcher itself (routing, HBX_BNKCALL/BNK

READ IN FULL: hbios.asm HB_DISPATCH and the whole group-dispatch ladder (4477-4530), CIO_DISPATCH/CIO_SPECIAL (4533-4560), EXT_DISPATCH (5211-5221), SYS_DISPATCH (5224-5260), EXT_SLICE end to end (5376-5678), SYS_GET's subfunction ladder (5977-6025), SYS_SET's (6432-6447), SYS_INT's (6954-7005), SYS_RESET's (5721-5732), HB_DISPCALL/HB_DISPCALC/HB_FUNCERR/HB_UNITERR (7435-7497), HBX_INVOKE (600-663), HBX_BNKSEL (665-700), HBX_BNKCPY + HBX_BC_ITER (1040-1130), HBX_BNKCALL (1134-1170), the proxy management block and entry vectors (1440-1465), the APPBOOT page-zero fixup (2330-2340), and hbios.inc's error codes (221-226), BF_* numbering (89-152), MID_*/SPS_* (365-380) and HB_INVOKE/HB_BNKCALL addresses (573-578). SystemGuide.md: Invocation and the register-preservation paragraph (815-840), Result Codes (865-885), DIODEVICE attribute bit table (1296-1340), EXTSLICE (2504-2540), and the proxy functions BNKSEL/BNKCPY/BNKCALL (3355-3420). Also read Source/HBIOS/invntslc.asm:100-175 as the one in-tree BF_EXTSLICE caller.\n\nON THE EMULATOR SIDE: hbios_dispatch.cc getTrapTypeFromFunc + handleMainEntry + handlePortDispatch + setResult + doRet + fetchGuestString + storeHostName (lines ~590-820), handleEXT in full including all ten HBF_HOST_* cases (2809-3240), handleCIO's head and tail (821-985), handleSYS's SYSRESET/SYSVER/SYSGET/SYSSET/SYSINT/SYSBOOT and all three default arms (1782-2400), handleDIO's DIODEVICE/DIOMEDIA/DIOCAP and default (1404-1530), handleRTC's default, handleDSKY in full (2771-2802), closeDisk (235-255). Plus hbios_dispatch.h's HBF_* enum, the HBF_HOST_* contract comments, HOST_PATH_MAX, HBR_* codes and the SYSGET/SYSSET/SYSRES subfunction enums; hbios_cpu.cc in full (ports 0xEC BNKCPY, 0xED BNKCALL, 0xEE signal, 0xEF dispatch); emu_hbios.asm in full (page zero, HB_INVOKE router, the proxy image, HBX_BNKSEL, HBX_BNKCALL and the 0xFFF0-0xFFFF vector block).\n\nCHECKED AND FOUND CORRECT, so not reported: the group boundaries in getTrapTypeFromFunc match hbios.asm's CP ladder exactly, including DSKY at $30-$3F and the $60-$DF gap; 0xEB-0xEF fall to handleEXT's default with HBR_NOFUNC, matching EXT_DISPATCH; handleSYS's outer default returns HBR_NOFUNC for $FD/$FF, matching HB_DISPERR; handleDSKY initialises result to HBR_NOHW so even its default matches DSKY_DISPERR's ERR_NOHW; the emu proxy's HBX_BNKCALL push/call/EX (SP),HL/bank-restore sequence is instruction-for-instruction RomWBW's and preserves HL and IX as RomWBW does; port 0xEC's BNKCPY leaves HL/DE advanced and BC=0 and treats BC=0 as a zero-length copy, both matching HBX_BNKCPY; SPS_HD512=$4100, SPS_HD1K=$4000, MID_HD=4, MID_HDNEW=10, MID_MDROM=1, MID_MDRAM=2 all match hbios.inc; IX, IY and the alternate register set are never touched by any dispatch path, which is what SystemGuide.md:820-822 promises; the HBF_CIOIN and HBF_HOST_READ PC-rewind of 2 bytes is correct for the 2-byte OUT at 0xFFF0.\n\nNOT READ (outside this group): handleVDA and handleSND bodies beyond their default arms, handleDIO's read/write/seek paths, handleRTC's NVRAM bodies, the SYSGET/SYSSET switch bodies other than their defaults, bootFromDevice and the ROM-app loader, emu_io.h's backend declarations beyond emu_host_path_caps, and hbios.asm's driver files (acia/tty/hdsk/etc.). NOTE: src/hbios_dispatch.cc was being edited by another process during this audit (mtime advanced from 05:02 to 05:15:41 mid-read); every hbios_dispatch.cc line number above was re-derived from the 05:15:41 revision, md5 5b276a8775972f4535d5bf61d0d315f3, and all cited constructs were re-confirmed present in it, but the numbers may have drifted again — anchor on the case/function names, which are unique.

### The rest of hbios.asm: init side effects, the HCB, published fixed addresses, the dispatch tables, a

WHAT I READ IN hbios.asm (9,802 lines). Fully: 418-560 (page zero, RST vectors, ROM_SIG, the whole HCB definition); 560-1360 (the upper-memory proxy - HBX_IDENT, HBX_INVOKE, HBX_BNKSEL, HBX_BNKCPY/HBX_BC_ITER, HBX_BNKCALL, HBX_PEEK/HBX_POKE, HBX_IVT), skimming the per-platform #IF arms for Z180/Z280/eZ80/MBC since the emulator is a plain Z80 MM_SBC target; 2224-2460 (HB_START1/HB_START2: dispatch-table clear, CB_CRTDEV reset, IVT clear, heap init at 2444-2446); 3718-3745 (NVR_INIT and the CB_SWITCHES status byte); 4091-4130 (INITSYS4 and the bank call into romldr); 4460-4700 (HB_DISPATCH, HB_DISPERR, CIO_DISPATCH/CIO_SPECIAL/CIO_ADDENT/CIO_SETCRT, the CIO and DIO unit tables and their FNCNT/MAX/CNT prefixes, DIO_DISPATCH); 5010-5290 (RTC_DISPATCH/RTC_SETDISP, DSKY_DISPATCH, VDA_DISPATCH, SND_DISPATCH, SND_BEEP, EXT_DISPATCH, SYS_DISPATCH); 5721-6060, 6101-6180, 6253-6320, 6415-6530, 6884-7080 (every SYS_* handler: RESET/RESINT/RESWARM/RESCOLD/RESUSER, VER, SETBNK, GETBNK, SETCPY, BNKCPY, ALLOC, FREE, the whole SYS_GET compare chain and its handlers, SYS_SET and its chain, SWITCH_RES/SWITCH_TAB, PEEK, POKE, SYS_INT and all three subfunction handlers); 7400-7620 (HB_DISPCALL, HB_DISPCALC with the unit and function range checks, HB_FUNCERR/HB_UNITERR, HB_ADDENT, HB_ALLOC).

ALSO READ: hbios.inc in full for the BF_*/BC_*/HBX_* equates and the fixed proxy addresses (lines 1-200 and 521-578); Source/Doc/SystemGuide.md sections on the HCB and NVRAM switches (660-720), SYSGET subfunctions (2755-2800), SYSINT (3226-3329) and Proxy Functions (3330-3430); siblings romldr.asm (page-zero setup, the HCB peeks at 151/205/1020/1051, curcon/conpoll/flush, cout/cin/cst) and sn76489.asm (the SND unit-vs-channel contract). On the emulator side I read hbios_dispatch.cc in full, hbios_cpu.cc in full, hbios_dispatch.h's constant tables, src/emu_hbios.asm in full, and verified page zero and the HCB against the shipped binary web/catalog/v0/3.6.0/emu_avw-v0-3.6.0.rom with xxd.

WHAT I DID NOT READ. hbios.asm 1360-2224 (HB_START cold boot, BOOTWAIT, Z280_INITZ/Z280_BOOTPDRTBL, BOOTFLIP, ROMRESUME, the SZ180/MBC bring-up paths); 2460-3718 (CPU type discovery HB_CPU1/2/3, HB_BOOTDLY, the console-readiness probe HB_CONRDY and its per-UART arms, HB_CHRES, HB_SPDTST, INTTEST, HB_Z280BUS, the ROM/bank checksum HB_ROMCK); 3745-4091 (watchdog activation, HB_CRTACT, the front-panel HB_FP1/2/3, INITSYS3/INITSYS3A, DBG_NOTE and the commented-out driver init tables); 4700-5010 (HB_DSKREAD/HB_DSKWRITE bounce-buffer helpers, HB_DSKIO, HB_DSKACT disk-activity LEDs) - the DIO read/write path they serve was pass 2's territory and the emulator's handling of the D=buffer-bank and HL-advance contract there is already commented as fixed; 5290-5720 (Z280_IVT and the EXT_SLICE body - I read the emulator's HBF_EXTSLICE implementation but not upstream's slice arithmetic line by line, so I make no claim about the LBA maths itself); 6320-6415 and 6530-6884 (the per-platform SYS_GETCPUSPD/SYS_SETCPUSPD arms and SYS_GETPANEL - I read the entry contracts and the SBC arm, not the Z180/Z280/eZ80 variants); 7620-9802 (Z280 support routines, the interrupt handlers and HB_BADINT, PRT*/XREGDMP diagnostics, TERM/ANSI glue, romfonts tables, and the NVSW_* helpers at 8090-8170 beyond what their call sites told me). I also did not read the ~130 device driver siblings in Source/HBIOS/ apart from sn76489.asm and romldr.asm, nor any cfg_*.asm beyond grepping AUTOCON.

CONSEQUENCE OF THE GAPS: I have not audited the boot-time hardware probing (CPU speed detection, console autodetect, ROM checksum) at all - the emulator implements none of it and I did not check whether any of it leaves guest-visible state behind besides CB_CONDEV/CB_CRTDEV, which I did check. Nor have I verified EXT_SLICE's arithmetic against the emulator's, only its reachability.

