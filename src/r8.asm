; R8.COM - Read host file to CP/M filesystem (RomWBW/HBIOS version)
;
; Usage: R8 <hostpath> [cpmname]
;   hostpath - path on host filesystem
;   cpmname  - optional CP/M filename (defaults to host filename)
;
; Uses HBIOS extension functions 0xE1-0xE7 for host file access
;
	.z80		; Enable Z80 mode

; CP/M addresses
TPA	equ	0100h
FCB	equ	005Ch	; Default FCB (second arg)
FCB2	equ	006Ch	; Second FCB area (part of default FCB)
CMDBUF	equ	0080h	; Command line buffer
DMA	equ	0080h	; Default DMA address

; BDOS function codes (still needed for CP/M file operations)
BDOS	equ	0005h
C_WRITE	equ	2	; Console output
C_PRINT	equ	9	; Print string
F_OPEN	equ	15	; Open file
F_CLOSE	equ	16	; Close file
F_SFIRST equ	17	; Search for first
F_DELETE equ	19	; Delete file
F_WRITE	equ	21	; Write sequential
F_MAKE	equ	22	; Make file
F_DMA	equ	26	; Set DMA address

; HBIOS extension functions for host file transfer
; Called via RST 8 with B = function code
H_OPEN_R equ	0E1h	; Open host file for reading (DE=path)
H_OPEN_W equ	0E2h	; Open host file for writing (DE=path)
H_READ	equ	0E3h	; Read byte (returns E=byte, A=status)
H_WRITE	equ	0E4h	; Write byte (E=byte)
H_CLOSE	equ	0E5h	; Close file (C=0 read, C=1 write)
H_MODE	equ	0E6h	; Get/set mode (C=0 get, C=1 set)
H_GETARG equ	0E7h	; Get arg (E=index, DE=buf)

	org	TPA

start:
	; Print banner
	ld	de,msg_banner
	ld	c,C_PRINT
	call	BDOS

	; Get first argument (host path) using HBIOS GETARG
	ld	de,hostpath
	ld	e,0		; Arg index 0 = first argument
	ld	b,H_GETARG
	rst	8
	or	a
	jp	nz,no_args	; No first argument

	; Display host path
	ld	de,msg_reading
	ld	c,C_PRINT
	call	BDOS
	ld	de,hostpath
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	call	BDOS

	; Check for second argument (CP/M name)
	ld	a,(FCB2+1)	; First char of second filename
	cp	' '
	jr	z,use_default
	or	a		; Also check for zero
	jr	z,use_default

	; Use FCB2 as our FCB
	ld	hl,FCB2
	ld	de,cpm_fcb
	ld	bc,12		; Copy drive + filename
	ldir
	jr	got_cpm_name

use_default:
	; Extract filename from host path (after last / or \)
	call	extract_filename
	; Convert to FCB format (8.3, uppercase)
	call	name_to_fcb

got_cpm_name:
	; Zero extent and record fields
	xor	a
	ld	(cpm_fcb+12),a	; EX
	ld	(cpm_fcb+32),a	; CR

	; Display CP/M filename
	ld	de,msg_creating
	ld	c,C_PRINT
	call	BDOS
	call	print_fcb_name
	ld	de,msg_crlf
	ld	c,C_PRINT
	call	BDOS

	; Open host file for reading
	ld	de,hostpath
	ld	b,H_OPEN_R
	rst	8
	or	a
	jp	nz,host_open_error

	; Delete existing CP/M file (if any)
	ld	de,cpm_fcb
	ld	c,F_DELETE
	call	BDOS

	; Create new CP/M file
	ld	de,cpm_fcb
	ld	c,F_MAKE
	call	BDOS
	cp	0FFh
	jp	z,cpm_create_error

	; Set DMA to our buffer
	ld	de,dma_buffer
	ld	c,F_DMA
	call	BDOS

	; Initialize counters
	ld	hl,0
	ld	(byte_count),hl
	ld	(byte_count+2),hl

	; Initialize buffer position
	ld	hl,dma_buffer
	ld	b,0		; Byte count in buffer

read_loop:
	; Read byte from host
	ld	a,b		; Save buffer count
	push	af
	push	hl

	ld	b,H_READ
	rst	8
	or	a
	jr	nz,read_done_pop	; EOF or error

	; Got byte in E
	pop	hl
	pop	af
	ld	b,a		; Restore buffer count

	; Store byte in buffer
	ld	(hl),e
	inc	hl
	inc	b

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

	; Buffer full?
	ld	a,b
	cp	128
	jr	nz,read_loop

	; Write buffer to CP/M file
	push	hl
	ld	de,cpm_fcb
	ld	c,F_WRITE
	call	BDOS
	pop	hl
	or	a
	jp	nz,cpm_write_error

	; Reset buffer
	ld	hl,dma_buffer
	ld	b,0
	jr	read_loop

read_done_pop:
	pop	hl
	pop	af
	ld	b,a

read_done:
	; Write any remaining bytes in buffer
	ld	a,b
	or	a
	jr	z,close_files

	; Pad buffer with ^Z to 128 bytes
	ld	a,b
pad_loop:
	cp	128
	jr	nc,write_final
	ld	(hl),1Ah	; ^Z
	inc	hl
	inc	a
	jr	pad_loop

write_final:
	ld	de,cpm_fcb
	ld	c,F_WRITE
	call	BDOS

close_files:
	; Close host file
	ld	b,H_CLOSE
	ld	c,0		; Close read file
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

	; Exit
	rst	0

; Extract filename from host path (after last / or \)
; Input: hostpath contains full path
; Output: filename buffer has just the filename part
extract_filename:
	ld	hl,hostpath
	ld	de,filename
	ld	bc,0		; BC = position of last separator + 1

extract_loop:
	ld	a,(hl)
	or	a
	jr	z,extract_done
	cp	'/'
	jr	z,found_sep
	cp	'\'
	jr	z,found_sep
	inc	hl
	jr	extract_loop

found_sep:
	inc	hl
	push	hl
	pop	bc		; BC = position after separator
	jr	extract_loop

extract_done:
	; BC points to start of filename (or hostpath if no separator)
	ld	a,b
	or	c
	jr	nz,copy_from_bc
	ld	bc,hostpath

copy_from_bc:
	ld	hl,filename
copy_fn_loop:
	ld	a,(bc)
	or	a
	jr	z,copy_fn_done
	ld	(hl),a
	inc	hl
	inc	bc
	jr	copy_fn_loop

copy_fn_done:
	xor	a
	ld	(hl),a
	ret

; Convert filename to FCB format (8.3, uppercase)
; Input: filename buffer
; Output: cpm_fcb filled
name_to_fcb:
	; Clear FCB with spaces
	ld	hl,cpm_fcb
	ld	(hl),0		; Drive = default
	inc	hl
	ld	b,11
clear_fcb:
	ld	(hl),' '
	inc	hl
	djnz	clear_fcb

	; Copy name part (up to 8 chars, before dot)
	ld	hl,filename
	ld	de,cpm_fcb+1
	ld	b,8

copy_name:
	ld	a,(hl)
	or	a
	jr	z,name_done
	cp	'.'
	jr	z,do_ext
	call	toupper
	ld	(de),a
	inc	hl
	inc	de
	djnz	copy_name

	; Skip to dot
skip_to_dot:
	ld	a,(hl)
	or	a
	jr	z,name_done
	cp	'.'
	jr	z,do_ext
	inc	hl
	jr	skip_to_dot

do_ext:
	inc	hl		; Skip dot
	ld	de,cpm_fcb+9
	ld	b,3

copy_ext:
	ld	a,(hl)
	or	a
	jr	z,name_done
	call	toupper
	ld	(de),a
	inc	hl
	inc	de
	djnz	copy_ext

name_done:
	ret

; Convert A to uppercase
toupper:
	cp	'a'
	ret	c
	cp	'z'+1
	ret	nc
	sub	'a'-'A'
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
	; Move to extension
	ld	hl,cpm_fcb+9
	ld	a,(hl)
	cp	' '
	ret	z		; No extension
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
	ld	b,0		; Quotient
div_loop:
	or	a
	sbc	hl,de
	jr	c,div_done
	inc	b
	jr	div_loop
div_done:
	add	hl,de		; Restore remainder
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

host_open_error:
	ld	de,msg_host_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

cpm_create_error:
	; Close host file first
	ld	b,H_CLOSE
	ld	c,0
	rst	8
	ld	de,msg_cpm_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

cpm_write_error:
	; Close both files
	ld	b,H_CLOSE
	ld	c,0
	rst	8
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_write_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

; Messages
msg_banner:
	db	'R8 - Read from host filesystem',0Dh,0Ah,'$'
msg_usage:
	db	'Usage: R8 <hostpath> [cpmname]',0Dh,0Ah
	db	'  hostpath - path on host filesystem',0Dh,0Ah
	db	'  cpmname  - optional CP/M filename',0Dh,0Ah,'$'
msg_reading:
	db	'Reading: $'
msg_creating:
	db	'Creating: $'
msg_crlf:
	db	0Dh,0Ah,'$'
msg_done:
	db	'Done: $'
msg_bytes:
	db	' bytes transferred',0Dh,0Ah,'$'
msg_host_err:
	db	'Error: Cannot open host file',0Dh,0Ah,'$'
msg_cpm_err:
	db	'Error: Cannot create CP/M file',0Dh,0Ah,'$'
msg_write_err:
	db	'Error: CP/M write failed',0Dh,0Ah,'$'

; Data areas
hostpath:
	ds	256		; Host path buffer
filename:
	ds	64		; Extracted filename
cpm_fcb:
	ds	36		; CP/M FCB
dma_buffer:
	ds	128		; DMA buffer
byte_count:
	dw	0,0		; 32-bit byte counter
print_flag:
	db	0		; For leading zero suppression

	end	start
