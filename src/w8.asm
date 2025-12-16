; W8.COM - Write CP/M file to host filesystem (RomWBW/HBIOS version)
;
; Usage: W8 <cpmname>
;   Exports CP/M file to host with lowercase filename
;
; Uses HBIOS extension functions for host file access

	.z80

; CP/M addresses
TPA	equ	0100h
FCB	equ	005Ch	; Default FCB (contains CP/M filename)
DMA	equ	0080h

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

	; Create host path from FCB (lowercase)
	call	fcb_to_hostpath

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
	; Close host file
	ld	b,H_CLOSE
	ld	c,1
	rst	8

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

; Messages
msg_banner:
	db	'W8 - Write to host filesystem',0Dh,0Ah,'$'
msg_usage:
	db	'Usage: W8 <cpmname>',0Dh,0Ah,'$'
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

; Data areas
hostpath:
	ds	64
cpm_fcb:
	ds	36
dma_buffer:
	ds	128
byte_count:
	dw	0,0
print_flag:
	db	0

	end	start
