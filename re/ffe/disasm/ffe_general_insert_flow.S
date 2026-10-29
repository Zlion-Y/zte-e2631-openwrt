
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c0410d74 <ffe_general_insert_flow>:
c0410d74:	e92d41f0 	push	{r4, r5, r6, r7, r8, lr}
c0410d78:	e30b6838 	tst	fp, #56, 16	@ 0x380000
c0410d7c:	e34c607a 	cmp	ip, #122	@ 0x7a
c0410d80:	e1a05000 	mov	r5, r0
c0410d84:	e5960000 	ldr	r0, [r6]
c0410d88:	e1a04002 	mov	r4, r2
c0410d8c:	e1a07003 	mov	r7, r3
c0410d90:	e2828038 	add	r8, r2, #56	@ 0x38
c0410d94:	e28000b4 	add	r0, r0, #180	@ 0xb4
c0410d98:	eb008df0 	bl	c0434560 <_raw_spin_lock_bh>
c0410d9c:	e5943038 	ldr	r3, [r4, #56]	@ 0x38
c0410da0:	e1530008 	cmp	r3, r8
c0410da4:	0a000040 	beq	c0410eac <ffe_general_insert_flow+0x138>
c0410da8:	e30a30a0 	tst	sl, #160	@ 0xa0
c0410dac:	e5962000 	ldr	r2, [r6]
c0410db0:	e34c3061 	cmp	ip, #97	@ 0x61
c0410db4:	e584701c 	str	r7, [r4, #28]
c0410db8:	e5933000 	ldr	r3, [r3]
c0410dbc:	e5941024 	ldr	r1, [r4, #36]	@ 0x24
c0410dc0:	e5843018 	str	r3, [r4, #24]
c0410dc4:	e5920044 	ldr	r0, [r2, #68]	@ 0x44
c0410dc8:	eb000986 	bl	c04133e8 <ffe_sharedata_store>
c0410dcc:	e5963000 	ldr	r3, [r6]
c0410dd0:	e5941068 	ldr	r1, [r4, #104]	@ 0x68
c0410dd4:	e5840024 	str	r0, [r4, #36]	@ 0x24
c0410dd8:	e5930048 	ldr	r0, [r3, #72]	@ 0x48
c0410ddc:	eb000981 	bl	c04133e8 <ffe_sharedata_store>
c0410de0:	e594303c 	ldr	r3, [r4, #60]	@ 0x3c
c0410de4:	e5942038 	ldr	r2, [r4, #56]	@ 0x38
c0410de8:	e1a01004 	mov	r1, r4
c0410dec:	e5840068 	str	r0, [r4, #104]	@ 0x68
c0410df0:	e5823004 	str	r3, [r2, #4]
c0410df4:	e5832000 	str	r2, [r3]
c0410df8:	e5963000 	ldr	r3, [r6]
c0410dfc:	e5848038 	str	r8, [r4, #56]	@ 0x38
c0410e00:	e584803c 	str	r8, [r4, #60]	@ 0x3c
c0410e04:	e5930008 	ldr	r0, [r3, #8]
c0410e08:	eb0027f3 	bl	c041addc <ffe_npu_create_flow>
c0410e0c:	e5953020 	ldr	r3, [r5, #32]
c0410e10:	e595200c 	ldr	r2, [r5, #12]
c0410e14:	e2433001 	sub	r3, r3, #1
c0410e18:	e0033007 	and	r3, r3, r7
c0410e1c:	e7921103 	ldr	r1, [r2, r3, lsl #2]
c0410e20:	e0820103 	add	r0, r2, r3, lsl #2
c0410e24:	e5840004 	str	r0, [r4, #4]
c0410e28:	e5841000 	str	r1, [r4]
c0410e2c:	f57ff05b 			@ <UNDEFINED> instruction: 0xf57ff05b
c0410e30:	e3510000 	cmp	r1, #0
c0410e34:	e7824103 	str	r4, [r2, r3, lsl #2]
c0410e38:	15814004 	strne	r4, [r1, #4]
c0410e3c:	f595f000 			@ <UNDEFINED> instruction: 0xf595f000
c0410e40:	e1953f9f 			@ <UNDEFINED> instruction: 0xe1953f9f
c0410e44:	e2833001 	add	r3, r3, #1
c0410e48:	e1852f93 			@ <UNDEFINED> instruction: 0xe1852f93
c0410e4c:	e3320000 	teq	r2, #0
c0410e50:	1afffffa 	bne	c0410e40 <ffe_general_insert_flow+0xcc>
c0410e54:	e5963000 	ldr	r3, [r6]
c0410e58:	e2832f4e 	add	r2, r3, #312	@ 0x138
c0410e5c:	f592f000 			@ <UNDEFINED> instruction: 0xf592f000
c0410e60:	e1921f9f 			@ <UNDEFINED> instruction: 0xe1921f9f
c0410e64:	e2811001 	add	r1, r1, #1
c0410e68:	e1820f91 			@ <UNDEFINED> instruction: 0xe1820f91
c0410e6c:	e3300000 	teq	r0, #0
c0410e70:	1afffffa 	bne	c0410e60 <ffe_general_insert_flow+0xec>
c0410e74:	e1a0300d 	mov	r3, sp
c0410e78:	e3c33d7f 	bic	r3, r3, #8128	@ 0x1fc0
c0410e7c:	e3c3303f 	bic	r3, r3, #63	@ 0x3f
c0410e80:	e1a00004 	mov	r0, r4
c0410e84:	e5933010 	ldr	r3, [r3, #16]
c0410e88:	e58430a8 	str	r3, [r4, #168]	@ 0xa8
c0410e8c:	eb0030cb 	bl	c041d1c0 <ffe_get_rps_cpu>
c0410e90:	e58400ac 	str	r0, [r4, #172]	@ 0xac
c0410e94:	eb0030d5 	bl	c041d1f0 <ffe_inc_rps_flow_cnts>
c0410e98:	e5960000 	ldr	r0, [r6]
c0410e9c:	e28000b4 	add	r0, r0, #180	@ 0xb4
c0410ea0:	eb008de3 	bl	c0434634 <_raw_spin_unlock_bh>
c0410ea4:	e3a00000 	mov	r0, #0
c0410ea8:	e8bd81f0 	pop	{r4, r5, r6, r7, r8, pc}
c0410eac:	e5960000 	ldr	r0, [r6]
c0410eb0:	e28000b4 	add	r0, r0, #180	@ 0xb4
c0410eb4:	eb008dde 	bl	c0434634 <_raw_spin_unlock_bh>
c0410eb8:	e1a01004 	mov	r1, r4
c0410ebc:	e5953038 	ldr	r3, [r5, #56]	@ 0x38
c0410ec0:	e1a00005 	mov	r0, r5
c0410ec4:	e12fff33 	msr	SP_hyp, r3, lsr pc
c0410ec8:	e3a00000 	mov	r0, #0
c0410ecc:	e8bd81f0 	pop	{r4, r5, r6, r7, r8, pc}
