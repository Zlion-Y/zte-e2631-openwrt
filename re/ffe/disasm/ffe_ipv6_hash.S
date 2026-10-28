
/tmp/vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c041a274 <ffe_ipv6_hash>:
c041a274:	e92d4010 	push	{r4, lr}
c041a278:	e30b1eff 	tst	fp, #4080	@ 0xff0
c041a27c:	e5902008 	ldr	r2, [r0, #8]
c041a280:	e34d1ead 	cmp	sp, #2768	@ 0xad0
c041a284:	e5903000 	ldr	r3, [r0]
c041a288:	e081e002 	add	lr, r1, r2
c041a28c:	e5904004 	ldr	r4, [r0, #4]
c041a290:	e0622003 	rsb	r2, r2, r3
c041a294:	e590c00c 	ldr	ip, [r0, #12]
c041a298:	e0222e6e 	eor	r2, r2, lr, ror #28
c041a29c:	e0811004 	add	r1, r1, r4
c041a2a0:	e0620001 	rsb	r0, r2, r1
c041a2a4:	e08e1001 	add	r1, lr, r1
c041a2a8:	e0200d62 	eor	r0, r0, r2, ror #26
c041a2ac:	e0822001 	add	r2, r2, r1
c041a2b0:	e0601001 	rsb	r1, r0, r1
c041a2b4:	e080e002 	add	lr, r0, r2
c041a2b8:	e0213c60 	eor	r3, r1, r0, ror #24
c041a2bc:	e0630002 	rsb	r0, r3, r2
c041a2c0:	e083200e 	add	r2, r3, lr
c041a2c4:	e0200863 	eor	r0, r0, r3, ror #16
c041a2c8:	e060300e 	rsb	r3, r0, lr
c041a2cc:	e0801002 	add	r1, r0, r2
c041a2d0:	e02336e0 	eor	r3, r3, r0, ror #13
c041a2d4:	e081000c 	add	r0, r1, ip
c041a2d8:	e0632002 	rsb	r2, r3, r2
c041a2dc:	e0831001 	add	r1, r3, r1
c041a2e0:	e0223e63 	eor	r3, r2, r3, ror #28
c041a2e4:	e0233001 	eor	r3, r3, r1
c041a2e8:	e0433961 	sub	r3, r3, r1, ror #18
c041a2ec:	e0200003 	eor	r0, r0, r3
c041a2f0:	e0400ae3 	sub	r0, r0, r3, ror #21
c041a2f4:	e0202001 	eor	r2, r0, r1
c041a2f8:	e04223e0 	sub	r2, r2, r0, ror #7
c041a2fc:	e0233002 	eor	r3, r3, r2
c041a300:	e0433862 	sub	r3, r3, r2, ror #16
c041a304:	e0200003 	eor	r0, r0, r3
c041a308:	e0400e63 	sub	r0, r0, r3, ror #28
c041a30c:	e0222000 	eor	r2, r2, r0
c041a310:	e0420960 	sub	r0, r2, r0, ror #18
c041a314:	e0233000 	eor	r3, r3, r0
c041a318:	e0430460 	sub	r0, r3, r0, ror #8
c041a31c:	e8bd8010 	pop	{r4, pc}

c041a320 <ipv6_multicast_init>:
c041a320:	e92d4030 	push	{r4, r5, lr}
c041a324:	e1a04000 	mov	r4, r0
c041a328:	e5900020 	ldr	r0, [r0, #32]
c041a32c:	e24dd00c 	sub	sp, sp, #12
c041a330:	e3a010d0 	mov	r1, #208	@ 0xd0
c041a334:	e1a00100 	lsl	r0, r0, #2
c041a338:	ebf25ec8 	bl	c00b1e60 <__kmalloc>
c041a33c:	e3500000 	cmp	r0, #0
c041a340:	0a000022 	beq	c041a3d0 <ipv6_multicast_init+0xb0>
c041a344:	e5943020 	ldr	r3, [r4, #32]
c041a348:	e3530000 	cmp	r3, #0
c041a34c:	c2402004 	subgt	r2, r0, #4
c041a350:	c3a03000 	movgt	r3, #0
c041a354:	c1a0c003 	movgt	ip, r3
c041a358:	da000004 	ble	c041a370 <ipv6_multicast_init+0x50>
c041a35c:	e5a2c004 	str	ip, [r2, #4]!
c041a360:	e2833001 	add	r3, r3, #1
c041a364:	e5941020 	ldr	r1, [r4, #32]
c041a368:	e1510003 	cmp	r1, r3
c041a36c:	cafffffa 	bgt	c041a35c <ipv6_multicast_init+0x3c>
c041a370:	e3a05000 	mov	r5, #0
c041a374:	e584000c 	str	r0, [r4, #12]
c041a378:	e5845010 	str	r5, [r4, #16]
c041a37c:	e3a0000a 	mov	r0, #10
c041a380:	ebffd905 	bl	c041079c <ffe_get_flow_valid_key>
c041a384:	e584001c 	str	r0, [r4, #28]
c041a388:	ebffdd99 	bl	c04119f4 <ffe_calc_key_len>
c041a38c:	e1a02005 	mov	r2, r5
c041a390:	e3a03a02 	mov	r3, #8192	@ 0x2000
c041a394:	e1a01000 	mov	r1, r0
c041a398:	e30c0484 	tst	ip, #132, 8	@ 0x84000000
c041a39c:	e34c0058 	cmp	ip, #88	@ 0x58
c041a3a0:	e58d5000 	str	r5, [sp]
c041a3a4:	ebf1fc72 	bl	c0099574 <kmem_cache_create>
c041a3a8:	e1500005 	cmp	r0, r5
c041a3ac:	e5840018 	str	r0, [r4, #24]
c041a3b0:	0a000006 	beq	c041a3d0 <ipv6_multicast_init+0xb0>
c041a3b4:	e1a00004 	mov	r0, r4
c041a3b8:	ebfff546 	bl	c04178d8 <ffe_create_proto_proc_file>
c041a3bc:	e1a00005 	mov	r0, r5
c041a3c0:	e3a03002 	mov	r3, #2
c041a3c4:	e5843068 	str	r3, [r4, #104]	@ 0x68
c041a3c8:	e28dd00c 	add	sp, sp, #12
c041a3cc:	e8bd8030 	pop	{r4, r5, pc}
c041a3d0:	e3e00000 	mvn	r0, #0
c041a3d4:	eafffffb 	b	c041a3c8 <ipv6_multicast_init+0xa8>

c041a3d8 <ipv6_multicast_list_each_flow>:
c041a3d8:	e59d3000 	ldr	r3, [sp]
c041a3dc:	eaffdd2c 	b	c0411894 <ffe_general_list_each_flow>

c041a3e0 <ipv6_multicast_insert_flow>:
c041a3e0:	e92d4ff0 	push	{r4, r5, r6, r7, r8, r9, sl, fp, lr}
c041a3e4:	e24dd00c 	sub	sp, sp, #12
c041a3e8:	e5923010 	ldr	r3, [r2, #16]
c041a3ec:	e1a09000 	mov	r9, r0
c041a3f0:	e1a08002 	mov	r8, r2
c041a3f4:	e58d1004 	str	r1, [sp, #4]
c041a3f8:	e5d36007 	ldrb	r6, [r3, #7]
c041a3fc:	e0836006 	add	r6, r3, r6
c041a400:	e2867018 	add	r7, r6, #24
c041a404:	e1a00007 	mov	r0, r7
c041a408:	ebffff99 	bl	c041a274 <ffe_ipv6_hash>
c041a40c:	e599200c 	ldr	r2, [r9, #12]
c041a410:	e1a0a000 	mov	sl, r0
c041a414:	e200100f 	and	r1, r0, #15
c041a418:	e7924101 	ldr	r4, [r2, r1, lsl #2]
c041a41c:	e3540000 	cmp	r4, #0
c041a420:	1a000007 	bne	c041a444 <ipv6_multicast_insert_flow+0x64>
c041a424:	e1a00009 	mov	r0, r9
c041a428:	e59d1004 	ldr	r1, [sp, #4]
c041a42c:	e1a02008 	mov	r2, r8
c041a430:	e1a0300a 	mov	r3, sl
c041a434:	ebffda4e 	bl	c0410d74 <ffe_general_insert_flow>
c041a438:	e3a00000 	mov	r0, #0
c041a43c:	e28dd00c 	add	sp, sp, #12
c041a440:	e8bd8ff0 	pop	{r4, r5, r6, r7, r8, r9, sl, fp, pc}
c041a444:	e2866008 	add	r6, r6, #8
c041a448:	ea000002 	b	c041a458 <ipv6_multicast_insert_flow+0x78>
c041a44c:	e5944000 	ldr	r4, [r4]
c041a450:	e3540000 	cmp	r4, #0
c041a454:	0afffff2 	beq	c041a424 <ipv6_multicast_insert_flow+0x44>
c041a458:	e594201c 	ldr	r2, [r4, #28]
c041a45c:	e5945010 	ldr	r5, [r4, #16]
c041a460:	e15a0002 	cmp	sl, r2
c041a464:	e5d5c007 	ldrb	ip, [r5, #7]
c041a468:	1afffff7 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a46c:	e085b00c 	add	fp, r5, ip
c041a470:	e1a01006 	mov	r1, r6
c041a474:	e28b0008 	add	r0, fp, #8
c041a478:	e3a02010 	mov	r2, #16
c041a47c:	ebf59477 	bl	c017f660 <memcmp>
c041a480:	e3500000 	cmp	r0, #0
c041a484:	1afffff0 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a488:	e28b0018 	add	r0, fp, #24
c041a48c:	e1a01007 	mov	r1, r7
c041a490:	e3a02010 	mov	r2, #16
c041a494:	ebf59471 	bl	c017f660 <memcmp>
c041a498:	e3500000 	cmp	r0, #0
c041a49c:	1affffea 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a4a0:	e5982010 	ldr	r2, [r8, #16]
c041a4a4:	e5d20002 	ldrb	r0, [r2, #2]
c041a4a8:	e3100001 	tst	r0, #1
c041a4ac:	1a000041 	bne	c041a5b8 <ipv6_multicast_insert_flow+0x1d8>
c041a4b0:	e0821000 	add	r1, r2, r0
c041a4b4:	e5d5c002 	ldrb	ip, [r5, #2]
c041a4b8:	e2911000 	adds	r1, r1, #0
c041a4bc:	13a01001 	movne	r1, #1
c041a4c0:	e31c0001 	tst	ip, #1
c041a4c4:	1a000041 	bne	c041a5d0 <ipv6_multicast_insert_flow+0x1f0>
c041a4c8:	e175000c 	cmn	r5, ip
c041a4cc:	13a0e001 	movne	lr, #1
c041a4d0:	03a0e000 	moveq	lr, #0
c041a4d4:	e11e0001 	tst	lr, r1
c041a4d8:	0a00006f 	beq	c041a69c <ipv6_multicast_insert_flow+0x2bc>
c041a4dc:	e19200b0 	ldrh	r0, [r2, r0]
c041a4e0:	e19510bc 	ldrh	r1, [r5, ip]
c041a4e4:	e6bf0fb0 			@ <UNDEFINED> instruction: 0xe6bf0fb0
c041a4e8:	e6bf1fb1 			@ <UNDEFINED> instruction: 0xe6bf1fb1
c041a4ec:	e7eb0050 			@ <UNDEFINED> instruction: 0xe7eb0050
c041a4f0:	e7eb1051 			@ <UNDEFINED> instruction: 0xe7eb1051
c041a4f4:	e1500001 	cmp	r0, r1
c041a4f8:	1affffd3 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a4fc:	e5d21003 	ldrb	r1, [r2, #3]
c041a500:	e3110001 	tst	r1, #1
c041a504:	0a000036 	beq	c041a5e4 <ipv6_multicast_insert_flow+0x204>
c041a508:	e5d52003 	ldrb	r2, [r5, #3]
c041a50c:	e3120001 	tst	r2, #1
c041a510:	1a000004 	bne	c041a528 <ipv6_multicast_insert_flow+0x148>
c041a514:	e1750002 	cmn	r5, r2
c041a518:	13a02001 	movne	r2, #1
c041a51c:	03a02000 	moveq	r2, #0
c041a520:	e3520000 	cmp	r2, #0
c041a524:	1affffc8 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a528:	e598b014 	ldr	fp, [r8, #20]
c041a52c:	e5945014 	ldr	r5, [r4, #20]
c041a530:	e5db2001 	ldrb	r2, [fp, #1]
c041a534:	e3120001 	tst	r2, #1
c041a538:	0a00003c 	beq	c041a630 <ipv6_multicast_insert_flow+0x250>
c041a53c:	e5d52001 	ldrb	r2, [r5, #1]
c041a540:	e3120001 	tst	r2, #1
c041a544:	1a000004 	bne	c041a55c <ipv6_multicast_insert_flow+0x17c>
c041a548:	e1750002 	cmn	r5, r2
c041a54c:	13a02001 	movne	r2, #1
c041a550:	03a02000 	moveq	r2, #0
c041a554:	e3520000 	cmp	r2, #0
c041a558:	1affffbb 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a55c:	e5db1000 	ldrb	r1, [fp]
c041a560:	e3110001 	tst	r1, #1
c041a564:	1a000043 	bne	c041a678 <ipv6_multicast_insert_flow+0x298>
c041a568:	e08b2001 	add	r2, fp, r1
c041a56c:	e5d50000 	ldrb	r0, [r5]
c041a570:	e2922000 	adds	r2, r2, #0
c041a574:	13a02001 	movne	r2, #1
c041a578:	e3100001 	tst	r0, #1
c041a57c:	1a000043 	bne	c041a690 <ipv6_multicast_insert_flow+0x2b0>
c041a580:	e1750000 	cmn	r5, r0
c041a584:	13a0c001 	movne	ip, #1
c041a588:	03a0c000 	moveq	ip, #0
c041a58c:	e11c0002 	tst	ip, r2
c041a590:	0a000045 	beq	c041a6ac <ipv6_multicast_insert_flow+0x2cc>
c041a594:	e79b1001 	ldr	r1, [fp, r1]
c041a598:	e7952000 	ldr	r2, [r5, r0]
c041a59c:	e1510002 	cmp	r1, r2
c041a5a0:	1affffa9 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a5a4:	e1a01008 	mov	r1, r8
c041a5a8:	e5993038 	ldr	r3, [r9, #56]	@ 0x38
c041a5ac:	e1a00009 	mov	r0, r9
c041a5b0:	e12fff33 	msr	SP_hyp, r3, lsr pc
c041a5b4:	eaffff9f 	b	c041a438 <ipv6_multicast_insert_flow+0x58>
c041a5b8:	e5d51002 	ldrb	r1, [r5, #2]
c041a5bc:	e3110001 	tst	r1, #1
c041a5c0:	1affffcd 	bne	c041a4fc <ipv6_multicast_insert_flow+0x11c>
c041a5c4:	e1750001 	cmn	r5, r1
c041a5c8:	13a01001 	movne	r1, #1
c041a5cc:	03a01000 	moveq	r1, #0
c041a5d0:	e3510000 	cmp	r1, #0
c041a5d4:	1affff9c 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a5d8:	e5d21003 	ldrb	r1, [r2, #3]
c041a5dc:	e3110001 	tst	r1, #1
c041a5e0:	1affffc8 	bne	c041a508 <ipv6_multicast_insert_flow+0x128>
c041a5e4:	e5d50003 	ldrb	r0, [r5, #3]
c041a5e8:	e0822001 	add	r2, r2, r1
c041a5ec:	e3100001 	tst	r0, #1
c041a5f0:	0a000002 	beq	c041a600 <ipv6_multicast_insert_flow+0x220>
c041a5f4:	e2922000 	adds	r2, r2, #0
c041a5f8:	13a02001 	movne	r2, #1
c041a5fc:	eaffffc7 	b	c041a520 <ipv6_multicast_insert_flow+0x140>
c041a600:	e2921000 	adds	r1, r2, #0
c041a604:	13a01001 	movne	r1, #1
c041a608:	e0955000 	adds	r5, r5, r0
c041a60c:	13a00001 	movne	r0, #1
c041a610:	03a00000 	moveq	r0, #0
c041a614:	e1100001 	tst	r0, r1
c041a618:	0a000025 	beq	c041a6b4 <ipv6_multicast_insert_flow+0x2d4>
c041a61c:	e1d210b2 	ldrh	r1, [r2, #2]
c041a620:	e1d520b2 	ldrh	r2, [r5, #2]
c041a624:	e1510002 	cmp	r1, r2
c041a628:	1affff87 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a62c:	eaffffbd 	b	c041a528 <ipv6_multicast_insert_flow+0x148>
c041a630:	e08b0002 	add	r0, fp, r2
c041a634:	e5d51001 	ldrb	r1, [r5, #1]
c041a638:	e2902000 	adds	r2, r0, #0
c041a63c:	13a02001 	movne	r2, #1
c041a640:	e3110001 	tst	r1, #1
c041a644:	1affffc2 	bne	c041a554 <ipv6_multicast_insert_flow+0x174>
c041a648:	e0951001 	adds	r1, r5, r1
c041a64c:	13a0c001 	movne	ip, #1
c041a650:	03a0c000 	moveq	ip, #0
c041a654:	e11c0002 	tst	ip, r2
c041a658:	0a000011 	beq	c041a6a4 <ipv6_multicast_insert_flow+0x2c4>
c041a65c:	e3a02006 	mov	r2, #6
c041a660:	ebf593fe 	bl	c017f660 <memcmp>
c041a664:	e3500000 	cmp	r0, #0
c041a668:	1affff77 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a66c:	e5db1000 	ldrb	r1, [fp]
c041a670:	e3110001 	tst	r1, #1
c041a674:	0affffbb 	beq	c041a568 <ipv6_multicast_insert_flow+0x188>
c041a678:	e5d52000 	ldrb	r2, [r5]
c041a67c:	e3120001 	tst	r2, #1
c041a680:	1affffc7 	bne	c041a5a4 <ipv6_multicast_insert_flow+0x1c4>
c041a684:	e1750002 	cmn	r5, r2
c041a688:	13a02001 	movne	r2, #1
c041a68c:	03a02000 	moveq	r2, #0
c041a690:	e3520000 	cmp	r2, #0
c041a694:	1affff6c 	bne	c041a44c <ipv6_multicast_insert_flow+0x6c>
c041a698:	eaffffc1 	b	c041a5a4 <ipv6_multicast_insert_flow+0x1c4>
c041a69c:	e18e1001 	orr	r1, lr, r1
c041a6a0:	eaffffca 	b	c041a5d0 <ipv6_multicast_insert_flow+0x1f0>
c041a6a4:	e18c2002 	orr	r2, ip, r2
c041a6a8:	eaffffa9 	b	c041a554 <ipv6_multicast_insert_flow+0x174>
c041a6ac:	e18c2002 	orr	r2, ip, r2
c041a6b0:	eafffff6 	b	c041a690 <ipv6_multicast_insert_flow+0x2b0>
c041a6b4:	e1802001 	orr	r2, r0, r1
c041a6b8:	eaffff98 	b	c041a520 <ipv6_multicast_insert_flow+0x140>

c041a6bc <ipv6_multicast_build_flow>:
c041a6bc:	e5923028 	ldr	r3, [r2, #40]	@ 0x28
c041a6c0:	e3833080 	orr	r3, r3, #128	@ 0x80
c041a6c4:	e5823028 	str	r3, [r2, #40]	@ 0x28
c041a6c8:	eaffdc4e 	b	c0411808 <ffe_general_build_flow>

c041a6cc <ipv6_multicast_free_flow>:
c041a6cc:	e92d4008 	push	{r3, lr}
c041a6d0:	ebffdc4a 	bl	c0411800 <ffe_general_free_flow>
c041a6d4:	e3a00000 	mov	r0, #0
c041a6d8:	e8bd8008 	pop	{r3, pc}

c041a6dc <ipv6_multicast_delete_flow>:
c041a6dc:	eaffdac5 	b	c04111f8 <ffe_general_delete_flow>

c041a6e0 <ipv6_multicast_dup_flow>:
c041a6e0:	eaffdbf6 	b	c04116c0 <ffe_general_dup_flow>

c041a6e4 <ipv6_multicast_alloc_flow>:
c041a6e4:	eaffd9f9 	b	c0410ed0 <ffe_general_alloc_flow>

c041a6e8 <ffe_qos_ioctl_event>:
c041a6e8:	e92d4008 	push	{r3, lr}
c041a6ec:	e241200c 	sub	r2, r1, #12
c041a6f0:	e2413006 	sub	r3, r1, #6
c041a6f4:	e3530001 	cmp	r3, #1
c041a6f8:	83520001 	cmphi	r2, #1
c041a6fc:	9a000004 	bls	c041a714 <ffe_qos_ioctl_event+0x2c>
c041a700:	e2411009 	sub	r1, r1, #9
c041a704:	e3510001 	cmp	r1, #1
c041a708:	9a000001 	bls	c041a714 <ffe_qos_ioctl_event+0x2c>
c041a70c:	e3a00000 	mov	r0, #0
c041a710:	e8bd8008 	pop	{r3, pc}
c041a714:	e3010fff 	tst	r1, #1020	@ 0x3fc
c041a718:	e3400700 	cmp	r0, #0, 14
c041a71c:	ebffed71 	bl	c0415ce8 <ffe_flush_flow_by_type>
c041a720:	e3a00000 	mov	r0, #0
c041a724:	e8bd8008 	pop	{r3, pc}

c041a728 <ffe_qos_notify_handler>:
c041a728:	e3510003 	cmp	r1, #3
c041a72c:	0a000001 	beq	c041a738 <ffe_qos_notify_handler+0x10>
c041a730:	e3a00001 	mov	r0, #1
c041a734:	e12fff1e 	msr	SP_hyp, lr, lsl pc
c041a738:	e5921018 	ldr	r1, [r2, #24]
c041a73c:	e3043334 	tst	r4, #52, 6	@ 0xd0000000
c041a740:	e34c3041 	cmp	ip, #65	@ 0x41
c041a744:	e5911004 	ldr	r1, [r1, #4]
c041a748:	e1510003 	cmp	r1, r3
c041a74c:	0afffff7 	beq	c041a730 <ffe_qos_notify_handler+0x8>
c041a750:	e592101c 	ldr	r1, [r2, #28]
c041a754:	e5913080 	ldr	r3, [r1, #128]	@ 0x80
c041a758:	e20308ff 	and	r0, r3, #16711680	@ 0xff0000
c041a75c:	e35008ff 	cmp	r0, #16711680	@ 0xff0000
c041a760:	05920014 	ldreq	r0, [r2, #20]
c041a764:	0590307c 	ldreq	r3, [r0, #124]	@ 0x7c
c041a768:	03833008 	orreq	r3, r3, #8
c041a76c:	0580307c 	streq	r3, [r0, #124]	@ 0x7c
c041a770:	e3a00001 	mov	r0, #1
c041a774:	05913080 	ldreq	r3, [r1, #128]	@ 0x80
c041a778:	e3130302 	tst	r3, #134217728	@ 0x8000000
c041a77c:	15922014 	ldrne	r2, [r2, #20]
c041a780:	1592307c 	ldrne	r3, [r2, #124]	@ 0x7c
c041a784:	13833010 	orrne	r3, r3, #16
c041a788:	1582307c 	strne	r3, [r2, #124]	@ 0x7c
c041a78c:	e12fff1e 	msr	SP_hyp, lr, lsl pc
