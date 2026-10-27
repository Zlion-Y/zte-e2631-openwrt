
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c04108cc <ffe_copy_flow_key>:
c04108cc:	e92d47f0 	push	{r4, r5, r6, r7, r8, r9, sl, lr}
c04108d0:	e30b9838 	tst	fp, #56, 16	@ 0x380000
c04108d4:	e1a08000 	mov	r8, r0
c04108d8:	e1a0a001 	mov	sl, r1
c04108dc:	e2414001 	sub	r4, r1, #1
c04108e0:	e281700f 	add	r7, r1, #15
c04108e4:	e1a05000 	mov	r5, r0
c04108e8:	e34c907a 	cmp	ip, #122	@ 0x7a
c04108ec:	e3a06000 	mov	r6, #0
c04108f0:	ea000003 	b	c0410904 <ffe_copy_flow_key+0x38>
c04108f4:	e1540007 	cmp	r4, r7
c04108f8:	e2855001 	add	r5, r5, #1
c04108fc:	e2866040 	add	r6, r6, #64	@ 0x40
c0410900:	0a000012 	beq	c0410950 <ffe_copy_flow_key+0x84>
c0410904:	e5f41001 	ldrb	r1, [r4, #1]!
c0410908:	e3110001 	tst	r1, #1
c041090c:	1afffff8 	bne	c04108f4 <ffe_copy_flow_key+0x28>
c0410910:	e5d53000 	ldrb	r3, [r5]
c0410914:	e3d33001 	bics	r3, r3, #1
c0410918:	0afffff5 	beq	c04108f4 <ffe_copy_flow_key+0x28>
c041091c:	e6ef0073 			@ <UNDEFINED> instruction: 0xe6ef0073
c0410920:	e5c50000 	strb	r0, [r5]
c0410924:	e5992000 	ldr	r2, [r9]
c0410928:	e0880000 	add	r0, r8, r0
c041092c:	e08a1001 	add	r1, sl, r1
c0410930:	e2855001 	add	r5, r5, #1
c0410934:	e5923000 	ldr	r3, [r2]
c0410938:	e0833006 	add	r3, r3, r6
c041093c:	e2866040 	add	r6, r6, #64	@ 0x40
c0410940:	e5932028 	ldr	r2, [r3, #40]	@ 0x28
c0410944:	ebf59e75 	bl	c0178320 <__memcpy>
c0410948:	e1540007 	cmp	r4, r7
c041094c:	1affffec 	bne	c0410904 <ffe_copy_flow_key+0x38>
c0410950:	e3a00000 	mov	r0, #0
c0410954:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}
