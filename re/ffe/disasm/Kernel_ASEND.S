
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c03e5cf0 <Kernel_ASEND>:
c03e5cf0:	e16fcf12 	cmn	pc, r2, lsl pc	@ <UNPREDICTABLE>
c03e5cf4:	e3530000 	cmp	r3, #0
c03e5cf8:	e92d43f0 	push	{r4, r5, r6, r7, r8, r9, lr}
c03e5cfc:	e1a0c2ac 	lsr	ip, ip, #5
c03e5d00:	e24dd024 	sub	sp, sp, #36	@ 0x24
c03e5d04:	03a0c000 	moveq	ip, #0
c03e5d08:	e35c0000 	cmp	ip, #0
c03e5d0c:	e1a05003 	mov	r5, r3
c03e5d10:	e1dd64b0 	ldrh	r6, [sp, #64]	@ 0x40
c03e5d14:	e59de044 	ldr	lr, [sp, #68]	@ 0x44
c03e5d18:	1a00000e 	bne	c03e5d58 <Kernel_ASEND+0x68>
c03e5d1c:	e3530b02 	cmp	r3, #2048	@ 0x800
c03e5d20:	8a000017 	bhi	c03e5d84 <Kernel_ASEND+0x94>
c03e5d24:	e35e0000 	cmp	lr, #0
c03e5d28:	0a000020 	beq	c03e5db0 <Kernel_ASEND+0xc0>
c03e5d2c:	e304354c 	tst	r4, #76, 10	@ 0x13000000
c03e5d30:	e58de000 	str	lr, [sp]
c03e5d34:	e1a0200c 	mov	r2, ip
c03e5d38:	e34c3058 	cmp	ip, #88	@ 0x58
c03e5d3c:	e3a00002 	mov	r0, #2
c03e5d40:	e3a01005 	mov	r1, #5
c03e5d44:	ebfff3df 	bl	c03e2cc8 <ProcKernLog>
c03e5d48:	e30f0c09 	tst	pc, #2304	@ 0x900
c03e5d4c:	e34f0fff 	cmp	pc, #1020	@ 0x3fc
c03e5d50:	e28dd024 	add	sp, sp, #36	@ 0x24
c03e5d54:	e8bd83f0 	pop	{r4, r5, r6, r7, r8, r9, pc}
c03e5d58:	e304353c 	tst	r4, #60, 10	@ 0xf000000
c03e5d5c:	e58d5000 	str	r5, [sp]
c03e5d60:	e34c3058 	cmp	ip, #88	@ 0x58
c03e5d64:	e3a01004 	mov	r1, #4
c03e5d68:	e3a02000 	mov	r2, #0
c03e5d6c:	e3a00002 	mov	r0, #2
c03e5d70:	ebfff3d4 	bl	c03e2cc8 <ProcKernLog>
c03e5d74:	e30f0c10 	tst	pc, #16, 24	@ 0x1000
c03e5d78:	e34f0fff 	cmp	pc, #1020	@ 0x3fc
c03e5d7c:	e28dd024 	add	sp, sp, #36	@ 0x24
c03e5d80:	e8bd83f0 	pop	{r4, r5, r6, r7, r8, r9, pc}
c03e5d84:	e304353c 	tst	r4, #60, 10	@ 0xf000000
c03e5d88:	e58d5000 	str	r5, [sp]
c03e5d8c:	e1a0200c 	mov	r2, ip
c03e5d90:	e34c3058 	cmp	ip, #88	@ 0x58
c03e5d94:	e3a01004 	mov	r1, #4
c03e5d98:	e3a00002 	mov	r0, #2
c03e5d9c:	ebfff3c9 	bl	c03e2cc8 <ProcKernLog>
c03e5da0:	e30f0c14 	tst	pc, #20, 24	@ 0x1400
c03e5da4:	e34f0fff 	cmp	pc, #1020	@ 0x3fc
c03e5da8:	e28dd024 	add	sp, sp, #36	@ 0x24
c03e5dac:	e8bd83f0 	pop	{r4, r5, r6, r7, r8, r9, pc}
c03e5db0:	e30f3ed8 	tst	pc, #216, 28	@ 0xd80
c03e5db4:	e1a07002 	mov	r7, r2
c03e5db8:	e34c3077 	cmp	ip, #119	@ 0x77
c03e5dbc:	e5932000 	ldr	r2, [r3]
c03e5dc0:	e3520000 	cmp	r2, #0
c03e5dc4:	0a000024 	beq	c03e5e5c <Kernel_ASEND+0x16c>
c03e5dc8:	e59231d4 	ldr	r3, [r2, #468]	@ 0x1d4
c03e5dcc:	e1a09000 	mov	r9, r0
c03e5dd0:	e1a08001 	mov	r8, r1
c03e5dd4:	e3530000 	cmp	r3, #0
c03e5dd8:	da00000e 	ble	c03e5e18 <Kernel_ASEND+0x128>
c03e5ddc:	e59211c8 	ldr	r1, [r2, #456]	@ 0x1c8
c03e5de0:	e2823f72 	add	r3, r2, #456	@ 0x1c8
c03e5de4:	e1530001 	cmp	r3, r1
c03e5de8:	0a000007 	beq	c03e5e0c <Kernel_ASEND+0x11c>
c03e5dec:	e58d9040 	str	r9, [sp, #64]	@ 0x40
c03e5df0:	e1a00008 	mov	r0, r8
c03e5df4:	e1a01007 	mov	r1, r7
c03e5df8:	e1a02005 	mov	r2, r5
c03e5dfc:	e1a03006 	mov	r3, r6
c03e5e00:	e28dd024 	add	sp, sp, #36	@ 0x24
c03e5e04:	e8bd43f0 	pop	{r4, r5, r6, r7, r8, r9, lr}
c03e5e08:	eaffff3c 	b	c03e5b00 <KernOssInner_CacheMsg>
c03e5e0c:	e59221cc 	ldr	r2, [r2, #460]	@ 0x1cc
c03e5e10:	e1530002 	cmp	r3, r2
c03e5e14:	1afffff4 	bne	c03e5dec <Kernel_ASEND+0xfc>
c03e5e18:	e1a03006 	mov	r3, r6
c03e5e1c:	e58d9000 	str	r9, [sp]
c03e5e20:	e1a00008 	mov	r0, r8
c03e5e24:	e1a01007 	mov	r1, r7
c03e5e28:	e1a02005 	mov	r2, r5
c03e5e2c:	ebfffe23 	bl	c03e56c0 <KernOssInner_ASEND>
c03e5e30:	e3e03e3f 	mvn	r3, #1008	@ 0x3f0
c03e5e34:	e1500003 	cmp	r0, r3
c03e5e38:	e1a04000 	mov	r4, r0
c03e5e3c:	1affffc3 	bne	c03e5d50 <Kernel_ASEND+0x60>
c03e5e40:	ebfbe19f 	bl	c02de4c4 <net_ratelimit>
c03e5e44:	e3500000 	cmp	r0, #0
c03e5e48:	1a00000a 	bne	c03e5e78 <Kernel_ASEND+0x188>
c03e5e4c:	e3560000 	cmp	r6, #0
c03e5e50:	13e00e3f 	mvnne	r0, #1008	@ 0x3f0
c03e5e54:	1affffbd 	bne	c03e5d50 <Kernel_ASEND+0x60>
c03e5e58:	eaffffe3 	b	c03e5dec <Kernel_ASEND+0xfc>
c03e5e5c:	e3043568 	tst	r4, #104, 10	@ 0x1a000000
c03e5e60:	e3a00002 	mov	r0, #2
c03e5e64:	e34c3058 	cmp	ip, #88	@ 0x58
c03e5e68:	e3a01004 	mov	r1, #4
c03e5e6c:	ebfff395 	bl	c03e2cc8 <ProcKernLog>
c03e5e70:	e3e00ffe 	mvn	r0, #1016	@ 0x3f8
c03e5e74:	eaffffb5 	b	c03e5d50 <Kernel_ASEND+0x60>
c03e5e78:	e3a0c000 	mov	ip, #0
c03e5e7c:	e3043580 	tst	r4, #128, 10	@ 0x20000000
c03e5e80:	e58d9004 	str	r9, [sp, #4]
c03e5e84:	e1a0200c 	mov	r2, ip
c03e5e88:	e58d8008 	str	r8, [sp, #8]
c03e5e8c:	e34c3058 	cmp	ip, #88	@ 0x58
c03e5e90:	e58d700c 	str	r7, [sp, #12]
c03e5e94:	e3a01005 	mov	r1, #5
c03e5e98:	e58d5010 	str	r5, [sp, #16]
c03e5e9c:	e3a00002 	mov	r0, #2
c03e5ea0:	e58d6014 	str	r6, [sp, #20]
c03e5ea4:	e58d4018 	str	r4, [sp, #24]
c03e5ea8:	e58dc000 	str	ip, [sp]
c03e5eac:	ebfff385 	bl	c03e2cc8 <ProcKernLog>
c03e5eb0:	eaffffe5 	b	c03e5e4c <Kernel_ASEND+0x15c>

c03e5eb4 <msgcnt_read>:
c03e5eb4:	e92d43f0 	push	{r4, r5, r6, r7, r8, r9, lr}
c03e5eb8:	e24dd084 	sub	sp, sp, #132	@ 0x84
c03e5ebc:	e1a05003 	mov	r5, r3
c03e5ec0:	e1a08001 	mov	r8, r1
c03e5ec4:	e1a09002 	mov	r9, r2
c03e5ec8:	e1a0000d 	mov	r0, sp
c03e5ecc:	e3a01000 	mov	r1, #0
c03e5ed0:	e3a02080 	mov	r2, #128	@ 0x80
c03e5ed4:	ebf64ab1 	bl	c01789a0 <__memset>
c03e5ed8:	e1c560d0 			@ <UNDEFINED> instruction: 0xe1c560d0
c03e5edc:	e30fced8 	tst	pc, #216, 28	@ 0xd80
c03e5ee0:	e1963007 	orrs	r3, r6, r7
c03e5ee4:	e34cc077 	cmp	ip, #119	@ 0x77
c03e5ee8:	e59c3000 	ldr	r3, [ip]
c03e5eec:	13a00000 	movne	r0, #0
