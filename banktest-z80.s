	.code
	push af
	call foo1
	pop af
	ret

	.data
	.word foo2

	.overlay1
foo1:
	push af
	call foo2
	pop af

	.overlay2
foo2:	ret

	.common
	.export stub
	.export stub_end

stub:
	.ds 512
stub_end:

	.common
	.export __bank_0_1
	.export __bank_1_2

__bank_0_1:
	ld a,1
	out (0xFF),a
	pop hl
	ld e,(hl)
	inc hl
	ld d,(hl)
	inc hl
	push hl
	ex de,hl
	call callhl
from0:
	xor a
	out (0xFF),a
	ret
__bank_1_2:
	ld a,2
	out (0xFF),a
	pop hl
	ld e,(hl)
	inc hl
	ld d,(hl)
	inc hl
	push hl
	ex de,hl
	call callhl
from1:
	ld a,1
	out (0xFF),a
	ret
callhl:	jp (hl)

	.export __bank_C_2
	.export __stub_2

__bank_C_2:
	pop	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	push	hl
__stub_2:		; Calling bank 2 from unknown place
	in a,(0xFF)
	ld c,a		; caller saved
	ld a,2
	out (0xFF),a
	ex de,hl
	call callhl
	ld a,c
	out (0xFF),a
	ret

