
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c04107f8 <ffe_build_packet_cmd>:
c04107f8:	e92d41f0 	push	{r4, r5, r6, r7, r8, lr}
c04107fc:	e3a0c000 	mov	ip, #0
c0410800:	e5903050 	ldr	r3, [r0, #80]	@ 0x50
c0410804:	e24dd008 	sub	sp, sp, #8
c0410808:	e1a06001 	mov	r6, r1
c041080c:	e1a07002 	mov	r7, r2
c0410810:	e153000c 	cmp	r3, ip
c0410814:	e58dc004 	str	ip, [sp, #4]
c0410818:	0a000022 	beq	c04108a8 <ffe_build_packet_cmd+0xb0>
c041081c:	e12fff33 	msr	SP_hyp, r3, lsr pc
c0410820:	e30b8838 	tst	fp, #56, 16	@ 0x380000
c0410824:	e1a04000 	mov	r4, r0
c0410828:	e34c807a 	cmp	ip, #122	@ 0x7a
c041082c:	e3a05000 	mov	r5, #0
c0410830:	e4d40001 	ldrb	r0, [r4], #1
c0410834:	e350000f 	cmp	r0, #15
c0410838:	8a000016 	bhi	c0410898 <ffe_build_packet_cmd+0xa0>
c041083c:	e5982000 	ldr	r2, [r8]
c0410840:	e592c000 	ldr	ip, [r2]
c0410844:	e08cc300 	add	ip, ip, r0, lsl #6
c0410848:	e59cc020 	ldr	ip, [ip, #32]
c041084c:	e35c0000 	cmp	ip, #0
c0410850:	0afffff6 	beq	c0410830 <ffe_build_packet_cmd+0x38>
c0410854:	e3500000 	cmp	r0, #0
c0410858:	0a000017 	beq	c04108bc <ffe_build_packet_cmd+0xc4>
c041085c:	e1a00007 	mov	r0, r7
c0410860:	e1a01006 	mov	r1, r6
c0410864:	e28d2004 	add	r2, sp, #4
c0410868:	e12fff3c 	msr	SP_hyp, ip, lsr pc
c041086c:	e3500000 	cmp	r0, #0
c0410870:	0a000009 	beq	c041089c <ffe_build_packet_cmd+0xa4>
c0410874:	e2853001 	add	r3, r5, #1
c0410878:	e5972024 	ldr	r2, [r7, #36]	@ 0x24
c041087c:	e353000c 	cmp	r3, #12
c0410880:	e7820105 	str	r0, [r2, r5, lsl #2]
c0410884:	ca000006 	bgt	c04108a4 <ffe_build_packet_cmd+0xac>
c0410888:	e4d40001 	ldrb	r0, [r4], #1
c041088c:	e1a05003 	mov	r5, r3
c0410890:	e350000f 	cmp	r0, #15
c0410894:	9affffe8 	bls	c041083c <ffe_build_packet_cmd+0x44>
c0410898:	e7f001f2 	udf	#18
c041089c:	e1a03005 	mov	r3, r5
c04108a0:	eafffff8 	b	c0410888 <ffe_build_packet_cmd+0x90>
c04108a4:	e7f001f2 	udf	#18
c04108a8:	e30b3838 	tst	fp, #56, 16	@ 0x380000
c04108ac:	e34c307a 	cmp	ip, #122	@ 0x7a
c04108b0:	e5933000 	ldr	r3, [r3]
c04108b4:	e5930098 	ldr	r0, [r3, #152]	@ 0x98
c04108b8:	eaffffd8 	b	c0410820 <ffe_build_packet_cmd+0x28>
c04108bc:	e59d2004 	ldr	r2, [sp, #4]
c04108c0:	e587202c 	str	r2, [r7, #44]	@ 0x2c
c04108c4:	e28dd008 	add	sp, sp, #8
c04108c8:	e8bd81f0 	pop	{r4, r5, r6, r7, r8, pc}
