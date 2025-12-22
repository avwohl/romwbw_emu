	.z80
;
;==================================================================================================
; EMU_HBIOS.ASM - Minimal HBIOS for cpmemu emulator
;==================================================================================================
;
; This is a minimal replacement HBIOS that provides HBIOS services via emulator I/O ports.
; No actual hardware I/O is performed - HBIOS calls are handled by the emulator.
;
; I/O Ports:
;   0xEE - Signal port (init/status signaling)
;   0xEF - HBIOS dispatch trigger (OUT triggers emulator to handle HBIOS call)
;
; Call Flow:
;   1. Caller sets up B=function, C=unit, other regs as needed
;   2. Caller executes RST 08 (or CALL 0xFFF0)
;   3. RST 08 vector at 0x0008 is JP 0xFFF0
;   4. At 0xFFF0: OUT (0xEF),A triggers emulator dispatch
;   5. Emulator reads B,C,D,E,H,L, performs operation, sets A=result
;   6. RET instruction returns to caller
;
; This approach is robust because:
;   - RST vectors have normal C3 (JP) instructions that code can inspect
;   - I/O port trap works regardless of memory bank configuration
;   - No PC-based trapping that can break during bank switches
;
; Memory Layout:
;   0x0000-0x00FF  Page zero (RST vectors, etc.)
;   0x0100-0x03FF  HCB and startup code
;   0x0400-0x05FF  Proxy image (copied to 0xFE00 at startup)
;   0xFE00-0xFFFF  HBIOS proxy (in upper RAM after copy)
;
; Assemble with: um80 -g emu_hbios.asm; ul80 -o emu_hbios.bin -p 0000 emu_hbios.rel
;
;==================================================================================================

EMU_SIGNAL_PORT	equ	0EEh		; Port to signal emulator (init/status)
EMU_DISPATCH_PORT equ	0EFh		; Port to trigger HBIOS dispatch
EMU_BNKCALL_PORT  equ	0EDh		; Port to trigger bank call (IX=addr, A=bank)
EMU_BNKCPY_PORT   equ	0ECh		; Port to trigger bank copy (params at 0xFFE2-0xFFE9)
HBX_LOC		equ	0FE00h		; Target location of proxy
HBX_SIZ		equ	0200h		; Size of proxy (512 bytes)

;==================================================================================================
; Page Zero - RST Vectors
;==================================================================================================

	org	0000h

RST00:
	di
	jp	HB_START		; Cold boot
	db	0			; Signature pointer at 0x0004
	dw	ROM_SIG

	org	0008h
RST08:
	jp	0FFF0h			; HBIOS API entry (emulator traps here)

	org	0010h
RST10:	ret

	org	0018h
RST18:	ret

	org	0020h
RST20:	ret

	org	0028h
RST28:	ret

	org	0030h
RST30:	ret

	org	0038h
RST38:
	ei
	reti				; IM1 interrupt handler

	org	0066h
NMI66:
	retn				; NMI handler

	org	0070h

;==================================================================================================
; ROM Signature
;==================================================================================================

ROM_SIG:
	db	076h, 0B5h		; Signature bytes
	db	1			; Structure version
	db	7			; ROM size (multiples of 4KB - 1)
	dw	NAME			; Pointer to ROM name
	dw	AUTH			; Pointer to author
	dw	DESC			; Pointer to description
	ds	6, 0			; Reserved

NAME:	db	"EMU HBIOS v1.0",0
AUTH:	db	"EMU",0
DESC:	db	"Emulator HBIOS - traps to cpmemu",0

;==================================================================================================
; HBIOS Configuration Block (HCB) at 0x0100
;==================================================================================================

	org	0100h

HCB:
	jp	HB_START		; Entry point (offset 0x00)

CB_MARKER:	db	'W', 0A8h	; Marker ('W', ~'W'=0xA8) (offset 0x03)
CB_VERSION:	db	035h		; Version 3.5 (offset 0x05)
		db	010h		; Update/patch 1.0 -> v3.5.1 (offset 0x06)

CB_PLATFORM:	db	0		; Platform (0 = EMU) (offset 0x07)
CB_CPUMHZ:	db	4		; 4 MHz (offset 0x08)
CB_CPUKHZ:	dw	4000		; 4000 KHz (offset 0x09)
CB_RAMBANKS:	db	16		; 512KB / 32KB = 16 banks (offset 0x0B)
CB_ROMBANKS:	db	16		; 512KB / 32KB = 16 banks (offset 0x0C)

CB_BOOTVOL:	dw	0		; Boot volume (unit/slice) (offset 0x0D)
CB_BOOTBID:	db	0		; Boot bank ID (offset 0x0F)
CB_SERDEV:	db	0		; Primary serial unit (offset 0x10)
CB_CRTDEV:	db	0FFh		; Primary CRT unit (0xFF = none) (offset 0x11)
CB_CONDEV:	db	0FFh		; Console unit (0xFF = use default) (offset 0x12)

CB_DIAGLVL:	db	4		; Diagnostic level (4 = standard) (offset 0x13)
CB_BOOTMODE:	db	1		; Boot mode (1 = menu) (offset 0x14)

	org	0120h			; Heap info at offset 0x20

CB_HEAP:	dw	0		; Heap start (offset 0x20)
CB_HEAPTOP:	dw	0		; Heap top (offset 0x22)

	org	0130h			; Switch info at offset 0x30

CB_SWITCHES:	db	0		; NVR status (offset 0x30)
CB_SW_AB_OPT:	dw	0		; Auto boot options (offset 0x31)
CB_SW_AB_CFG:	db	0		; Auto boot config (offset 0x33)
CB_SW_CKSUM:	db	0		; Checksum (offset 0x34)

	org	01D8h			; Bank IDs at offset 0xD8

; Bank IDs at offset 0xD8 (per hbios.inc)
CB_BIDCOM:	db	08Fh		; Common bank (offset 0xD8)
CB_BIDUSR:	db	08Eh		; User bank (offset 0xD9)
CB_BIDBIOS:	db	080h		; BIOS bank (offset 0xDA)
CB_BIDAUX:	db	08Dh		; Aux bank (offset 0xDB)
CB_BIDRAMD0:	db	081h		; RAM disk start (offset 0xDC)
CB_RAMD_BNKS:	db	8		; RAM disk banks (offset 0xDD)
CB_BIDROMD0:	db	004h		; ROM disk start bank (offset 0xDE) - bank 4 to skip HBIOS/romldr
CB_ROMD_BNKS:	db	12		; ROM disk banks (offset 0xDF)
CB_BIDAPP0:	db	089h		; App bank start (offset 0xE0)
CB_APP_BNKS:	db	3		; App bank count (offset 0xE1)

	org	0200h			; HB_START at 0x200

;==================================================================================================
; HB_START - System initialization
;==================================================================================================

HB_START:
	di
	im	1

	; Set up stack in upper memory (below where proxy will go)
	ld	sp, HBX_LOC

	; Signal emulator that HBIOS is starting
	ld	a, 001h			; Signal: HBIOS starting
	out	(EMU_SIGNAL_PORT), a

	; Install proxy at 0xFE00
	ld	hl, HBX_IMG		; Source: proxy image
	ld	de, HBX_LOC		; Dest: top of RAM
	ld	bc, HBX_SIZ		; Size: 512 bytes
	ldir

	; Signal emulator that proxy is installed and provide trap addresses
	ld	a, 002h			; Signal: proxy ready
	out	(EMU_SIGNAL_PORT), a

	; Output CIO_DISPATCH address (emulator will trap here)
	ld	hl, CIO_DISPATCH
	ld	a, l
	out	(EMU_SIGNAL_PORT), a
	ld	a, h
	out	(EMU_SIGNAL_PORT), a

	; Output DIO_DISPATCH address
	ld	hl, DIO_DISPATCH
	ld	a, l
	out	(EMU_SIGNAL_PORT), a
	ld	a, h
	out	(EMU_SIGNAL_PORT), a

	; Output RTC_DISPATCH address
	ld	hl, RTC_DISPATCH
	ld	a, l
	out	(EMU_SIGNAL_PORT), a
	ld	a, h
	out	(EMU_SIGNAL_PORT), a

	; Output SYS_DISPATCH address
	ld	hl, SYS_DISPATCH
	ld	a, l
	out	(EMU_SIGNAL_PORT), a
	ld	a, h
	out	(EMU_SIGNAL_PORT), a

	; Signal done - emulator should start trapping
	ld	a, 0FFh			; Signal: init complete
	out	(EMU_SIGNAL_PORT), a

	; Jump to romldr in bank 1
	; We need to execute from common RAM (>= 0x8000) when switching banks,
	; otherwise after SYSSETBNK returns, we'll be executing bank 1's code
	; at this address instead of ours.
	;
	; Copy the bank switch trampoline to common RAM and execute it there.
	ld	hl, BANK_TRAMPOLINE	; Source: trampoline code
	ld	de, 0FD00h		; Dest: in common RAM
	ld	bc, BANK_TRAMPOLINE_END - BANK_TRAMPOLINE
	ldir
	jp	0FD00h			; Jump to trampoline in RAM

;--------------------------------------------------------------------------------------------------
; Bank switch trampoline - gets copied to common RAM and executed there
;--------------------------------------------------------------------------------------------------
BANK_TRAMPOLINE:
	ld	b, 0F2h			; SYSSETBNK
	ld	c, 001h			; Bank 1 (romldr)
	rst	08h			; Emulator switches to ROM bank 1
	jp	0000h			; Now jump to romldr entry point in bank 1
BANK_TRAMPOLINE_END:


;==================================================================================================
; HB_INVOKE - HBIOS API entry (called via RST 08)
;==================================================================================================

HB_INVOKE:
	; Route to appropriate dispatcher based on function code in B
	ld	a, b
	cp	010h
	jp	c, CIO_DISPATCH		; 0x00-0x0F: Character I/O
	cp	020h
	jp	c, DIO_DISPATCH		; 0x10-0x1F: Disk I/O
	cp	030h
	jp	c, RTC_DISPATCH		; 0x20-0x2F: RTC
	cp	0F0h
	jp	nc, SYS_DISPATCH	; 0xF0-0xFF: System
	; Other functions not implemented
	ld	a, 0FFh			; Error: not implemented
	ret

;==================================================================================================
; CIO_DISPATCH - Character I/O dispatch
; Uses I/O port 0xEF to trigger emulator dispatch
;==================================================================================================

CIO_DISPATCH:
	; B = function (0x00-0x0F)
	; C = unit
	; E = character (for output)
	; Returns: A = status, E = character (for input)
	out	(EMU_DISPATCH_PORT), a	; Trigger emulator dispatch
	ret				; Emulator has set A with result

;==================================================================================================
; DIO_DISPATCH - Disk I/O dispatch
; Uses I/O port 0xEF to trigger emulator dispatch
;==================================================================================================

DIO_DISPATCH:
	; B = function (0x10-0x1F)
	; C = unit
	; DE:HL = LBA (for seek)
	; HL = buffer, DE = count (for read/write)
	; Returns: A = status, E = sectors read/written
	out	(EMU_DISPATCH_PORT), a	; Trigger emulator dispatch
	ret				; Emulator has set A with result

;==================================================================================================
; RTC_DISPATCH - Real-time clock dispatch
;==================================================================================================

RTC_DISPATCH:
	; B = function (0x20-0x2F)
	out	(EMU_DISPATCH_PORT), a	; Trigger emulator dispatch
	ret

;==================================================================================================
; SYS_DISPATCH - System functions dispatch
;==================================================================================================

SYS_DISPATCH:
	; B = function (0xF0-0xFF)
	; Handles: SYSRESET, SYSVER, SYSSETBNK, SYSGETBNK, etc.
	out	(EMU_DISPATCH_PORT), a	; Trigger emulator dispatch
	ret

;==================================================================================================
; HBIOS Proxy Image (will be copied to 0xFE00)
;==================================================================================================

	org	0500h			; Storage location in ROM (must be after data strings)

HBX_IMG:
	; This proxy gets copied to 0xFE00-0xFFFF at startup
	; All addresses must work at target location

; Offset 0x00: Ident block
	db	'W', 0A8h		; Marker ('W', ~'W'=0xA8)
	db	035h			; Version 3.5
	db	010h			; Update/patch 1.0 -> v3.5.1

; Offset 0x04: Entry point for HBIOS call from proxy
	jp	HB_INVOKE		; Goes to main HBIOS code in ROM bank

; Offset 0x07: Bank switching (updates CURBNK then selects bank)
HBX_BNKSEL_START equ $ - HBX_IMG
	ld	(0FFE0h), a		; Update CURBNK at 0xFFE0
	bit	7, a			; Check RAM/ROM bit
	jr	z, HBX_SEL_ROM		; Jump if ROM
	out	(078h), a		; RAM bank select port
	ret
HBX_SEL_ROM:
	out	(07Ch), a		; ROM bank select port
	ret

	org	06E0h			; PMGMT at offset 0x1E0 within HBX_IMG (0xFFE0 when installed)

; Offset 0x1E0: HBIOS Proxy Management Block (at 0xFFE0 when installed)
PMGMT_CURBNK:	db	000h		; Current bank ID
PMGMT_INVBNK:	db	0FFh		; Invocation bank
PMGMT_SRCADR:	dw	0		; Bank copy source
PMGMT_SRCBNK:	db	08Eh		; Bank copy source bank
PMGMT_DSTADR:	dw	0		; Bank copy dest
PMGMT_DSTBNK:	db	08Eh		; Bank copy dest bank
PMGMT_CPYLEN:	dw	0		; Bank copy length
		dw	0		; Reserved
		dw	0		; Reserved
PMGMT_RTCLATCH:	db	0		; RTC latch shadow
PMGMT_LOCK:	db	0FEh		; Mutex lock

	org	06F0h			; Entry points at offset 0x1F0 (0xFFF0 when installed)

; Offset 0x1F0: Fixed address entry points (at 0xFFF0 when installed)
	out	(EMU_DISPATCH_PORT), a	; 0xFFF0: HBIOS invoke (triggers emulator)
	ret				; 0xFFF2: Return to caller
	jp	HBX_LOC + HBX_BNKSEL_START ; 0xFFF3: Bank select (in proxy)
	out	(EMU_BNKCPY_PORT), a	; 0xFFF6: Bank copy (triggers emulator)
	ret				; 0xFFF8: Return after copy
	out	(EMU_BNKCALL_PORT), a	; 0xFFF9: Bank call (trigger emulator)
	ret				; 0xFFFB: Return after emulator handles it
	dw	HBX_LOC			; 0xFFFC: Ident pointer -> proxy start
	dw	HBX_LOC			; 0xFFFE: Reserved

HBX_IMG_END:

;==================================================================================================
; End of EMU HBIOS
;==================================================================================================

	end
