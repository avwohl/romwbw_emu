; gdb_prog.asm - a small CP/M 2.2 program for tests/gdb_stub_test.py.
;
; The test starts it from a disk image and finds it by its first bytes, so it
; has to be a program that the guest loads at 0100H.  It prints one line (BDOS
; 9), calls OUTER, which calls INNER, prints a second line and warm-boots.  The
; test sets a breakpoint at INNER and expects the shadow stack to show
; INNER, OUTER and the main routine.
;
; No ORG: ul80 bases the code at 0100H itself.  gdb_stub_test.py checks the
; offsets that it uses against the bytes of the linked file.
	.z80
start:	ld	c,9
	ld	de,msg
	call	5
	call	outer		; 0108H
	ld	c,9
	ld	de,done
	call	5
	jp	0
outer:	call	inner		; 0116H
	ret
inner:	ld	a,5		; 011AH
	inc	a
	ret
msg:	db	'GDBTEST1',13,10,'$'
done:	db	'GDBDONE',13,10,'$'
	end
