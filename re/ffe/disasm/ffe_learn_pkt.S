
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c0410958 <ffe_learn_pkt>:
c0410958:	e92d47f0 	push	{r4, r5, r6, r7, r8, r9, sl, lr}
c041095c:	e30b8838 	tst	fp, #56, 16	@ 0x380000
c0410960:	e34c807a 	cmp	ip, #122	@ 0x7a
c0410964:	e1a0300d 	mov	r3, sp
c0410968:	e598c000 	ldr	ip, [r8]
c041096c:	e3c33d7f 	bic	r3, r3, #8128	@ 0x1fc0
c0410970:	e3c3303f 	bic	r3, r3, #63	@ 0x3f
c0410974:	e591e028 	ldr	lr, [r1, #40]	@ 0x28
c0410978:	e5933010 	ldr	r3, [r3, #16]
c041097c:	e59c2150 	ldr	r2, [ip, #336]	@ 0x150
c0410980:	e59c6000 	ldr	r6, [ip]
c0410984:	e3822301 	orr	r2, r2, #67108864	@ 0x4000000
c0410988:	e08c3103 	add	r3, ip, r3, lsl #2
c041098c:	e112000e 	tst	r2, lr
c0410990:	e593509c 	ldr	r5, [r3, #156]	@ 0x9c
c0410994:	0a000036 	beq	c0410a74 <ffe_learn_pkt+0x11c>
c0410998:	e5957008 	ldr	r7, [r5, #8]
c041099c:	e3002101 	tst	r0, #1073741824	@ 0x40000000
c04109a0:	e595400c 	ldr	r4, [r5, #12]
c04109a4:	e1a03002 	mov	r3, r2
c04109a8:	e595a000 	ldr	sl, [r5]
c04109ac:	e300e101 	tst	r0, #1073741824	@ 0x40000000
c04109b0:	e5959004 	ldr	r9, [r5, #4]
c04109b4:	e7dfe81e 			@ <UNDEFINED> instruction: 0xe7dfe81e
c04109b8:	e300c101 	tst	r0, #1073741824	@ 0x40000000
c04109bc:	e7df2812 			@ <UNDEFINED> instruction: 0xe7df2812
c04109c0:	e7dfc81c 			@ <UNDEFINED> instruction: 0xe7dfc81c
c04109c4:	e7df3813 			@ <UNDEFINED> instruction: 0xe7df3813
c04109c8:	e1843003 	orr	r3, r4, r3
c04109cc:	e1872002 	orr	r2, r7, r2
c04109d0:	e18ae00e 	orr	lr, sl, lr
c04109d4:	e189c00c 	orr	ip, r9, ip
c04109d8:	e585e000 	str	lr, [r5]
c04109dc:	e585c004 	str	ip, [r5, #4]
c04109e0:	e5852008 	str	r2, [r5, #8]
c04109e4:	e585300c 	str	r3, [r5, #12]
c04109e8:	e5903008 	ldr	r3, [r0, #8]
c04109ec:	e3530000 	cmp	r3, #0
c04109f0:	c1a04000 	movgt	r4, r0
c04109f4:	c1a07001 	movgt	r7, r1
c04109f8:	c1a00006 	movgt	r0, r6
c04109fc:	ca000007 	bgt	c0410a20 <ffe_learn_pkt+0xc8>
c0410a00:	ea00001b 	b	c0410a74 <ffe_learn_pkt+0x11c>
c0410a04:	e1d400b0 	ldrh	r0, [r4]
c0410a08:	e350000f 	cmp	r0, #15
c0410a0c:	e0860300 	add	r0, r6, r0, lsl #6
c0410a10:	8a000017 	bhi	c0410a74 <ffe_learn_pkt+0x11c>
c0410a14:	e5943008 	ldr	r3, [r4, #8]
c0410a18:	e3530000 	cmp	r3, #0
c0410a1c:	da000014 	ble	c0410a74 <ffe_learn_pkt+0x11c>
c0410a20:	e5903008 	ldr	r3, [r0, #8]
c0410a24:	e1a01004 	mov	r1, r4
c0410a28:	e1a02005 	mov	r2, r5
c0410a2c:	e12fff33 	msr	SP_hyp, r3, lsr pc
c0410a30:	e350000b 	cmp	r0, #11
c0410a34:	8afffff2 	bhi	c0410a04 <ffe_learn_pkt+0xac>
c0410a38:	e5983000 	ldr	r3, [r8]
c0410a3c:	e5976034 	ldr	r6, [r7, #52]	@ 0x34
c0410a40:	e5933004 	ldr	r3, [r3, #4]
c0410a44:	e0833380 	add	r3, r3, r0, lsl #7
c0410a48:	e1560003 	cmp	r6, r3
c0410a4c:	1a000008 	bne	c0410a74 <ffe_learn_pkt+0x11c>
c0410a50:	e1a01005 	mov	r1, r5
c0410a54:	e5970014 	ldr	r0, [r7, #20]
c0410a58:	ebffff9b 	bl	c04108cc <ffe_copy_flow_key>
c0410a5c:	e1a01004 	mov	r1, r4
c0410a60:	e1a02007 	mov	r2, r7
c0410a64:	e5963040 	ldr	r3, [r6, #64]	@ 0x40
c0410a68:	e1a00006 	mov	r0, r6
c0410a6c:	e8bd47f0 	pop	{r4, r5, r6, r7, r8, r9, sl, lr}
c0410a70:	e12fff13 	msr	SP_hyp, r3, lsl pc
c0410a74:	e3e00000 	mvn	r0, #0
c0410a78:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}
