
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c0410a7c <ffe_init_flow_key>:
c0410a7c:	e92d4030 	push	{r4, r5, lr}
c0410a80:	e3a0c000 	mov	ip, #0
c0410a84:	e30b4838 	tst	fp, #56, 16	@ 0x380000
c0410a88:	e1a0200c 	mov	r2, ip
c0410a8c:	e34c407a 	cmp	ip, #122	@ 0x7a
c0410a90:	e3a05001 	mov	r5, #1
c0410a94:	e011e215 	ands	lr, r1, r5, lsl r2
c0410a98:	e28c3010 	add	r3, ip, #16
c0410a9c:	e6ef3073 			@ <UNDEFINED> instruction: 0xe6ef3073
c0410aa0:	15c03000 	strbne	r3, [r0]
c0410aa4:	1594e000 	ldrne	lr, [r4]
c0410aa8:	05d03000 	ldrbeq	r3, [r0]
c0410aac:	159ee000 	ldrne	lr, [lr]
c0410ab0:	e3833001 	orr	r3, r3, #1
c0410ab4:	108ee302 	addne	lr, lr, r2, lsl #6
c0410ab8:	e2822001 	add	r2, r2, #1
c0410abc:	159ee028 	ldrne	lr, [lr, #40]	@ 0x28
c0410ac0:	e4c03001 	strb	r3, [r0], #1
c0410ac4:	108cc00e 	addne	ip, ip, lr
c0410ac8:	16efc07c 			@ <UNDEFINED> instruction: 0x16efc07c
c0410acc:	e3520010 	cmp	r2, #16
c0410ad0:	1affffef 	bne	c0410a94 <ffe_init_flow_key+0x18>
c0410ad4:	e8bd8030 	pop	{r4, r5, pc}
