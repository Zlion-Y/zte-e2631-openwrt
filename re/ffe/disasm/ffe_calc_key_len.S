
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c04119f4 <ffe_calc_key_len>:
c04119f4:	e30b2838 	tst	fp, #56, 16	@ 0x380000
c04119f8:	e2503000 	subs	r3, r0, #0
c04119fc:	e34c207a 	cmp	ip, #122	@ 0x7a
c0411a00:	e5922000 	ldr	r2, [r2]
c0411a04:	e592c000 	ldr	ip, [r2]
c0411a08:	0a00000e 	beq	c0411a48 <ffe_calc_key_len+0x54>
c0411a0c:	e3a01000 	mov	r1, #0
c0411a10:	e3a00010 	mov	r0, #16
c0411a14:	e3130001 	tst	r3, #1
c0411a18:	e2811001 	add	r1, r1, #1
c0411a1c:	e28cc040 	add	ip, ip, #64	@ 0x40
c0411a20:	151c2018 	ldrne	r2, [ip, #-24]	@ 0xffffffe8
c0411a24:	10800002 	addne	r0, r0, r2
c0411a28:	e1b030a3 	lsrs	r3, r3, #1
c0411a2c:	03a02001 	moveq	r2, #1
c0411a30:	13a02000 	movne	r2, #0
c0411a34:	e3510010 	cmp	r1, #16
c0411a38:	03822001 	orreq	r2, r2, #1
c0411a3c:	e3520000 	cmp	r2, #0
c0411a40:	0afffff3 	beq	c0411a14 <ffe_calc_key_len+0x20>
c0411a44:	e12fff1e 	msr	SP_hyp, lr, lsl pc
c0411a48:	e3a00010 	mov	r0, #16
c0411a4c:	e12fff1e 	msr	SP_hyp, lr, lsl pc
