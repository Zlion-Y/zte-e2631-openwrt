
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c041c4ac <npu_cap_setup>:
c041c4ac:	e92d4010 	push	{r4, lr}
c041c4b0:	e1a04000 	mov	r4, r0
c041c4b4:	e5903000 	ldr	r3, [r0]
c041c4b8:	e24dd008 	sub	sp, sp, #8
c041c4bc:	e5932008 	ldr	r2, [r3, #8]
c041c4c0:	e3520000 	cmp	r2, #0
c041c4c4:	0a000011 	beq	c041c510 <npu_cap_setup+0x64>
c041c4c8:	e5932048 	ldr	r2, [r3, #72]	@ 0x48
c041c4cc:	e3520000 	cmp	r2, #0
c041c4d0:	0a00000e 	beq	c041c510 <npu_cap_setup+0x64>
c041c4d4:	e30c264c 	tst	ip, #76, 12	@ 0x4c00000
c041c4d8:	e2833f52 	add	r3, r3, #328	@ 0x148
c041c4dc:	e34c2041 	cmp	ip, #65	@ 0x41
c041c4e0:	ea000002 	b	c041c4f0 <npu_cap_setup+0x44>
c041c4e4:	e5131100 	ldr	r1, [r3, #-256]	@ 0xffffff00
c041c4e8:	e3510000 	cmp	r1, #0
c041c4ec:	0a000006 	beq	c041c50c <npu_cap_setup+0x60>
c041c4f0:	e5131148 	ldr	r1, [r3, #-328]	@ 0xfffffeb8
c041c4f4:	e2833f52 	add	r3, r3, #328	@ 0x148
c041c4f8:	e3510000 	cmp	r1, #0
c041c4fc:	e5131140 	ldr	r1, [r3, #-320]	@ 0xfffffec0
c041c500:	05032290 	streq	r2, [r3, #-656]	@ 0xfffffd70
c041c504:	e3510000 	cmp	r1, #0
c041c508:	1afffff5 	bne	c041c4e4 <npu_cap_setup+0x38>
c041c50c:	e5943000 	ldr	r3, [r4]
c041c510:	e30dced8 	tst	sp, #216, 28	@ 0xd80
c041c514:	e3a01004 	mov	r1, #4
c041c518:	e34cc065 	cmp	ip, #101	@ 0x65
c041c51c:	e1a02004 	mov	r2, r4
c041c520:	e58c3024 	str	r3, [ip, #36]	@ 0x24
c041c524:	e1a0000c 	mov	r0, ip
c041c528:	e5943000 	ldr	r3, [r4]
c041c52c:	e58c304c 	str	r3, [ip, #76]	@ 0x4c
c041c530:	e5943000 	ldr	r3, [r4]
c041c534:	e58c3074 	str	r3, [ip, #116]	@ 0x74
c041c538:	e5943000 	ldr	r3, [r4]
c041c53c:	e58c309c 	str	r3, [ip, #156]	@ 0x9c
c041c540:	ebfffb97 	bl	c041b3a4 <npu_register_filter>
c041c544:	e30b3838 	tst	fp, #56, 16	@ 0x380000
c041c548:	e34c307a 	cmp	ip, #122	@ 0x7a
c041c54c:	e5941000 	ldr	r1, [r4]
c041c550:	e5933000 	ldr	r3, [r3]
c041c554:	e5932148 	ldr	r2, [r3, #328]	@ 0x148
c041c558:	e3520000 	cmp	r2, #0
c041c55c:	0a00001e 	beq	c041c5dc <npu_cap_setup+0x130>
c041c560:	e30c071c 	tst	ip, #28, 14	@ 0x700000
c041c564:	e58d1000 	str	r1, [sp]
c041c568:	e34c0058 	cmp	ip, #88	@ 0x58
c041c56c:	e3a01f69 	mov	r1, #420	@ 0x1a4
c041c570:	e59f309c 	ldr	r3, [pc, #156]	@ c041c614 <npu_cap_setup+0x168>
c041c574:	ebf39b44 	bl	c010328c <proc_create_data>
c041c578:	e3500000 	cmp	r0, #0
c041c57c:	0a00001d 	beq	c041c5f8 <npu_cap_setup+0x14c>
c041c580:	e3093484 	tst	r9, #132, 8	@ 0x84000000
c041c584:	e30810d0 	tst	r8, #208	@ 0xd0
c041c588:	e34c3066 	cmp	ip, #102	@ 0x66
c041c58c:	e5930014 	ldr	r0, [r3, #20]
c041c590:	ebf25680 	bl	c00b1f98 <kmem_cache_alloc>
c041c594:	e2503000 	subs	r3, r0, #0
c041c598:	0a00000c 	beq	c041c5d0 <npu_cap_setup+0x124>
c041c59c:	e30cc724 	tst	ip, #36, 14	@ 0x900000
c041c5a0:	e30b1ffc 	tst	fp, #252, 30	@ 0x3f0
c041c5a4:	e30c2728 	tst	ip, #40, 14	@ 0xa00000
c041c5a8:	e34c1041 	cmp	ip, #65	@ 0x41
c041c5ac:	e34cc058 	cmp	ip, #88	@ 0x58
c041c5b0:	e34c2058 	cmp	ip, #88	@ 0x58
c041c5b4:	e5831004 	str	r1, [r3, #4]
c041c5b8:	e3a01004 	mov	r1, #4
c041c5bc:	e583401c 	str	r4, [r3, #28]
c041c5c0:	e583c000 	str	ip, [r3]
c041c5c4:	e583200c 	str	r2, [r3, #12]
c041c5c8:	e5831008 	str	r1, [r3, #8]
c041c5cc:	ebffe00c 	bl	c0414604 <ffe_register_cmd>
c041c5d0:	e3a00000 	mov	r0, #0
c041c5d4:	e28dd008 	add	sp, sp, #8
c041c5d8:	e8bd8010 	pop	{r4, pc}
c041c5dc:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041c5e0:	e3051ddc 	tst	r5, #220, 26	@ 0x3700
c041c5e4:	e34c1049 	cmp	ip, #73	@ 0x49
c041c5e8:	e300234d 	tst	r0, #872415233	@ 0x34000001
c041c5ec:	e34c0058 	cmp	ip, #88	@ 0x58
c041c5f0:	eb00415d 	bl	c042cb6c <printk>
c041c5f4:	eaffffe1 	b	c041c580 <npu_cap_setup+0xd4>
c041c5f8:	e30c0508 	tst	ip, #8, 10	@ 0x2000000
c041c5fc:	e3051ddc 	tst	r5, #220, 26	@ 0x3700
c041c600:	e34c1049 	cmp	ip, #73	@ 0x49
c041c604:	e3a02fd5 	mov	r2, #852	@ 0x354
c041c608:	e34c0058 	cmp	ip, #88	@ 0x58
c041c60c:	eb004156 	bl	c042cb6c <printk>
c041c610:	eaffffda 	b	c041c580 <npu_cap_setup+0xd4>
c041c614:	c0495df0 			@ <UNDEFINED> instruction: 0xc0495df0
