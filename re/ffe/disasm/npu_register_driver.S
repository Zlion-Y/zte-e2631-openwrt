
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c041b4f0 <npu_register_driver>:
c041b4f0:	e92d41f0 	push	{r4, r5, r6, r7, r8, lr}
c041b4f4:	e2505000 	subs	r5, r0, #0
c041b4f8:	e24dd008 	sub	sp, sp, #8
c041b4fc:	0a000074 	beq	c041b6d4 <npu_register_driver+0x1e4>
c041b500:	e5953010 	ldr	r3, [r5, #16]
c041b504:	e3530000 	cmp	r3, #0
c041b508:	0a000071 	beq	c041b6d4 <npu_register_driver+0x1e4>
c041b50c:	e5953018 	ldr	r3, [r5, #24]
c041b510:	e3530000 	cmp	r3, #0
c041b514:	0a00006e 	beq	c041b6d4 <npu_register_driver+0x1e4>
c041b518:	e5953020 	ldr	r3, [r5, #32]
c041b51c:	e3530000 	cmp	r3, #0
c041b520:	0a00006b 	beq	c041b6d4 <npu_register_driver+0x1e4>
c041b524:	e5953008 	ldr	r3, [r5, #8]
c041b528:	e3530000 	cmp	r3, #0
c041b52c:	0a000002 	beq	c041b53c <npu_register_driver+0x4c>
c041b530:	e5953014 	ldr	r3, [r5, #20]
c041b534:	e3530000 	cmp	r3, #0
c041b538:	0a000078 	beq	c041b720 <npu_register_driver+0x230>
c041b53c:	e30b6838 	tst	fp, #56, 16	@ 0x380000
c041b540:	e34c607a 	cmp	ip, #122	@ 0x7a
c041b544:	e5963000 	ldr	r3, [r6]
c041b548:	e5933008 	ldr	r3, [r3, #8]
c041b54c:	e3530000 	cmp	r3, #0
c041b550:	1a00006c 	bne	c041b708 <npu_register_driver+0x218>
c041b554:	e5952000 	ldr	r2, [r5]
c041b558:	e5853004 	str	r3, [r5, #4]
c041b55c:	e3520000 	cmp	r2, #0
c041b560:	0a000003 	beq	c041b574 <npu_register_driver+0x84>
c041b564:	e1a00005 	mov	r0, r5
c041b568:	eb0003cf 	bl	c041c4ac <npu_cap_setup>
c041b56c:	e3500000 	cmp	r0, #0
c041b570:	1a00005d 	bne	c041b6ec <npu_register_driver+0x1fc>
c041b574:	e1a00005 	mov	r0, r5
c041b578:	e3097484 	tst	r9, #132, 8	@ 0x84000000
c041b57c:	eb000506 	bl	c041c99c <npu_mc_filter_setup>
c041b580:	e1a00005 	mov	r0, r5
c041b584:	eb000556 	bl	c041cae4 <npu_flow_retry_setup>
c041b588:	e5953028 	ldr	r3, [r5, #40]	@ 0x28
c041b58c:	e3a02000 	mov	r2, #0
c041b590:	e34c7066 	cmp	ip, #102	@ 0x66
c041b594:	e1530002 	cmp	r3, r2
c041b598:	e585203c 	str	r2, [r5, #60]	@ 0x3c
c041b59c:	e30810d0 	tst	r8, #208	@ 0xd0
c041b5a0:	030a3844 	tsteq	sl, #68, 16	@ 0x440000
c041b5a4:	034c3041 	cmpeq	ip, #65	@ 0x41
c041b5a8:	05853028 	streq	r3, [r5, #40]	@ 0x28
c041b5ac:	e5970014 	ldr	r0, [r7, #20]
c041b5b0:	ebf25a78 	bl	c00b1f98 <kmem_cache_alloc>
c041b5b4:	e2504000 	subs	r4, r0, #0
c041b5b8:	0a00003d 	beq	c041b6b4 <npu_register_driver+0x1c4>
c041b5bc:	e30c1528 	tst	ip, #40, 10	@ 0xa000000
c041b5c0:	e30a2900 	tst	sl, #0, 18
c041b5c4:	e30c3530 	tst	ip, #48, 10	@ 0xc000000
c041b5c8:	e34c1058 	cmp	ip, #88	@ 0x58
c041b5cc:	e34c3058 	cmp	ip, #88	@ 0x58
c041b5d0:	e34c2041 	cmp	ip, #65	@ 0x41
c041b5d4:	e584300c 	str	r3, [r4, #12]
c041b5d8:	e3a08001 	mov	r8, #1
c041b5dc:	e5841000 	str	r1, [r4]
c041b5e0:	e5842004 	str	r2, [r4, #4]
c041b5e4:	e584501c 	str	r5, [r4, #28]
c041b5e8:	e5848008 	str	r8, [r4, #8]
c041b5ec:	ebffe404 	bl	c0414604 <ffe_register_cmd>
c041b5f0:	e5970014 	ldr	r0, [r7, #20]
c041b5f4:	e30810d0 	tst	r8, #208	@ 0xd0
c041b5f8:	ebf25a66 	bl	c00b1f98 <kmem_cache_alloc>
c041b5fc:	e2503000 	subs	r3, r0, #0
c041b600:	0a00002d 	beq	c041b6bc <npu_register_driver+0x1cc>
c041b604:	e30bc67c 	tst	fp, #124, 12	@ 0x7c00000
c041b608:	e30b184c 	tst	fp, #76, 16	@ 0x4c0000
c041b60c:	e30c2558 	tst	ip, #88, 10	@ 0x16000000
c041b610:	e5838008 	str	r8, [r3, #8]
c041b614:	e583501c 	str	r5, [r3, #28]
c041b618:	e34cc058 	cmp	ip, #88	@ 0x58
c041b61c:	e34c1041 	cmp	ip, #65	@ 0x41
c041b620:	e583c000 	str	ip, [r3]
c041b624:	e5831004 	str	r1, [r3, #4]
c041b628:	e34c2058 	cmp	ip, #88	@ 0x58
c041b62c:	e583200c 	str	r2, [r3, #12]
c041b630:	e3a04000 	mov	r4, #0
c041b634:	ebffe3f2 	bl	c0414604 <ffe_register_cmd>
c041b638:	e3a07003 	mov	r7, #3
c041b63c:	ebffff87 	bl	c041b460 <npu_filter_proc_init>
c041b640:	ebffd9d1 	bl	c0411d8c <ffe_disable_lock>
c041b644:	e5963000 	ldr	r3, [r6]
c041b648:	e5835008 	str	r5, [r3, #8]
c041b64c:	e5858038 	str	r8, [r5, #56]	@ 0x38
c041b650:	ea000003 	b	c041b664 <npu_register_driver+0x174>
c041b654:	e2844080 	add	r4, r4, #128	@ 0x80
c041b658:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b65c:	0a000010 	beq	c041b6a4 <npu_register_driver+0x1b4>
c041b660:	e5963000 	ldr	r3, [r6]
c041b664:	e5933004 	ldr	r3, [r3, #4]
c041b668:	e083c004 	add	ip, r3, r4
c041b66c:	e7933004 	ldr	r3, [r3, r4]
c041b670:	e3530000 	cmp	r3, #0
c041b674:	0afffff6 	beq	c041b654 <npu_register_driver+0x164>
c041b678:	e3a02000 	mov	r2, #0
c041b67c:	e3061108 	tst	r6, #8, 2
c041b680:	e58d7000 	str	r7, [sp]
c041b684:	e1a0000c 	mov	r0, ip
c041b688:	e34c1041 	cmp	ip, #65	@ 0x41
c041b68c:	e59cc054 	ldr	ip, [ip, #84]	@ 0x54
c041b690:	e1a03002 	mov	r3, r2
c041b694:	e2844080 	add	r4, r4, #128	@ 0x80
c041b698:	e12fff3c 	msr	SP_hyp, ip, lsr pc
c041b69c:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b6a0:	1affffee 	bne	c041b660 <npu_register_driver+0x170>
c041b6a4:	ebffd9c5 	bl	c0411dc0 <ffe_enable_unlock>
c041b6a8:	e3a00000 	mov	r0, #0
c041b6ac:	e28dd008 	add	sp, sp, #8
c041b6b0:	e8bd81f0 	pop	{r4, r5, r6, r7, r8, pc}
c041b6b4:	e3e00000 	mvn	r0, #0
c041b6b8:	eafffffb 	b	c041b6ac <npu_register_driver+0x1bc>
c041b6bc:	e1a00004 	mov	r0, r4
c041b6c0:	ebffe409 	bl	c04146ec <ffe_unregister_cmd>
c041b6c4:	e1a00004 	mov	r0, r4
c041b6c8:	ebf25bad 	bl	c00b2584 <kfree>
c041b6cc:	e3e00000 	mvn	r0, #0
c041b6d0:	eafffff5 	b	c041b6ac <npu_register_driver+0x1bc>
c041b6d4:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041b6d8:	e59f1058 	ldr	r1, [pc, #88]	@ c041b738 <npu_register_driver+0x248>
c041b6dc:	e300230a 	tst	r0, #671088640	@ 0x28000000
c041b6e0:	e34c0058 	cmp	ip, #88	@ 0x58
c041b6e4:	eb004520 	bl	c042cb6c <printk>
c041b6e8:	e7f001f2 	udf	#18
c041b6ec:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041b6f0:	e59f1040 	ldr	r1, [pc, #64]	@ c041b738 <npu_register_driver+0x248>
c041b6f4:	e34c0058 	cmp	ip, #88	@ 0x58
c041b6f8:	e3a02e32 	mov	r2, #800	@ 0x320
c041b6fc:	eb00451a 	bl	c042cb6c <printk>
c041b700:	e3e00000 	mvn	r0, #0
c041b704:	eaffffe8 	b	c041b6ac <npu_register_driver+0x1bc>
c041b708:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041b70c:	e59f1024 	ldr	r1, [pc, #36]	@ c041b738 <npu_register_driver+0x248>
c041b710:	e3002315 	tst	r0, #1409286144	@ 0x54000000
c041b714:	e34c0058 	cmp	ip, #88	@ 0x58
c041b718:	eb004513 	bl	c042cb6c <printk>
c041b71c:	e7f001f2 	udf	#18
c041b720:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041b724:	e59f100c 	ldr	r1, [pc, #12]	@ c041b738 <npu_register_driver+0x248>
c041b728:	e300230f 	tst	r0, #1006632960	@ 0x3c000000
c041b72c:	e34c0058 	cmp	ip, #88	@ 0x58
c041b730:	eb00450d 	bl	c042cb6c <printk>
c041b734:	e7f001f2 	udf	#18
c041b738:	c0495dc8 	subgt	r5, r9, r8, asr #27

c041b73c <ffe_npu_enable>:
c041b73c:	e92d40f0 	push	{r4, r5, r6, r7, lr}
c041b740:	e30b5838 	tst	fp, #56, 16	@ 0x380000
c041b744:	e34c507a 	cmp	ip, #122	@ 0x7a
c041b748:	e24dd00c 	sub	sp, sp, #12
c041b74c:	e5953000 	ldr	r3, [r5]
c041b750:	e3a04000 	mov	r4, #0
c041b754:	e3a06003 	mov	r6, #3
c041b758:	e5937008 	ldr	r7, [r3, #8]
c041b75c:	ebffd98a 	bl	c0411d8c <ffe_disable_lock>
c041b760:	ea000001 	b	c041b76c <ffe_npu_enable+0x30>
c041b764:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b768:	0a000010 	beq	c041b7b0 <ffe_npu_enable+0x74>
c041b76c:	e5953000 	ldr	r3, [r5]
c041b770:	e5933004 	ldr	r3, [r3, #4]
c041b774:	e083c004 	add	ip, r3, r4
c041b778:	e7933004 	ldr	r3, [r3, r4]
c041b77c:	e2844080 	add	r4, r4, #128	@ 0x80
c041b780:	e3530000 	cmp	r3, #0
c041b784:	0afffff6 	beq	c041b764 <ffe_npu_enable+0x28>
c041b788:	e3a02000 	mov	r2, #0
c041b78c:	e3061108 	tst	r6, #8, 2
c041b790:	e58d6000 	str	r6, [sp]
c041b794:	e1a0000c 	mov	r0, ip
c041b798:	e34c1041 	cmp	ip, #65	@ 0x41
c041b79c:	e59cc054 	ldr	ip, [ip, #84]	@ 0x54
c041b7a0:	e1a03002 	mov	r3, r2
c041b7a4:	e12fff3c 	msr	SP_hyp, ip, lsr pc
c041b7a8:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b7ac:	1affffee 	bne	c041b76c <ffe_npu_enable+0x30>
c041b7b0:	e3a03001 	mov	r3, #1
c041b7b4:	e5873038 	str	r3, [r7, #56]	@ 0x38
c041b7b8:	e28dd00c 	add	sp, sp, #12
c041b7bc:	e8bd40f0 	pop	{r4, r5, r6, r7, lr}
c041b7c0:	eaffd97e 	b	c0411dc0 <ffe_enable_unlock>

c041b7c4 <ffe_npu_disable>:
c041b7c4:	e92d40f0 	push	{r4, r5, r6, r7, lr}
c041b7c8:	e30b5838 	tst	fp, #56, 16	@ 0x380000
c041b7cc:	e34c507a 	cmp	ip, #122	@ 0x7a
c041b7d0:	e24dd00c 	sub	sp, sp, #12
c041b7d4:	e5953000 	ldr	r3, [r5]
c041b7d8:	e3a04000 	mov	r4, #0
c041b7dc:	e3a06003 	mov	r6, #3
c041b7e0:	e5937008 	ldr	r7, [r3, #8]
c041b7e4:	ebffd968 	bl	c0411d8c <ffe_disable_lock>
c041b7e8:	ea000001 	b	c041b7f4 <ffe_npu_disable+0x30>
c041b7ec:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b7f0:	0a000010 	beq	c041b838 <ffe_npu_disable+0x74>
c041b7f4:	e5953000 	ldr	r3, [r5]
c041b7f8:	e5933004 	ldr	r3, [r3, #4]
c041b7fc:	e083c004 	add	ip, r3, r4
c041b800:	e7933004 	ldr	r3, [r3, r4]
c041b804:	e2844080 	add	r4, r4, #128	@ 0x80
c041b808:	e3530000 	cmp	r3, #0
c041b80c:	0afffff6 	beq	c041b7ec <ffe_npu_disable+0x28>
c041b810:	e3a02000 	mov	r2, #0
c041b814:	e30a19d4 	tst	sl, #212, 18	@ 0x350000
c041b818:	e58d6000 	str	r6, [sp]
c041b81c:	e1a0000c 	mov	r0, ip
c041b820:	e34c1041 	cmp	ip, #65	@ 0x41
c041b824:	e59cc054 	ldr	ip, [ip, #84]	@ 0x54
c041b828:	e1a03002 	mov	r3, r2
c041b82c:	e12fff3c 	msr	SP_hyp, ip, lsr pc
c041b830:	e3540c06 	cmp	r4, #1536	@ 0x600
c041b834:	1affffee 	bne	c041b7f4 <ffe_npu_disable+0x30>
c041b838:	e3a03000 	mov	r3, #0
c041b83c:	e5873038 	str	r3, [r7, #56]	@ 0x38
c041b840:	e28dd00c 	add	sp, sp, #12
c041b844:	e8bd40f0 	pop	{r4, r5, r6, r7, lr}
c041b848:	eaffd95c 	b	c0411dc0 <ffe_enable_unlock>

c041b84c <ffe_cmd_npu_enable>:
c041b84c:	e3510001 	cmp	r1, #1
c041b850:	da000022 	ble	c041b8e0 <ffe_cmd_npu_enable+0x94>
c041b854:	e92d4010 	push	{r4, lr}
c041b858:	e30b4838 	tst	fp, #56, 16	@ 0x380000
c041b85c:	e34c407a 	cmp	ip, #122	@ 0x7a
c041b860:	e24dd008 	sub	sp, sp, #8
c041b864:	e5943000 	ldr	r3, [r4]
c041b868:	e5933008 	ldr	r3, [r3, #8]
c041b86c:	e3530000 	cmp	r3, #0
c041b870:	0a00001c 	beq	c041b8e8 <ffe_cmd_npu_enable+0x9c>
c041b874:	e30b1aa4 	tst	fp, #164, 20	@ 0xa4000
c041b878:	e5920004 	ldr	r0, [r2, #4]
c041b87c:	e34c1058 	cmp	ip, #88	@ 0x58
c041b880:	e28d2004 	add	r2, sp, #4
c041b884:	ebf59e9b 	bl	c01832f8 <sscanf>
c041b888:	e3500001 	cmp	r0, #1
c041b88c:	1a00000e 	bne	c041b8cc <ffe_cmd_npu_enable+0x80>
c041b890:	e59d3004 	ldr	r3, [sp, #4]
c041b894:	e3530000 	cmp	r3, #0
c041b898:	1a000009 	bne	c041b8c4 <ffe_cmd_npu_enable+0x78>
c041b89c:	ebffffc8 	bl	c041b7c4 <ffe_npu_disable>
c041b8a0:	e5943000 	ldr	r3, [r4]
c041b8a4:	e30c0588 	tst	ip, #136, 10	@ 0x22000000
c041b8a8:	e34c0058 	cmp	ip, #88	@ 0x58
c041b8ac:	e5933008 	ldr	r3, [r3, #8]
c041b8b0:	e5931038 	ldr	r1, [r3, #56]	@ 0x38
c041b8b4:	eb0044ac 	bl	c042cb6c <printk>
c041b8b8:	e3a00000 	mov	r0, #0
c041b8bc:	e28dd008 	add	sp, sp, #8
c041b8c0:	e8bd8010 	pop	{r4, pc}
c041b8c4:	ebffff9c 	bl	c041b73c <ffe_npu_enable>
c041b8c8:	eafffff4 	b	c041b8a0 <ffe_cmd_npu_enable+0x54>
c041b8cc:	e30b0a64 	tst	fp, #100, 20	@ 0x64000
c041b8d0:	e34c0058 	cmp	ip, #88	@ 0x58
c041b8d4:	eb0044a4 	bl	c042cb6c <printk>
c041b8d8:	e3e00000 	mvn	r0, #0
c041b8dc:	eafffff6 	b	c041b8bc <ffe_cmd_npu_enable+0x70>
c041b8e0:	e3e00000 	mvn	r0, #0
c041b8e4:	e12fff1e 	msr	SP_hyp, lr, lsl pc
c041b8e8:	e30c0570 	tst	ip, #112, 10	@ 0x1c000000
c041b8ec:	e34c0058 	cmp	ip, #88	@ 0x58
c041b8f0:	eb00449d 	bl	c042cb6c <printk>
c041b8f4:	e3e00000 	mvn	r0, #0
c041b8f8:	eaffffef 	b	c041b8bc <ffe_cmd_npu_enable+0x70>
