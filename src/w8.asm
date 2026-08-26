; W8.COM - Write CP/M file to host filesystem (RomWBW/HBIOS version)
;
; Usage: W8 <cpmname> [hostpath]
;   Exports a CP/M file to the host.  With no hostpath the name is the CP/M
;   name lowercased, in the emulator's working directory - what this did
;   before hostpath existed.  With a hostpath the file goes there, the way R8
;   already takes one.  The hostpath is the whole rest of the command line, so
;   a directory with a space in its name works: on a desktop host that is not
;   an exotic case, it is /Users/me/My Documents and C:\Program Files.
;
;   CP/M's CCP uppercases the whole command line, so the path arrives here in
;   upper case and the emulator is what puts the case back: it resolves the
;   directory components case-insensitively and lowercases the final name.
;   Which means the path typed is NOT the path written, so this does not print
;   it - it asks the emulator where the file actually went (H_GETNAME) once the
;   file is open, and prints that.  On the browser and mobile front ends the
;   answer is not a path at all: those cannot honour a directory, and the file
;   arrives as a download or in the app's own Exports folder under the last
;   component of whatever was typed.  Printing what the user typed named a file
;   that did not exist on three of the five front ends.
;
;   An emulator built before H_GETNAME existed answers "no such function", and
;   this falls back to printing the requested path - a new W8.COM has to keep
;   working on an already-released front end.
;
; Uses HBIOS extension functions for host file access
;
; Build:  um80 -o w8.rel w8.asm && ul80 -o w8.com w8.rel
;   There is deliberately no ORG here.  M80 assembles this as one relocatable
;   code segment and L80 bases a .COM at 0100h by itself; an `org 0100h` in the
;   source is applied ON TOP of that base, which put the code at 0200h behind
;   256 zero bytes.  That built a working program only because CP/M loads the
;   whole file at 0100h and the Z80 slides through 256 NOPs into it - 256 bytes
;   of file carried for nothing, and a binary that could not be compared
;   against the bare .COM layout r8.com was already built in.

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
H_GETNAME equ	0E8h	; Where the open write file really lands (C=bufsize,
			; DE=buffer).  A=0 and the buffer holds a string; A<>0
			; on an emulator that does not implement it.
H_CAPS	equ	0E9h	; What the emulator's host-file support guarantees.
			; A=0 and E=capability bits; A<>0 on an emulator that
			; predates the call.  No inputs and no state, so unlike
			; H_GETNAME it can be asked before anything is open -
			; which is what makes it usable as a safety interlock.
CAP_SAFE_PATH equ 01h	; a host path from the guest cannot escape the place
			; the front end writes to

EOFCHR	equ	1Ah	; CP/M end-of-text marker, and the padding R8 writes into
			; the last record of an imported file
HRSIZE	equ	255	; size of hostreal, the buffer H_GETNAME fills.  One symbol
			; for the ds and for the C the call is given: two separate
			; literals could drift apart, and the emulator writes
			; exactly as many bytes as C says - straight into cpm_fcb
			; and the DMA buffer behind it if C were the larger.

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

	; Host path: the rest of the command line when one was given, otherwise
	; the CP/M name lowercased (the original behaviour).
	ld	a,(have_hostarg)
	or	a
	call	z,fcb_to_hostpath

	; A host path is only safe to send to an emulator that says it handles
	; one safely.  See check_host_path_safe - this is the interlock, and it
	; runs before F_OPEN and before H_OPEN_W so a refusal opens nothing,
	; creates nothing and truncates nothing.
	call	check_host_path_safe

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

	; Now that the file exists, ask where it went and say so.  This is the
	; whole reason the message moved below the open: before the open there
	; is nothing to ask about.
	call	print_destination

	; Set DMA to our buffer
	ld	de,dma_buffer
	ld	c,F_DMA
	call	BDOS

	; Initialize counters
	ld	hl,0
	ld	(byte_count),hl
	ld	(byte_count+2),hl
	ld	(zpend),hl

	; Read loop
read_loop:
	; Read record from CP/M file.
	;
	; A=0 is a record; A=1 is end of file; anything else is a real error.
	; This used to treat every nonzero code as end of file, which is exact
	; under CP/M 2.2 - 1 is the only code it returns - and wrong under the
	; systems on the disks this ships with.  ZSDOS and CP/M 3 report a read
	; failure with codes of their own (9, 10, 0FFh), and taking those for
	; end of file made a failed export look like a complete one: W8 closed
	; the host file and printed "Done: <n> bytes" for a file that stops
	; wherever the disk stopped answering.  Silent truncation reported as
	; success is the same defect the 1Ah handling below exists to undo.
	ld	de,cpm_fcb
	ld	c,F_READ
	call	BDOS
	or	a
	jr	z,got_record
	cp	1
	jr	z,read_done
	jp	cpm_read_error
got_record:

	; Copy the record to the host, deferring EOF characters - see the note
	; on zpend below for why they are not simply written or simply dropped.
	ld	hl,dma_buffer
	ld	b,128

write_loop:
	ld	a,(hl)
	cp	EOFCHR
	jr	z,defer_eof

	; A real byte.  Anything deferred was interior padding after all, so it
	; is part of the file: write it out before this byte, in order.
	call	flush_deferred
	call	put_byte
	jr	next_byte

defer_eof:
	; Count it, do not write it.  If the file ends here the whole run is
	; R8's record padding and must not reach the host; if it does not, the
	; run is data and flush_deferred above writes it.
	push	hl
	ld	hl,(zpend)
	inc	hl
	ld	(zpend),hl
	ld	a,h
	and	l
	inc	a		; A = 0 only when zpend has reached 0FFFFh
	pop	hl
	jr	nz,next_byte
	; 65535 deferred.  The counter is 16 bits, so rather than wrap and lose
	; 64K of file, write the run out now.  A trailing run this long would
	; then leave a multiple of 65535 EOF characters in the host file - a
	; bounded, documented wrong answer instead of an unbounded one, and it
	; needs a CP/M file holding 512 consecutive empty records to reach.
	call	flush_deferred

next_byte:
	inc	hl
	djnz	write_loop
	jr	read_loop

read_done:
	; Whatever is still deferred was the padding in the final record.  Drop
	; it: that is what makes a text file R8 imported come back out byte for
	; byte, and it is the only part of the old "stop at the first EOF
	; character" rule worth keeping.
	;
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
	call	print_dec32

	ld	de,msg_bytes
	ld	c,C_PRINT
	call	BDOS

	rst	0

; Write A to the host file and add one to byte_count.  Preserves BC, HL.
; Does not return on a write error - jumps to the handler, which warm-boots.
put_byte:
	push	hl
	push	bc
	ld	e,a
	ld	b,H_WRITE
	rst	8
	or	a
	jr	nz,pb_error
	ld	hl,(byte_count)
	inc	hl
	ld	(byte_count),hl
	ld	a,h
	or	l
	jr	nz,pb_done
	ld	hl,(byte_count+2)
	inc	hl
	ld	(byte_count+2),hl
pb_done:
	pop	bc
	pop	hl
	ret
pb_error:
	pop	bc
	pop	hl
	jp	host_write_error

; Write out the deferred run of EOF characters and clear it.
;
; Preserves A as well as BC and HL, and A is the one that matters: write_loop
; holds the byte it is about to export in A across this call.  Every exit here
; would otherwise leave A=0 - both the "nothing deferred" test and the loop's
; own countdown end on `ld a,b / or c` - so the guest's file arrived at the host
; as the right number of zero bytes, with only the EOF characters this routine
; writes itself surviving.  Caught by an adversarial read of the assembly after
; a live test that checked the exported length and not its contents.
flush_deferred:
	push	af
	push	hl
	push	bc
	ld	bc,(zpend)
	ld	a,b
	or	c
	jr	z,fd_done
fd_loop:
	ld	a,EOFCHR
	call	put_byte	; preserves BC, so the count survives the call
	dec	bc
	ld	a,b
	or	c
	jr	nz,fd_loop
fd_done:
	ld	hl,0
	ld	(zpend),hl
	pop	bc
	pop	hl
	pop	af
	ret

; Refuse to hand a host path to an emulator that does not guarantee it is safe.
;
; This exists because a disk image and the emulator that runs it travel
; separately.  The front ends fetch their images from a pinned release tag, so
; a user who has not updated keeps getting the old images - but nothing stops
; an image being copied in by hand, and this W8.COM then runs on whatever
; emulator happens to be there.  On an iOS build before build 52 that was
; fatal: the front end joined the guest's string to its Exports folder and
; called removeItem on the result, so "W8 ANYFILE.TXT .." deleted the user's
; entire Documents folder - every disk image they had - and reported success.
;
; So the guest asks first.  H_CAPS is a call no older emulator has, and it takes
; no arguments and touches no state, so it can be asked before anything is
; opened.  A<>0 means the emulator predates it; CAP_SAFE_PATH clear means it has
; the call but does not make the guarantee.  Either way, refuse.
;
; Only when a path was actually typed.  With no hostpath the name comes from the
; FCB, and the CCP cannot put a '.' in an FCB name field - measured: "W8 .."
; prints the usage message - so it can never be ".." or contain "../".  That
; matters more than it looks: it means a refreshed disk image still does
; ordinary "W8 FOO.TXT" exports on an old emulator.  Only the dangerous form is
; withheld, so updating the images does not break the common case.
;
; Fails closed.  The probe cannot tell a safe old emulator (the CLI, which was
; never vulnerable) from a dangerous one, and the cost of guessing wrong in one
; direction is an error message while the other is the user's disk library.
check_host_path_safe:
	ld	a,(have_hostarg)
	or	a
	ret	z		; no path given - nothing to withhold
	ld	b,H_CAPS
	rst	8
	or	a
	jp	nz,old_host_error	; no such call: predates v1.36
	ld	a,e
	and	CAP_SAFE_PATH
	jp	z,old_host_error	; has the call, makes no guarantee
	ret

; Print where the file is actually being written.
;
; H_GETNAME answers with the destination after the emulator has done whatever
; its platform does to the requested path - resolved a shouted directory,
; lowercased the name, reduced a path to a download name, redirected it into a
; sandbox.  A failure here is not an error: an emulator that predates the call
; returns "no such function", and the requested path is then the best answer
; available.
print_destination:
	ld	de,msg_tohost
	ld	c,C_PRINT
	call	BDOS
	ld	c,HRSIZE	; buffer size including the terminator
	ld	de,hostreal
	ld	b,H_GETNAME
	rst	8
	or	a
	ld	de,hostreal
	jr	z,pd_show
	ld	de,hostpath	; older emulator: say what was asked for
pd_show:
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	jp	BDOS

; Parse the optional host path out of the command tail into hostpath.
;
; The tail is "<cpmname> <hostpath>" including the CCP's leading space, so this
; skips spaces, steps over the first token, skips spaces again, and takes ALL
; of the rest - not up to the next space.  A host path is the last thing on the
; line and a directory name may contain spaces, which on a desktop host is
; ordinary rather than exotic.  Trailing spaces are trimmed, so a line the user
; ended with a space does not create a file whose name ends in one.
;
; Anything short of a second token - empty tail, all spaces, one token only -
; leaves have_hostarg zero and the caller falls back to fcb_to_hostpath.
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

pha_copy:			; copy the rest of the tail verbatim
	ld	de,hostpath
	ld	(hp_end),de	; one past the last non-space stored so far
	ld	c,127		; the tail cannot exceed 127, so this cap is only
			; a backstop - it can no longer be reached by any input
pha_loop:
	ld	a,(hl)
	ld	(de),a
	inc	hl
	inc	de
	cp	' '
	jr	z,pha_nospc
	ld	(hp_end),de	; remember where the name really ends
pha_nospc:
	dec	c
	jr	z,pha_end	; unreachable for a legal tail; here so a corrupt
			; length byte cannot run off the buffer
	djnz	pha_loop
pha_end:
	; Terminate after the last non-space rather than after the last byte
	; copied, which trims any trailing spaces without a second pass.
	ld	de,(hp_end)
	xor	a
	ld	(de),a
	ld	a,(hostpath)
	or	a
	ret	z		; nothing but spaces: leave have_hostarg clear
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

; Print byte_count (32 bits) as decimal, no leading zeros.
;
; It was 16 bits, which is not enough for a file this program can be asked to
; copy: a CP/M 2.2 file reaches 8 MB, and the low-word-only count reported a
; 100000-byte export as "34464".  Destroys byte_count, which is its last use.
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
;
; Nothing has been opened when this fires - the interlock runs before F_OPEN -
; so there is nothing to close and nothing has been created.
old_host_error:
	ld	de,msg_old_host
	ld	c,C_PRINT
	call	BDOS
	rst	0

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

; The host file was never created, so there is nothing to close - but say which
; path failed, because with no hostpath given it is one this program invented.
;
; Labelled "Asked for:", not printed bare after the message.  A bare path in the
; same shape as the "To host:" line reads as a claim about a destination, and it
; is not one: nothing was created, and the case differs from what "To host:"
; would have said - the emulator lowercases the name it creates and resolves the
; directory through whatever case really exists, and none of that has happened
; here.  The label is what gives the difference a reason the user can see.  With
; no hostpath given the string is one this program built from the FCB and is
; already lowercase, so without a label the same failure printed two different
; cases depending on which form of the command was used.  R8's failed open says
; the same thing in the same words.
host_open_error:
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_host_err
	ld	c,C_PRINT
	call	BDOS
	call	print_asked_for
	rst	0

; "  Asked for: <hostpath>" - what this program requested, whether or not it
; can exist.
print_asked_for:
	ld	de,msg_asked
	ld	c,C_PRINT
	call	BDOS
	ld	de,hostpath
	call	print_string
	ld	de,msg_crlf
	ld	c,C_PRINT
	jp	BDOS

; A CP/M read failed part way through.  The host file is open and holds a
; prefix of the export, so close it - the bytes already written are on the host
; either way - and say plainly that it is short.  Do NOT print "Done", which is
; what this used to do.
cpm_read_error:
	ld	b,H_CLOSE
	ld	c,1
	rst	8
	ld	de,cpm_fcb
	ld	c,F_CLOSE
	call	BDOS
	ld	de,msg_cpm_read
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
msg_asked:
	db	'  Asked for: $'
msg_cpm_read:
	db	'Error: CP/M read failed - the host file is short',0Dh,0Ah,'$'
msg_host_write:
	db	'Error: Host write failed',0Dh,0Ah,'$'
msg_host_close:
	db	'Host file close failed - file may be truncated',0Dh,0Ah,'$'
msg_old_host:
	db	'This emulator is too old to be given a host path safely.',0Dh,0Ah
	db	'Nothing was written.  Update the emulator, or use W8 with',0Dh,0Ah
	db	'no path to export into its own folder.',0Dh,0Ah,'$'

; Data areas
hostpath:
	ds	128		; the whole CP/M tail can be 127 bytes + NUL.  Was 64,
			; which was enough for the 8.3 name fcb_to_hostpath
			; builds but silently cut a typed path at 63 - and
			; still reported success, so the export landed under a
			; shortened name.  R8's buffer is 128 for the same
			; reason; the two accept the same paths.
hostreal:
	ds	HRSIZE		; what H_GETNAME answers with.  Bigger than
			; hostpath on purpose: the destination is routinely
			; longer than the request, because a bare name comes
			; back as an absolute path.
cpm_fcb:
	ds	36
dma_buffer:
	ds	128
byte_count:
	dw	0,0
zpend:
	dw	0		; EOF characters seen but not yet written.  They
			; are written only if more file follows, and dropped at
			; end of file, which is what tells R8's record padding
			; apart from a 1Ah that is simply a byte of a binary.
			; Stopping at the first one - what this used to do -
			; truncated every binary silently: W8.COM itself came
			; out 368 bytes of 1408, reported as "Done: 368 bytes".
digits:
	ds	10		; 32 bits is at most 10 decimal digits
hp_end:
	dw	0		; parse_host_arg's trailing-space trim point
have_hostarg:
	db	0		; non-zero once parse_host_arg found a second token

	end	start
