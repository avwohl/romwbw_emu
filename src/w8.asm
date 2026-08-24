; W8.COM - Write CP/M file to host filesystem (RomWBW/HBIOS version)
;
; Usage: W8 <cpmname> [hostpath]
;   Exports a CP/M file to the host.  With no hostpath the name is the CP/M
;   name lowercased, in the emulator's working directory - what this did
;   before hostpath existed.  With a hostpath the file goes exactly there,
;   the way R8 already takes one.
;
;   CP/M's CCP uppercases the whole command line, so the path arrives here in
;   upper case and the emulator is what puts the case back: it resolves the
;   directory components case-insensitively and lowercases the final name.
;
; Uses HBIOS extension functions for host file access

	.z80

; CP/M addresses
TPA	equ	0100h
FCB	equ	005Ch	; Default FCB (contains CP/M filename)
DMA	equ	0080h
CMDBUF	equ	0080h	; Command tail (length byte, then text) - the same address
			; as the default DMA, so it must be read before F_DMA
			; moves the buffer and a record read overwrites it

; BDOS function codes
BDOS	equ	0005h
C_WRITE	equ	2
C_PRINT	equ	9
F_OPEN	equ	15
F_CLOSE	equ	16
F_READ	equ	20
F_DMA	equ	26

; HBIOS extension functions for host file transfer
H_OPEN_W equ	0E2h	; Open host file for writing (DE=path)
H_WRITE	equ	0E4h	; Write byte (E=byte)
H_CLOSE	equ	0E5h	; Close file (C=0 read, C=1 write)

	org	TPA

start:
	; Print banner
	ld	de,msg_banner
	ld	c,C_PRINT
	call	BDOS

	; Read the optional host path off the command tail first: it lives at
	; 0080h, which is also the default DMA, so nothing may move the DMA
	; before this runs.
	call	parse_host_arg

	; Check if we have a filename in FCB
	ld	a,(FCB+1)
	cp	' '
	jp	z,no_args

	; Copy FCB to our FCB area
	ld	hl,FCB
	ld	de,cpm_fcb
	ld	bc,36
	ldir

	; Zero extent and record count for open
	xor	a
	ld	(cpm_fcb+12),a
	ld	(cpm_fcb+32),a

	; Display CP/M filename
	ld	de,msg_writing
	ld	c,C_PRINT
	call	BDOS
	call	print_fcb_name
	ld	de,msg_crlf
	ld	c,C_PRINT
	call	BDOS

	; Host path: the second command-line token when one was given, otherwise
	; the CP/M name lowercased (the original behaviour).
	ld	a,(have_hostarg)
	or	a
	call	z,fcb_to_hostpath

	; Display host path
	ld	de,msg_tohost
	ld	c,C_PRINT
	call	BDOS
	ld	de,hostpath
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	call	BDOS

	; Open CP/M file for reading
	ld	de,cpm_fcb
	ld	c,F_OPEN
	call	BDOS
	cp	0FFh
	jp	z,cpm_open_error

	; Open host file for writing
	ld	de,hostpath
	ld	b,H_OPEN_W
	rst	8
	or	a
	jp	nz,host_open_error

	; Set DMA to our buffer
	ld	de,dma_buffer
	ld	c,F_DMA
	call	BDOS

	; Initialize counters
	ld	hl,0
	ld	(byte_count),hl
	ld	(byte_count+2),hl

	; Read loop
read_loop:
	; Read record from CP/M file
	ld	de,cpm_fcb
	ld	c,F_READ
	call	BDOS
	or	a
	jr	nz,read_done

	; Write 128 bytes to host (or until ^Z)
	ld	hl,dma_buffer
	ld	b,128

write_loop:
	ld	a,(hl)

	; ^Z means EOF
	cp	1Ah
	jr	z,read_done

	; Write byte to host
	ld	e,a
	push	hl
	push	bc
	ld	b,H_WRITE
	rst	8
	pop	bc
	pop	hl
	or	a
	jp	nz,host_write_error

	; Increment byte count
	push	hl
	push	bc
	ld	hl,(byte_count)
	inc	hl
	ld	(byte_count),hl
	ld	a,h
	or	l
	jr	nz,no_high_inc
	ld	hl,(byte_count+2)
	inc	hl
	ld	(byte_count+2),hl
no_high_inc:
	pop	bc
	pop	hl

	inc	hl
	djnz	write_loop
	jr	read_loop

read_done:
	; Close host file - A=0 on success; nonzero means the final flush
	; failed and the host file may be truncated (e.g. disk full)
	ld	b,H_CLOSE
	ld	c,1
	rst	8
	or	a
	jp	nz,host_close_error

	; Close CP/M file
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS

	; Print success message
	ld	de,msg_done
	ld	c,C_PRINT
	call	BDOS

	; Print byte count
	ld	hl,(byte_count)
	call	print_dec16

	ld	de,msg_bytes
	ld	c,C_PRINT
	call	BDOS

	rst	0

; Parse an optional second token from the command tail into hostpath.
;
; The tail is "<cpmname> <hostpath>" including the CCP's leading space, so this
; skips spaces, steps over the first token, skips spaces again, and copies what
; is left up to the next space.  Anything short of a full second token - empty
; tail, all spaces, one token only, trailing spaces - leaves have_hostarg zero
; and the caller falls back to fcb_to_hostpath.
;
; Clobbers A, BC, DE, HL.
parse_host_arg:
	xor	a
	ld	(have_hostarg),a
	ld	a,(CMDBUF)
	or	a
	ret	z		; empty tail
	ld	b,a		; B = bytes left in the tail
	ld	hl,CMDBUF+1

pha_skip1:			; leading spaces
	ld	a,(hl)
	cp	' '
	jr	nz,pha_tok1
	inc	hl
	djnz	pha_skip1
	ret			; nothing but spaces

pha_tok1:			; step over the CP/M filename
	ld	a,(hl)
	cp	' '
	jr	z,pha_skip2
	inc	hl
	djnz	pha_tok1
	ret			; one token only - no host path given

pha_skip2:			; spaces between the two tokens
	ld	a,(hl)
	cp	' '
	jr	nz,pha_copy
	inc	hl
	djnz	pha_skip2
	ret			; trailing spaces only

pha_copy:			; copy the host path
	ld	de,hostpath
	ld	c,127		; the tail cannot exceed 127, so this cap is only a
			; backstop - it can no longer be reached by any input
pha_loop:
	ld	a,(hl)
	cp	' '
	jr	z,pha_end
	ld	(de),a
	inc	hl
	inc	de
	dec	c
	jr	z,pha_end	; unreachable for a legal tail; here so a corrupt
			; length byte cannot run off the buffer
	djnz	pha_loop
pha_end:
	xor	a
	ld	(de),a		; NUL-terminate
	ld	a,1
	ld	(have_hostarg),a
	ret

; Create host path from FCB (8.3 -> lowercase)
fcb_to_hostpath:
	ld	hl,cpm_fcb+1
	ld	de,hostpath
	ld	b,8

copy_name:
	ld	a,(hl)
	cp	' '
	jr	z,do_dot
	call	tolower
	ld	(de),a
	inc	hl
	inc	de
	djnz	copy_name

do_dot:
	; Skip to extension in FCB
	ld	hl,cpm_fcb+9
	ld	a,(hl)
	cp	' '
	jr	z,name_done

	; Add dot
	ld	a,'.'
	ld	(de),a
	inc	de

	; Copy extension
	ld	b,3
copy_ext:
	ld	a,(hl)
	cp	' '
	jr	z,name_done
	call	tolower
	ld	(de),a
	inc	hl
	inc	de
	djnz	copy_ext

name_done:
	xor	a
	ld	(de),a
	ret

; Convert A to lowercase
tolower:
	cp	'A'
	ret	c
	cp	'Z'+1
	ret	nc
	; requires um80 >= 0.3.43: older versions uppercased char literals in
	; add a,<expr> operands, miscompiling this as add a,0
	add	a,'a'-'A'
	ret

; Print null-terminated string at DE
print_string:
	ld	a,(de)
	or	a
	ret	z
	push	de
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	de
	inc	de
	jr	print_string

; Print FCB filename (8.3 format)
print_fcb_name:
	ld	hl,cpm_fcb+1
	ld	b,8
print_name:
	ld	a,(hl)
	cp	' '
	jr	z,print_dot
	push	hl
	push	bc
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	bc
	pop	hl
	inc	hl
	djnz	print_name
print_dot:
	ld	hl,cpm_fcb+9
	ld	a,(hl)
	cp	' '
	ret	z
	push	hl
	ld	e,'.'
	ld	c,C_WRITE
	call	BDOS
	pop	hl
	ld	b,3
print_ext:
	ld	a,(hl)
	cp	' '
	ret	z
	push	hl
	push	bc
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	bc
	pop	hl
	inc	hl
	djnz	print_ext
	ret

; Print HL as decimal number
print_dec16:
	xor	a
	ld	(print_flag),a
	ld	de,10000
	call	div16
	ld	de,1000
	call	div16
	ld	de,100
	call	div16
	ld	de,10
	call	div16
	ld	a,l
	add	a,'0'
	ld	e,a
	ld	c,C_WRITE
	jp	BDOS

; Divide HL by DE, print quotient digit, remainder in HL
div16:
	ld	b,0
div_loop:
	or	a
	sbc	hl,de
	jr	c,div_done
	inc	b
	jr	div_loop
div_done:
	add	hl,de
	ld	a,b
	or	a
	jr	z,skip_zero
	add	a,'0'
	push	hl
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	hl
	ld	a,1
	ld	(print_flag),a
	ret
skip_zero:
	ld	a,(print_flag)
	or	a
	ret	z
	ld	a,'0'
	push	hl
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	hl
	ret

; Error handlers
no_args:
	ld	de,msg_usage
	ld	c,C_PRINT
	call	BDOS
	rst	0

cpm_open_error:
	ld	de,msg_cpm_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

host_open_error:
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_host_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

host_write_error:
	ld	b,H_CLOSE
	ld	c,1
	rst	8
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_host_write
	ld	c,C_PRINT
	call	BDOS
	rst	0

; Host file already closed (that is what failed) - do not re-issue H_CLOSE
host_close_error:
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_host_close
	ld	c,C_PRINT
	call	BDOS
	rst	0

; Messages
msg_banner:
	db	'W8 - Write to host filesystem',0Dh,0Ah,'$'
msg_usage:
	db	'Usage: W8 <cpmname> [hostpath]',0Dh,0Ah,'$'
msg_writing:
	db	'Writing: $'
msg_tohost:
	db	'To host: $'
msg_crlf:
	db	0Dh,0Ah,'$'
msg_done:
	db	'Done: $'
msg_bytes:
	db	' bytes',0Dh,0Ah,'$'
msg_cpm_err:
	db	'Error: Cannot open CP/M file',0Dh,0Ah,'$'
msg_host_err:
	db	'Error: Cannot create host file',0Dh,0Ah,'$'
msg_host_write:
	db	'Error: Host write failed',0Dh,0Ah,'$'
msg_host_close:
	db	'Host file close failed - file may be truncated',0Dh,0Ah,'$'

; Data areas
hostpath:
	ds	128		; the whole CP/M tail can be 127 bytes + NUL.  Was 64,
			; which was enough for the 8.3 name fcb_to_hostpath
			; builds but silently cut a typed path at 63 - and
			; still reported success, so the export landed under a
			; shortened name.  R8's buffer is 128 for the same
			; reason; the two now accept the same paths.
cpm_fcb:
	ds	36
dma_buffer:
	ds	128
byte_count:
	dw	0,0
print_flag:
	db	0
have_hostarg:
	db	0		; non-zero once parse_host_arg found a second token

	end	start
