; R8.COM - Read host file to CP/M filesystem (RomWBW/HBIOS version)
;
; Usage: R8 <hostpath>
;   Imports host file to CP/M with uppercase filename.  The host path is the
;   whole command tail, not the first word of it, so a directory with a space
;   in its name works - /Users/me/My Documents on a Mac, C:\Program Files on
;   Windows.  Trailing spaces are trimmed.
;
;   The "Reading:" line names the file the emulator actually opened, not the
;   path that was typed, and it is printed after the open for that reason.  The
;   two are usually the same file - it has to exist for the open to succeed -
;   but not the same string: the CCP uppercases the command line, so the
;   emulator resolves it case-insensitively and answers with the absolute path
;   it settled on, and a front end that opens a file picker returns whatever
;   the user chose there.  W8 has asked the same question about its destination
;   since H_GETNAME; H_GETRNAME is the read twin.
;
;   An emulator built before H_GETRNAME existed answers "no such function", and
;   so does one whose front end cannot say; R8 then prints the requested path,
;   which is what it always printed.
;
; Uses HBIOS extension functions for host file access
;
; Build:  um80 -o r8.rel r8.asm && ul80 -o r8.com r8.rel
;   There is deliberately no ORG here - see the same note in w8.asm.  M80
;   assembles this as one relocatable code segment and L80 bases a .COM at
;   0100h by itself; an `org 0100h` in the source is applied on top of that
;   base and puts the code at 0200h behind 256 zero bytes.  The shipped r8.com
;   was built without one, which is why it and w8.com had different layouts and
;   only one of them could be checked against its source.

	.z80

; CP/M addresses
TPA	equ	0100h
CMDBUF	equ	0080h	; Command line: length byte + text
DMA	equ	0080h

; BDOS function codes
BDOS	equ	0005h
C_WRITE	equ	2
C_PRINT	equ	9
F_CLOSE	equ	16
F_DELETE equ	19
F_WRITE	equ	21
F_MAKE	equ	22
F_DMA	equ	26

; HBIOS extension functions for host file transfer
H_OPEN_R equ	0E1h	; Open host file for reading (DE=path)
H_READ	equ	0E3h	; Read byte (returns E=byte, A=status)
H_CLOSE	equ	0E5h	; Close file (C=0 read, C=1 write)
H_GETRNAME equ	0EAh	; Which file the open read is really reading (C=bufsize,
			; DE=buffer).  A=0 and the buffer holds a string; A<>0
			; on an emulator that does not implement it.  The read
			; twin of W8's H_GETNAME.
HRSIZE	equ	255	; size of hostreal, the buffer H_GETRNAME fills.  One
			; symbol for the ds and for the C the call is given: two
			; separate literals could drift apart, and the emulator
			; writes exactly as many bytes as C says - straight into
			; cpm_fcb and the DMA buffer behind it if C were larger.

start:
	; Print banner
	ld	de,msg_banner
	ld	c,C_PRINT
	call	BDOS

	; Get host path from command line at 0x80
	; Format: length byte, then text (starts with space after command)
	ld	a,(CMDBUF)
	or	a
	jp	z,no_args

	; Copy command tail to hostpath, skipping leading spaces.  What follows
	; the first non-space is taken whole, spaces and all: a host path is the
	; only argument, and a directory whose name contains a space is ordinary
	; on every host this runs on.  Only trailing spaces are dropped, so a
	; line the user ended with a space does not name a file ending in one.
	ld	hl,CMDBUF+1
	ld	de,hostpath
	ld	b,a		; length

skip_spaces:
	ld	a,(hl)
	cp	' '
	jr	nz,copy_path
	inc	hl
	djnz	skip_spaces
	jp	no_args		; all spaces = no argument

copy_path:
	ld	(hp_end),de	; one past the last non-space stored so far
	ld	c,127		; the tail cannot exceed 127, so this cap is only
			; a backstop - it can no longer be reached by any input.
			; W8 has always had one; this did not, so a corrupt
			; length byte at 0080h ran off the end of hostpath and
			; over the FCB and the DMA buffer behind it.
copy_loop:
	ld	a,(hl)
	ld	(de),a
	inc	hl
	inc	de
	cp	' '
	jr	z,copy_nospc
	ld	(hp_end),de	; remember where the path really ends
copy_nospc:
	dec	c
	jr	z,path_done
	djnz	copy_loop

path_done:
	; Terminate after the last non-space rather than after the last byte
	; copied, which trims trailing spaces without a second pass.
	ld	de,(hp_end)
	xor	a
	ld	(de),a		; null terminate

	; Check we got something
	ld	a,(hostpath)
	or	a
	jp	z,no_args

	; Extract filename from path and convert to FCB
	call	path_to_fcb

	; A path can end in a separator, which names a directory and no file.
	; That used to reach F_MAKE with the FCB's eleven blanks still in place
	; and create a nameless directory entry: a slot consumed, shown by DIR
	; as an empty name, and not erasable by name from the CCP.
	ld	a,(cpm_fcb+1)
	cp	' '
	jp	z,no_name_error

	; Open host file for reading.  This comes BEFORE both the "Reading:" and
	; the "Creating:" lines: announcing files and then failing to open the
	; host one told the user about a transfer that never started.
	ld	de,hostpath
	ld	b,H_OPEN_R
	rst	8
	or	a
	jp	nz,host_open_error

	; Now that it is open, say which file it actually is.  Not the path that
	; was typed: the CCP shouted it, so the emulator may have opened it only
	; on a case-insensitive retry and will answer with the absolute path it
	; settled on - and a front end with a file picker opens whatever the user
	; chose there, which need not resemble the typed string at all.  W8 has
	; asked this question about its destination since H_GETNAME; this is the
	; same question about the source.
	call	print_source

	; Display CP/M filename
	ld	de,msg_creating
	ld	c,C_PRINT
	call	BDOS
	call	print_fcb_name
	ld	de,msg_crlf
	ld	c,C_PRINT
	call	BDOS

	; Say so if the name is not the host's name.  Silence here was the
	; dangerous part of the old behaviour, not the substitution.
	ld	a,(name_mangled)
	or	a
	jr	z,name_ok
	ld	de,msg_mangled
	ld	c,C_PRINT
	call	BDOS
name_ok:

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
	ld	a,b
	push	af
	push	hl

	ld	b,H_READ
	rst	8
	or	a
	jr	nz,read_done_pop

	; Got byte in E
	pop	hl
	pop	af
	ld	b,a

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
	ld	(hl),1Ah
	inc	hl
	inc	a
	jr	pad_loop

write_final:
	ld	de,cpm_fcb
	ld	c,F_WRITE
	call	BDOS
	; Every full record above is checked; this one was not, so a CP/M disk
	; that filled on the last record closed the file and printed "Done"
	; for an import that is short by up to 127 bytes.
	or	a
	jp	nz,cpm_write_error

close_files:
	; Close host file
	ld	b,H_CLOSE
	ld	c,0
	rst	8

	; Close CP/M file.  F_CLOSE is where CP/M writes the directory entry
	; back, so a failure here means the data went to the disk and the
	; directory did not - the one place where "Done" would be furthest from
	; the truth.
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	cp	0FFh
	jp	z,cpm_close_error

	; Print success message
	ld	de,msg_done
	ld	c,C_PRINT
	call	BDOS

	; Print byte count
	call	print_dec32

	ld	de,msg_bytes
	ld	c,C_PRINT
	call	BDOS

	rst	0

; Print which host file is actually being read.
;
; H_GETRNAME answers with the source after the emulator has done whatever its
; platform does to the requested path.  A failure is not an error: an emulator
; that predates the call returns "no such function", and so does one whose front
; end cannot say (the browser, where the file came from a picker) - the
; requested path is then the best answer available, which is what this printed
; unconditionally before.
print_source:
	ld	de,msg_reading
	ld	c,C_PRINT
	call	BDOS
	ld	c,HRSIZE	; buffer size including the terminator
	ld	de,hostreal
	ld	b,H_GETRNAME
	rst	8
	or	a
	ld	de,hostreal
	jr	z,ps_show
	ld	de,hostpath	; older emulator, or one with no answer
ps_show:
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	jp	BDOS

; Extract filename from host path and convert to FCB format
; Finds text after last / or \, converts to uppercase 8.3
path_to_fcb:
	; Clear FCB with spaces
	ld	hl,cpm_fcb
	ld	(hl),0		; Drive = default
	inc	hl
	ld	b,11
clear_fcb:
	ld	(hl),' '
	inc	hl
	djnz	clear_fcb

	; Zero the rest of FCB
	ld	b,24
clear_rest:
	ld	(hl),0
	inc	hl
	djnz	clear_rest

	; Find last separator in hostpath
	ld	hl,hostpath
	ld	de,hostpath	; DE = start of filename part
find_sep:
	ld	a,(hl)
	or	a
	jr	z,got_filename
	cp	'/'
	jr	z,found_sep
	cp	'\'
	jr	z,found_sep
	inc	hl
	jr	find_sep
found_sep:
	inc	hl
	push	hl
	pop	de		; DE = after separator
	jr	find_sep

got_filename:
	; DE points at the basename.  A leading dot names a hidden file rather
	; than an empty name plus a type, so drop it and keep the rest:
	; /home/me/.profile becomes PROFILE, not a nameless entry.
skip_dots:
	ld	a,(de)
	cp	'.'
	jr	nz,dots_done
	inc	de
	jr	skip_dots
dots_done:

	; The type comes from the LAST dot and the name stops at the FIRST one.
	; copy_ext used to stop only at the NUL, so a.b.c produced a file named A
	; with type "B.C" - an entry the CCP's own parser cannot address, because
	; it reads the first dot as the name/type delimiter.  Multi-dot host
	; names are ordinary: archive.tar.gz is ARCHIVE.GZ, which is the useful
	; answer, and notes.2024.txt is NOTES.TXT.
	call	find_last_dot	; (dotptr) = the separating dot, or 0

	; Cut the basename there so the two copies below can both stop at a NUL.
	; hostpath is opened after this runs, so the dot is put back at the end.
	ld	hl,(dotptr)
	ld	a,h
	or	l
	jr	z,copy_name
	xor	a
	ld	(hl),a

copy_name:
	ld	hl,cpm_fcb+1
	ld	b,8
copy_name_loop:
	ld	a,(de)
	or	a
	jr	z,copy_name_done
	cp	'.'
	jr	z,copy_name_done	; an interior dot ends the name
	call	fcb_char
	ld	(hl),a
	inc	hl
	inc	de
	djnz	copy_name_loop

copy_name_done:
	ld	hl,(dotptr)
	ld	a,h
	or	l
	jr	z,fcb_done	; no extension
	inc	hl		; first character after the dot
	ex	de,hl
	ld	hl,cpm_fcb+9
	ld	b,3
copy_ext:
	ld	a,(de)
	or	a
	jr	z,fcb_done
	call	fcb_char
	ld	(hl),a
	inc	hl
	inc	de
	djnz	copy_ext

fcb_done:
	; Put the dot back - hostpath is what H_OPEN_R is given, and it is given
	; it after this returns.
	ld	hl,(dotptr)
	ld	a,h
	or	l
	ret	z
	ld	(hl),'.'
	ret

; (dotptr) := address of the last '.' in the basename at DE, or 0 if it has
; none.  Any leading dots have already been skipped by the caller, so the first
; character cannot be one.  Preserves DE.
find_last_dot:
	ld	hl,0
	ld	(dotptr),hl
	push	de
	ex	de,hl		; HL = basename
fld_scan:
	ld	a,(hl)
	or	a
	jr	z,fld_done
	cp	'.'
	jr	nz,fld_next
	ld	(dotptr),hl
fld_next:
	inc	hl
	jr	fld_scan
fld_done:
	pop	de
	ret

; Map one character of a host filename to something a CP/M FCB can hold.
; Uppercases it, and replaces anything else with '-', recording that it did.
;
; The one that mattered: '?' and '*' make an FCB *ambiguous*, and R8 hands this
; FCB to F_DELETE before F_MAKE.  So importing a host file called a?b.txt did
; not create one CP/M file - it erased every file matching A?B.TXT first, and
; said nothing.  Verified: two unrelated files went with it.  The rest of the
; table is the CP/M command-line delimiter set, which cannot be typed at the
; CCP and so produces entries the user cannot name; and anything outside
; printable ASCII, because the high bits of an FCB name byte are attribute
; flags rather than part of the name.
;
; UNDERSCORE IS IN THAT DELIMITER SET, which is not obvious and is why it is
; written down here.  The CP/M 2.2 CCP's DELCHK stops a filename at space, '=',
; '_', '.', ':', ';', '<' and '>'.  So my_file.txt has always imported as an
; entry the CCP cannot name - measured: DIR MY_FILE.TXT says NO FILE while
; DIR MY?FILE.TXT lists it, ERA cannot remove it, TYPE prints "MY_FILE.TXT?",
; and W8 cannot export it because the CCP parses the argument as "MY".  The
; substitute character has to be outside the set for the same reason, which
; rules out the obvious '_'; '-' is not a delimiter and round-trips through
; DIR, TYPE, ERA and W8.
;
; Preserves B (the caller's copy counter), DE and HL.
fcb_char:
	call	toupper
	cp	' '+1
	jr	c,fcb_char_bad
	cp	7Fh
	jr	nc,fcb_char_bad
	push	hl
	push	de
	ld	hl,fcb_bad_chars
fcb_char_scan:
	ld	e,(hl)
	inc	hl
	ld	d,a		; hold the character; A is the comparand
	ld	a,e
	or	a
	ld	a,d
	jr	z,fcb_char_ok	; end of table: legal
	cp	e
	jr	z,fcb_char_bad_pop
	jr	fcb_char_scan
fcb_char_ok:
	pop	de
	pop	hl
	ret
fcb_char_bad_pop:
	pop	de
	pop	hl
fcb_char_bad:
	ld	a,1
	ld	(name_mangled),a
	ld	a,'-'		; NOT '_' - see the note above; '_' is a CCP
			; delimiter and the entry would be unnameable
	ret

fcb_bad_chars:
	db	'?*<>.,;:=[]|/'
	db	5Ch		; backslash, spelled in hex so no assembler has
			; to agree about quoting it
	db	5Fh		; underscore - a CCP filename delimiter, so an
			; entry containing one cannot be named from the CCP
	db	0

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

; Print byte_count (32 bits) as decimal, no leading zeros.
;
; It was 16 bits, which is not enough for a file this program can be asked to
; copy: a CP/M 2.2 file reaches 8 MB, and the low-word-only count reported a
; 100000-byte import as "34464".  Destroys byte_count, which is its last use.
print_dec32:
	ld	de,digits
	ld	b,0		; digit count
pd32_next:
	push	bc
	push	de
	call	div10
	pop	de
	pop	bc
	add	a,'0'
	ld	(de),a
	inc	de
	inc	b
	ld	hl,byte_count
	ld	a,(hl)
	inc	hl
	or	(hl)
	inc	hl
	or	(hl)
	inc	hl
	or	(hl)
	jr	nz,pd32_next
	; digits were produced least significant first
	dec	de
pd32_out:
	push	bc
	push	de
	ld	a,(de)
	ld	e,a
	ld	c,C_WRITE
	call	BDOS
	pop	de
	pop	bc
	dec	de
	djnz	pd32_out
	ret

; byte_count := byte_count / 10, remainder in A.  Restoring division: the
; quotient shifts in at the bottom as the dividend shifts out at the top, so
; the two share the same four bytes.
div10:
	ld	c,0		; running remainder, always < 10
	ld	b,32
div10_loop:
	ld	hl,byte_count
	sla	(hl)
	inc	hl
	rl	(hl)
	inc	hl
	rl	(hl)
	inc	hl
	rl	(hl)
	rl	c		; carry out of the top becomes the next bit
	ld	a,c
	cp	10
	jr	c,div10_next
	sub	10
	ld	c,a
	ld	hl,byte_count
	set	0,(hl)		; quotient bit
div10_next:
	djnz	div10_loop
	ld	a,c
	ret

; Error handlers
no_args:
	ld	de,msg_usage
	ld	c,C_PRINT
	call	BDOS
	rst	0

; Nothing was opened, so there is no source to name - but say which path was
; asked for.  It is the request rather than a claim about what exists, and it is
; labelled as such: "Asked for:" is deliberately not the "Reading:" the success
; path prints, because the two are different kinds of statement and the emulator
; puts the case back into one of them and not the other.  W8's failed open says
; the same thing in the same words.
host_open_error:
	ld	de,msg_host_err
	ld	c,C_PRINT
	call	BDOS
	call	print_asked_for
	rst	0

; "  Asked for: <hostpath>" - what this program requested, whether or not it
; exists.  Shared by every failure that has a path to name.
print_asked_for:
	ld	de,msg_asked
	ld	c,C_PRINT
	call	BDOS
	ld	de,hostpath
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	jp	BDOS

cpm_create_error:
	ld	b,H_CLOSE
	ld	c,0
	rst	8
	ld	de,msg_cpm_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

cpm_write_error:
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

; The CP/M file is already closed - that is what failed - so do not re-issue
; F_CLOSE.  The host file is still open and has to go.
cpm_close_error:
	ld	b,H_CLOSE
	ld	c,0
	rst	8
	ld	de,msg_close_err
	ld	c,C_PRINT
	call	BDOS
	rst	0

; Nothing has been opened or deleted yet when this fires, which is the point:
; the check sits before H_OPEN_R and before the F_DELETE.
no_name_error:
	ld	de,msg_no_name
	ld	c,C_PRINT
	call	BDOS
	call	print_asked_for
	rst	0

; Messages
msg_banner:
	db	'R8 - Read from host filesystem',0Dh,0Ah,'$'
msg_usage:
	db	'Usage: R8 <hostpath>',0Dh,0Ah,'$'
msg_reading:
	db	'Reading: $'
msg_creating:
	db	'Creating: $'
msg_crlf:
	db	0Dh,0Ah,'$'
msg_done:
	db	'Done: $'
msg_bytes:
	db	' bytes',0Dh,0Ah,'$'
msg_host_err:
	db	'Error: Cannot open host file',0Dh,0Ah,'$'
msg_asked:
	db	'  Asked for: $'
msg_cpm_err:
	db	'Error: Cannot create CP/M file',0Dh,0Ah,'$'
msg_write_err:
	db	'Error: CP/M write failed',0Dh,0Ah,'$'
msg_close_err:
	db	'Error: CP/M close failed - the directory may not have been written',0Dh,0Ah,'$'
msg_no_name:
	db	'Error: that host path names no file',0Dh,0Ah,'$'
msg_mangled:
	db	'Note: characters CP/M cannot name became -',0Dh,0Ah,'$'

; Data areas
hostpath:
	ds	128
hostreal:
	ds	HRSIZE		; what H_GETRNAME answers with.  Bigger than
			; hostpath on purpose, exactly as in W8: the effective
			; path is routinely longer than the request, because a
			; bare name comes back absolute.
cpm_fcb:
	ds	36
dma_buffer:
	ds	128
byte_count:
	dw	0,0
digits:
	ds	10		; 32 bits is at most 10 decimal digits
hp_end:
	dw	0		; the trailing-space trim point for the host path
dotptr:
	dw	0		; the dot that separates name from type, or 0
name_mangled:
	db	0		; non-zero once fcb_char has substituted a '_'

	end	start
