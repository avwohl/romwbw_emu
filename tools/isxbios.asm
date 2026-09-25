; isxbios.asm - a compact CP/M 2.2 BIOS for DRI's ISX on RomWBW
;
; tools/romwbw-batch installs this, at F600H, in front of ISX when it runs
; ISX with --isx=compact.  It exists for one reason: memory.  ISX lets an ISIS
; program use everything up to the BIOS, and RomWBW's CP/M 2.2 CBIOS starts at
; E600H, which leaves Intel's PL/M-80 V3.1 too little to compile the larger
; MP/M II utilities - SET, SHOW and STAT stop with "LIMIT EXCEEDED: DYNAMIC
; STORAGE", ED and PIP crash.  With this BIOS at F600H the top of ISIS memory
; is F5FFH, 4 KB higher.  docs/ISX.md has the measurements.
;
; It is what ISX needs and nothing more: the seventeen CP/M 2.2 entries, the
; console through HBIOS CIO, and two hd1k drives through HBIOS DIO with a
; one-sector write-through deblocking buffer.  A: is HBIOS disk unit 2 and B:
; unit 3 (--disk0 and --disk1), so ISX's :F0: and :F1: are the two disks the
; batch attached, whatever the CBIOS's own drive map says.
;
; WBOOT does not warm boot: RomWBW's CBIOS is gone by then - ISIS programs
; have overwritten it - so it asks HBIOS for a reset, which runs the boot
; loader and boots CP/M afresh from the same disk; the CCP then carries on
; with $$$.SUB.  That is also what DRI's `CPM` command reaches, since it jumps
; through (0001).
;
; Memory, while ISX runs:
;
;   F600-F632  this jump table          (below F700H, so ISX leaves it here)
;   F633-F7FF  code, tables, variables
;   F800-F87F  ISX's MDS monitor vector (ISX copies it there itself)
;   F880-FCFF  buffers: two allocation vectors, the directory buffer and
;              the 512-byte host sector
;   FE00-FFFF  the HBIOS proxy - untouched
;
; The patch bytes after the jump table are set by romwbw-batch before the
; image is installed: drive count, HBIOS units and per-drive LBA offsets.
; The installer it puts in front of ISX sets BANK from HBIOS SYSGET BNKINFO.
;
; Assemble:  um80 isxbios.asm -o isxbios.rel && ul80 -o isxbios.bin -p F600 isxbios.rel
; romwbw-batch carries the bytes, and tests/batch_test.py checks that they are
; still what this file assembles to.

	.z80
	aseg
	org	0F600h

BASE:	jp	wboot		; 00 BOOT - ISX repoints this at its own BDOS
	jp	wboot		; 03 WBOOT
	jp	const		; 06 CONST
	jp	conin		; 09 CONIN
	jp	conout		; 0C CONOUT
	jp	list		; 0F LIST
	jp	list		; 12 PUNCH
	jp	reader		; 15 READER
	jp	home		; 18 HOME
	jp	seldsk		; 1B SELDSK
	jp	settrk		; 1E SETTRK
	jp	setsec		; 21 SETSEC
	jp	setdma		; 24 SETDMA
	jp	read		; 27 READ
	jp	write		; 2A WRITE
	jp	listst		; 2D LISTST
	jp	sectran		; 30 SECTRAN

; --- patched by romwbw-batch (offsets 33H-3DH from BASE) ----------------------
ndrive:	db	2		; 33 drives: 1 = A: only, 2 = A: and B:
units:	db	2,3		; 34 HBIOS disk unit of A:, of B:
lbabas:	dw	0,0		; 36 LBA of A:'s slice, 32 bits
	dw	0,0		; 3A LBA of B:'s slice
bank:	db	8Eh		; 3E bank id for DIO transfers (installer sets it)

; HBIOS: B = function, C = unit, RST 08.
CIOIN	equ	00h
CIOOUT	equ	01h
CIOIST	equ	02h
DIOSEEK	equ	12h
DIOREAD	equ	13h
DIOWRIT	equ	14h
SYSRES	equ	0F0h
CONSOLE	equ	80h		; CIO unit: the current console

; --- buffers, above ISX's monitor vector -----------------------------------------
alva	equ	0F880h		; 256: DSM 2043 needs 2044 bits
alvb	equ	0F980h		; 256
dirbuf	equ	0FA80h		; 128
hstbuf	equ	0FB00h		; 512: one hd1k sector

; --- console ------------------------------------------------------------------------
wboot:	ld	bc,SYSRES*256+01h	; warm reset: boot loader, then CP/M again
	rst	8
	halt

const:	ld	bc,CIOIST*256+CONSOLE
	rst	8
	or	a
	ret	z
	ld	a,0FFh
	ret

conin:	ld	bc,CIOIN*256+CONSOLE
	rst	8
	ld	a,e
	ret

conout:	ld	e,c
	ld	bc,CIOOUT*256+CONSOLE
	rst	8
list:	ret

listst:	ld	a,0FFh		; the list device takes anything and discards it
	ret

reader:	ld	a,1Ah		; end of file
	ret

; --- disk ---------------------------------------------------------------------------
home:	ld	bc,0
settrk:	ld	(trk),bc
	ret

setsec:	ld	a,c
	ld	(sec),a
	ret

setdma:	ld	(dma),bc
	ret

sectran:
	ld	h,b
	ld	l,c
	ret

seldsk:	ld	hl,0		; 0 = no such drive
	ld	a,(ndrive)
	ld	b,a
	ld	a,c
	cp	b
	ret	nc
	ld	(drv),a
	ld	hl,dpha
	or	a
	ret	z
	ld	hl,dphb
	ret

read:	call	fetch		; HL -> the record in hstbuf
	ret	nz
	ld	de,(dma)
	ld	bc,128
	ldir
	xor	a
	ret

; Write-through: the record goes into the host sector and the sector goes to
; the disk now, so nothing is left dirty when WBOOT resets the machine.
write:	call	fetch
	ret	nz
	ex	de,hl
	ld	hl,(dma)
	ld	bc,128
	ldir
	ld	b,DIOWRIT
	call	dio
	ret	z
	jr	bad

; Make sure hstbuf holds the host sector the current track and sector are in,
; and point HL at the record.  Z and A = 0 on success.
fetch:	ld	a,(drv)
	ld	e,a
	ld	d,0
	ld	hl,units
	add	hl,de
	ld	a,(hl)
	ld	(nunit),a
	ld	hl,lbabas
	add	hl,de
	add	hl,de
	add	hl,de
	add	hl,de
	push	hl
	ld	hl,(trk)	; 16 host sectors a track, 4 records a sector
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,hl
	ld	a,(sec)
	rrca
	rrca
	and	0Fh
	or	l
	ld	l,a
	ex	de,hl		; DE = sector within the slice
	pop	hl		; HL -> the slice's 32-bit LBA
	ld	a,(hl)
	add	a,e
	ld	(nlba),a
	inc	hl
	ld	a,(hl)
	adc	a,d
	ld	(nlba+1),a
	inc	hl
	ld	a,(hl)
	adc	a,0
	ld	(nlba+2),a
	inc	hl
	ld	a,(hl)
	adc	a,0
	ld	(nlba+3),a
	ld	hl,nunit	; the same unit and sector as the one we hold?
	ld	de,cunit
	ld	b,5
fcmp:	ld	a,(de)
	cp	(hl)
	jr	nz,fmiss
	inc	hl
	inc	de
	djnz	fcmp
	jr	fhit
fmiss:	ld	hl,nunit
	ld	de,cunit
	ld	bc,5
	ldir
	ld	b,DIOREAD
	call	dio
	jr	nz,bad
fhit:	ld	a,(sec)
	and	3
	rrca			; record * 128: bit 0 -> 80H, bit 1 -> 100H
	ld	l,a
	and	80h
	ld	e,a
	ld	a,l
	and	01h
	ld	d,a
	ld	hl,hstbuf
	add	hl,de
	xor	a
	ret

bad:	ld	a,0FFh		; forget the sector; report the error
	ld	(cunit),a
	ld	a,1
	or	a
	ret

; B = DIOREAD or DIOWRIT, for cunit/clba.  Z on success.
dio:	push	bc
	ld	a,(cunit)
	ld	c,a
	ld	hl,(clba)
	ld	de,(clba+2)
	set	7,d		; LBA addressing
	ld	b,DIOSEEK
	rst	8
	pop	bc
	or	a
	ret	nz
	ld	a,(cunit)
	ld	c,a
	ld	a,(bank)
	ld	d,a
	ld	e,1
	ld	hl,hstbuf
	rst	8
	or	a
	ret

; --- tables -----------------------------------------------------------------------------
; hd1k: 64 records a track, 4 KB blocks, 2044 of them, 1024 directory
; entries, two system tracks - cbios.asm's DPB_HDNEW.
dpb:	dw	64
	db	5,31,1
	dw	2043,1023
	db	0FFh,00h
	dw	0,2

dpha:	dw	0,0,0,0,dirbuf,dpb,0,alva
dphb:	dw	0,0,0,0,dirbuf,dpb,0,alvb

; --- variables ------------------------------------------------------------------------
drv:	db	0
trk:	dw	0
sec:	db	0
dma:	dw	80h
nunit:	db	0		; the sector wanted: unit, then 32-bit LBA
nlba:	dw	0,0
cunit:	db	0FFh		; the sector in hstbuf: 0FFh = none
clba:	dw	0,0

	end
