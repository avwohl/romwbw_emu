;==================================================================================================
; EMU_ROM.ASM - Minimal ROM for cpmemu emulator with HBIOS API
;==================================================================================================
;
; This is a standalone boot ROM that provides the HBIOS API by trapping to
; the emulator. It doesn't use the full RomWBW build - instead it's a minimal
; ROM that:
;   1. Sets up the RST 08 vector to trap to the emulator
;   2. Provides a simple boot menu
;   3. Loads and runs CP/M or another OS from disk
;
; The emulator's handle_hbios_call() does all the actual work.
;
; HBIOS API (via RST 08):
;   B = Function code, C = Unit/subfunction
;   DE, HL = Parameters
;   Returns: A = status, DE/HL = results
;
;==================================================================================================

; HBIOS Function Constants
BF_CIOIN	equ	00h	; Console input (wait for char)
BF_CIOOUT	equ	01h	; Console output
BF_CIOIST	equ	02h	; Console input status
BF_CIOOST	equ	03h	; Console output status

BF_DIOSTATUS	equ	10h	; Disk status
BF_DIORESET	equ	11h	; Disk reset
BF_DIOSEEK	equ	12h	; Disk seek
BF_DIOREAD	equ	13h	; Disk read
BF_DIOWRITE	equ	14h	; Disk write

BF_SYSRESET	equ	0F0h	; System reset
BF_SYSVER	equ	0F1h	; Get version
BF_SYSSETBNK	equ	0F2h	; Set bank
BF_SYSGETBNK	equ	0F3h	; Get bank

	org	0000h

;--------------------------------------------------------------------------------------------------
; RST 00 - Cold boot entry
;--------------------------------------------------------------------------------------------------
RST00:
	di			; Disable interrupts
	jp	COLD_BOOT	; Jump to initialization

	ds	0008h - $, 0	; Pad to RST 08

;--------------------------------------------------------------------------------------------------
; RST 08 - HBIOS API entry point
;--------------------------------------------------------------------------------------------------
; The emulator traps this address and handles the HBIOS call.
; When the emulator sees PC=0x0008 with opcode 0xC7 (RST 00) or similar,
; it reads registers B,C,DE,HL, performs the function, sets results,
; and simulates RET.
;--------------------------------------------------------------------------------------------------
RST08:
	; This instruction is intercepted by the emulator
	; The emulator handles the call and returns
	ret			; Emulator will intercept before this executes

	ds	0010h - $, 0

RST10:
	ret
	ds	0018h - $, 0

RST18:
	ret
	ds	0020h - $, 0

RST20:
	ret
	ds	0028h - $, 0

RST28:
	ret
	ds	0030h - $, 0

RST30:
	ret
	ds	0038h - $, 0

;--------------------------------------------------------------------------------------------------
; RST 38 / INT - Interrupt handler (IM1)
;--------------------------------------------------------------------------------------------------
RST38:
	reti

	ds	0066h - $, 0

;--------------------------------------------------------------------------------------------------
; NMI - Non-maskable interrupt handler
;--------------------------------------------------------------------------------------------------
NMI:
	retn

;--------------------------------------------------------------------------------------------------
; COLD_BOOT - System initialization
;--------------------------------------------------------------------------------------------------
	org	0100h

COLD_BOOT:
	; Initialize stack in high RAM (common area, always visible)
	ld	sp, 0FFF0h

	; Stay in ROM bank 0 for now - don't switch to RAM
	; The upper 32K (0x8000-0xFFFF) is always common RAM (bank 0x8F)
	; The lower 32K is currently ROM bank 0 where our code resides

	; Print boot message
	ld	hl, MSG_BOOT
	call	PRINT_STR

	; Get and print HBIOS version
	ld	b, BF_SYSVER
	rst	08h
	; DE = version (major.minor in D.E format)
	push	de
	ld	hl, MSG_VER
	call	PRINT_STR
	pop	de
	ld	a, d
	call	PRINT_HEX
	ld	a, '.'
	call	PRINT_CHAR
	ld	a, e
	call	PRINT_HEX
	call	PRINT_CRLF

	; Print prompt
	ld	hl, MSG_READY
	call	PRINT_STR

	; Simple command loop
CMD_LOOP:
	; Print prompt
	ld	a, '>'
	call	PRINT_CHAR

	; Get command
	call	GET_LINE
	ld	hl, LINE_BUF

	; Check for empty line
	ld	a, (hl)
	or	a
	jr	z, CMD_LOOP

	; Check commands
	cp	'B'
	jr	z, CMD_BOOT
	cp	'b'
	jr	z, CMD_BOOT
	cp	'R'
	jr	z, CMD_RESET
	cp	'r'
	jr	z, CMD_RESET
	cp	'?'
	jr	z, CMD_HELP
	cp	'H'
	jr	z, CMD_HELP
	cp	'h'
	jr	z, CMD_HELP

	; Unknown command
	ld	hl, MSG_UNKNOWN
	call	PRINT_STR
	jr	CMD_LOOP

;--------------------------------------------------------------------------------------------------
; CMD_HELP - Show help
;--------------------------------------------------------------------------------------------------
CMD_HELP:
	ld	hl, MSG_HELP
	call	PRINT_STR
	jr	CMD_LOOP

;--------------------------------------------------------------------------------------------------
; CMD_BOOT - Boot from disk
;--------------------------------------------------------------------------------------------------
CMD_BOOT:
	ld	hl, MSG_BOOTING
	call	PRINT_STR

	; Reset disk
	ld	b, BF_DIORESET
	ld	c, 0		; Unit 0
	rst	08h

	; Seek to sector 0
	ld	b, BF_DIOSEEK
	ld	c, 0		; Unit 0
	ld	d, 80h		; LBA mode flag
	ld	e, 0		; LBA high
	ld	hl, 0		; LBA low = 0
	rst	08h
	or	a
	jr	nz, BOOT_ERR

	; Read boot sector to 0x8000 (temp location)
	ld	b, BF_DIOREAD
	ld	c, 0		; Unit 0
	ld	d, 80h		; Current bank
	ld	e, 1		; 1 block (512 bytes)
	ld	hl, 8000h	; Load address (temp)
	rst	08h
	or	a
	jr	nz, BOOT_ERR

	; Copy to 0x0100 and jump
	ld	hl, 8000h
	ld	de, 0100h
	ld	bc, 512
	ldir

	; Jump to loaded code
	jp	0100h

BOOT_ERR:
	ld	hl, MSG_BOOTERR
	call	PRINT_STR
	jp	CMD_LOOP

;--------------------------------------------------------------------------------------------------
; CMD_RESET - Warm reset
;--------------------------------------------------------------------------------------------------
CMD_RESET:
	ld	b, BF_SYSRESET
	rst	08h
	jp	0		; Should not return

;--------------------------------------------------------------------------------------------------
; PRINT_STR - Print null-terminated string at HL
;--------------------------------------------------------------------------------------------------
PRINT_STR:
	ld	a, (hl)
	or	a
	ret	z
	call	PRINT_CHAR
	inc	hl
	jr	PRINT_STR

;--------------------------------------------------------------------------------------------------
; PRINT_CHAR - Print character in A
;--------------------------------------------------------------------------------------------------
PRINT_CHAR:
	push	bc
	push	de
	ld	e, a
	ld	b, BF_CIOOUT
	ld	c, 0		; Unit 0
	rst	08h
	pop	de
	pop	bc
	ret

;--------------------------------------------------------------------------------------------------
; PRINT_HEX - Print A as 2 hex digits
;--------------------------------------------------------------------------------------------------
PRINT_HEX:
	push	af
	rrca
	rrca
	rrca
	rrca
	call	PRINT_NIB
	pop	af
PRINT_NIB:
	and	0Fh
	cp	10
	jr	c, PN_DIG
	add	a, 'A' - 10
	jr	PN_OUT
PN_DIG:
	add	a, '0'
PN_OUT:
	jp	PRINT_CHAR

;--------------------------------------------------------------------------------------------------
; PRINT_CRLF - Print CR/LF
;--------------------------------------------------------------------------------------------------
PRINT_CRLF:
	ld	a, 13
	call	PRINT_CHAR
	ld	a, 10
	jp	PRINT_CHAR

;--------------------------------------------------------------------------------------------------
; GET_CHAR - Get character, return in A
;--------------------------------------------------------------------------------------------------
GET_CHAR:
	push	bc
	push	de
	ld	b, BF_CIOIN
	ld	c, 0		; Unit 0
	rst	08h
	ld	a, e		; Char returned in E
	pop	de
	pop	bc
	ret

;--------------------------------------------------------------------------------------------------
; GET_LINE - Get line into LINE_BUF, handle backspace
;--------------------------------------------------------------------------------------------------
GET_LINE:
	ld	hl, LINE_BUF
	ld	b, 0		; Count
GL_LOOP:
	call	GET_CHAR
	cp	13		; CR?
	jr	z, GL_DONE
	cp	10		; LF?
	jr	z, GL_DONE
	cp	8		; Backspace?
	jr	z, GL_BS
	cp	127		; Delete?
	jr	z, GL_BS
	; Store char
	ld	(hl), a
	inc	hl
	inc	b
	; Echo
	call	PRINT_CHAR
	; Check buffer full
	ld	a, b
	cp	78
	jr	c, GL_LOOP
GL_DONE:
	ld	(hl), 0		; Null terminate
	call	PRINT_CRLF
	ret
GL_BS:
	ld	a, b
	or	a
	jr	z, GL_LOOP	; Nothing to delete
	dec	hl
	dec	b
	; Echo backspace
	ld	a, 8
	call	PRINT_CHAR
	ld	a, ' '
	call	PRINT_CHAR
	ld	a, 8
	call	PRINT_CHAR
	jr	GL_LOOP

;--------------------------------------------------------------------------------------------------
; Messages
;--------------------------------------------------------------------------------------------------
MSG_BOOT:
	db	13, 10
	db	"EMU ROM v1.0 - HBIOS Emulator Bootstrap", 13, 10
	db	"(C) 2024 cpmemu project", 13, 10, 0

MSG_VER:
	db	"HBIOS Version: ", 0

MSG_READY:
	db	13, 10
	db	"Commands: B=Boot, R=Reset, H=Help", 13, 10, 0

MSG_HELP:
	db	13, 10
	db	"EMU ROM Commands:", 13, 10
	db	"  B - Boot from disk unit 0", 13, 10
	db	"  R - Reset system", 13, 10
	db	"  H - This help", 13, 10, 0

MSG_UNKNOWN:
	db	"Unknown command. Type H for help.", 13, 10, 0

MSG_BOOTING:
	db	"Booting from disk...", 13, 10, 0

MSG_BOOTERR:
	db	"Boot error!", 13, 10, 0

;--------------------------------------------------------------------------------------------------
; Data
;--------------------------------------------------------------------------------------------------
LINE_BUF:
	ds	80

	end
