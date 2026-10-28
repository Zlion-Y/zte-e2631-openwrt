
/tmp/vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c0607170 <init_ipv6_packet_layer>:
c0607170:	e92d49f8 	push	{r3, r4, r5, r6, r7, r8, fp, lr}
c0607174:	e28db01c 	add	fp, sp, #28
c0607178:	e24dd060 	sub	sp, sp, #96	@ 0x60
c060717c:	e3a01040 	mov	r1, #64	@ 0x40
c0607180:	e28d401f 	add	r4, sp, #31
c0607184:	e30686c8 	tst	r6, #200, 12	@ 0xc800000
c0607188:	e3c4401f 	bic	r4, r4, #31
c060718c:	e30879a4 	tst	r8, #164, 18	@ 0x290000
c0607190:	e3086804 	tst	r8, #4, 16	@ 0x40000
c0607194:	e306568c 	tst	r6, #140, 12	@ 0x8c00000
c0607198:	e1a00004 	mov	r0, r4
c060719c:	e34c8043 	cmp	ip, #67	@ 0x43
c06071a0:	ebedc636 	bl	c0178a80 <__memzero>
c06071a4:	e30b3840 	tst	fp, #64, 16	@ 0x400000
c06071a8:	e34c307a 	cmp	ip, #122	@ 0x7a
c06071ac:	e308e618 	tst	r8, #24, 12	@ 0x1800000
c06071b0:	e5d32057 	ldrb	r2, [r3, #87]	@ 0x57
c06071b4:	e308c318 	tst	r8, #24, 6	@ 0x60000000
c06071b8:	e3080590 	tst	r8, #144, 10	@ 0x24000000
c06071bc:	e30814cc 	tst	r8, #204, 8	@ 0xcc000000
c06071c0:	e3520010 	cmp	r2, #16
c06071c4:	e3082320 	tst	r8, #32, 6	@ 0x80000000
c06071c8:	e34c1041 	cmp	ip, #65	@ 0x41
c06071cc:	e34c2041 	cmp	ip, #65	@ 0x41
c06071d0:	e34c7041 	cmp	ip, #65	@ 0x41
c06071d4:	e34c6041 	cmp	ip, #65	@ 0x41
c06071d8:	e34c5043 	cmp	ip, #67	@ 0x43
c06071dc:	e34ce041 	cmp	ip, #65	@ 0x41
c06071e0:	e34cc041 	cmp	ip, #65	@ 0x41
c06071e4:	e34c0041 	cmp	ip, #65	@ 0x41
c06071e8:	e5841020 	str	r1, [r4, #32]
c06071ec:	e3a0102c 	mov	r1, #44	@ 0x2c
c06071f0:	e5842024 	str	r2, [r4, #36]	@ 0x24
c06071f4:	e3a02007 	mov	r2, #7
c06071f8:	e5848004 	str	r8, [r4, #4]
c06071fc:	e5847008 	str	r7, [r4, #8]
c0607200:	e584600c 	str	r6, [r4, #12]
c0607204:	e5845010 	str	r5, [r4, #16]
c0607208:	e584e014 	str	lr, [r4, #20]
c060720c:	e584c018 	str	ip, [r4, #24]
c0607210:	e584001c 	str	r0, [r4, #28]
c0607214:	e5841028 	str	r1, [r4, #40]	@ 0x28
c0607218:	e5842000 	str	r2, [r4]
c060721c:	1a000003 	bne	c0607230 <init_ipv6_packet_layer+0xc0>
c0607220:	e5d31163 	ldrb	r1, [r3, #355]	@ 0x163
c0607224:	e5c32057 	strb	r2, [r3, #87]	@ 0x57
c0607228:	e3510010 	cmp	r1, #16
c060722c:	0a000000 	beq	c0607234 <init_ipv6_packet_layer+0xc4>
c0607230:	e7f001f2 	udf	#18
c0607234:	e1a00004 	mov	r0, r4
c0607238:	e5c32163 	strb	r2, [r3, #355]	@ 0x163
c060723c:	ebf82a2c 	bl	c0411af4 <ffe_register_packet_layer>
c0607240:	e24bd01c 	sub	sp, fp, #28
c0607244:	e8bd89f8 	pop	{r3, r4, r5, r6, r7, r8, fp, pc}

c0607248 <init_ipv6_tcp_proto>:
c0607248:	e30d3c20 	tst	sp, #32, 24	@ 0x2000
c060724c:	e3a02b01 	mov	r2, #1024	@ 0x400
c0607250:	e34c3065 	cmp	ip, #101	@ 0x65
c0607254:	e1a00003 	mov	r0, r3
c0607258:	e5832020 	str	r2, [r3, #32]
c060725c:	eaf82a61 	b	c0411be8 <ffe_register_proto>

c0607260 <init_ipv6_udp_proto>:
c0607260:	e30d3ca0 	tst	sp, #160, 24	@ 0xa000
c0607264:	e3a02b01 	mov	r2, #1024	@ 0x400
c0607268:	e34c3065 	cmp	ip, #101	@ 0x65
c060726c:	e1a00003 	mov	r0, r3
c0607270:	e5832020 	str	r2, [r3, #32]
c0607274:	eaf82a5b 	b	c0411be8 <ffe_register_proto>

c0607278 <init_ipv6_raw_proto>:
c0607278:	e30d3d20 	tst	sp, #32, 26	@ 0x800
c060727c:	e3a02010 	mov	r2, #16
c0607280:	e34c3065 	cmp	ip, #101	@ 0x65
c0607284:	e1a00003 	mov	r0, r3
c0607288:	e5832020 	str	r2, [r3, #32]
c060728c:	eaf82a55 	b	c0411be8 <ffe_register_proto>

c0607290 <init_ipv4_multicast_proto>:
c0607290:	e92d4038 	push	{r3, r4, r5, lr}
c0607294:	e30d4da0 	tst	sp, #160, 26	@ 0x2800
c0607298:	e34c4065 	cmp	ip, #101	@ 0x65
c060729c:	e3a03010 	mov	r3, #16
c06072a0:	e1a00004 	mov	r0, r4
c06072a4:	e5843020 	str	r3, [r4, #32]
c06072a8:	ebf82a4e 	bl	c0411be8 <ffe_register_proto>
c06072ac:	e2505000 	subs	r5, r0, #0
c06072b0:	1a000001 	bne	c06072bc <init_ipv4_multicast_proto+0x2c>
c06072b4:	e2840080 	add	r0, r4, #128	@ 0x80
c06072b8:	ebf834d1 	bl	c0414604 <ffe_register_cmd>
c06072bc:	e1a00005 	mov	r0, r5
c06072c0:	e8bd8038 	pop	{r3, r4, r5, pc}

c06072c4 <init_ipv6_multicast_proto>:
c06072c4:	e30d3e40 	tst	sp, #64, 28	@ 0x400
c06072c8:	e3a02010 	mov	r2, #16
c06072cc:	e34c3065 	cmp	ip, #101	@ 0x65
c06072d0:	e1a00003 	mov	r0, r3
c06072d4:	e5832020 	str	r2, [r3, #32]
c06072d8:	eaf82a42 	b	c0411be8 <ffe_register_proto>

c06072dc <init_ffe_qos>:
c06072dc:	e92d4010 	push	{r4, lr}
c06072e0:	e30d4ec0 	tst	sp, #192, 28	@ 0xc00
c06072e4:	e34c4065 	cmp	ip, #101	@ 0x65
c06072e8:	e1a00004 	mov	r0, r4
c06072ec:	ebf82488 	bl	c0410514 <ffe_register_notifier>
c06072f0:	e284000c 	add	r0, r4, #12
c06072f4:	ebf817bc 	bl	c040d1ec <qos_register_notifier>
c06072f8:	e3a00000 	mov	r0, #0
c06072fc:	e8bd8010 	pop	{r4, pc}

c0607300 <npu_main_init>:
c0607300:	e30b1a5c 	tst	fp, #92, 20	@ 0x5c000
c0607304:	e3a03000 	mov	r3, #0
c0607308:	e34c107a 	cmp	ip, #122	@ 0x7a
c060730c:	e0812003 	add	r2, r1, r3
c0607310:	e7832001 	str	r2, [r3, r1]
c0607314:	e2833008 	add	r3, r3, #8
c0607318:	e3530038 	cmp	r3, #56	@ 0x38
c060731c:	e5822004 	str	r2, [r2, #4]
c0607320:	1afffff9 	bne	c060730c <npu_main_init+0xc>
c0607324:	e59f301c 	ldr	r3, [pc, #28]	@ c0607348 <npu_main_init+0x48>
c0607328:	e2832038 	add	r2, r3, #56	@ 0x38
c060732c:	e5833000 	str	r3, [r3]
c0607330:	e5833004 	str	r3, [r3, #4]
c0607334:	e2833008 	add	r3, r3, #8
c0607338:	e1530002 	cmp	r3, r2
c060733c:	1afffffa 	bne	c060732c <npu_main_init+0x2c>
c0607340:	e3a00000 	mov	r0, #0
c0607344:	e12fff1e 	msr	SP_hyp, lr, lsl pc
c0607348:	c07aba94 			@ <UNDEFINED> instruction: 0xc07aba94

c060734c <ffe_backlog_init>:
c060734c:	e30b3ad0 	tst	fp, #208, 20	@ 0xd0000
c0607350:	e3a02000 	mov	r2, #0
c0607354:	e34c307a 	cmp	ip, #122	@ 0x7a
c0607358:	e30c1c00 	tst	ip, #0, 24
c060735c:	e283c010 	add	ip, r3, #16
c0607360:	e34c1041 	cmp	ip, #65	@ 0x41
c0607364:	e3a00040 	mov	r0, #64	@ 0x40
c0607368:	e5831024 	str	r1, [r3, #36]	@ 0x24
c060736c:	e583001c 	str	r0, [r3, #28]
c0607370:	e2831068 	add	r1, r3, #104	@ 0x68
c0607374:	e5833000 	str	r3, [r3]
c0607378:	e1a00002 	mov	r0, r2
c060737c:	e5833004 	str	r3, [r3, #4]
c0607380:	e583c010 	str	ip, [r3, #16]
c0607384:	e583c014 	str	ip, [r3, #20]
c0607388:	e5831068 	str	r1, [r3, #104]	@ 0x68
c060738c:	e583106c 	str	r1, [r3, #108]	@ 0x6c
c0607390:	e583200c 	str	r2, [r3, #12]
c0607394:	e5832008 	str	r2, [r3, #8]
c0607398:	e12fff1e 	msr	SP_hyp, lr, lsl pc

c060739c <ffe_rps_init>:
c060739c:	e30839e0 	tst	r8, #224, 18	@ 0x380000
c06073a0:	e3e00000 	mvn	r0, #0
c06073a4:	e34c3043 	cmp	ip, #67	@ 0x43
c06073a8:	e92d47f0 	push	{r4, r5, r6, r7, r8, r9, sl, lr}
c06073ac:	e30b7384 	tst	fp, #132, 6	@ 0x10000002
c06073b0:	e5936000 	ldr	r6, [r3]
c06073b4:	e30b843c 	tst	fp, #60, 8	@ 0x3c000000
c06073b8:	e24dd008 	sub	sp, sp, #8
c06073bc:	e3075aa0 	tst	r7, #160, 20	@ 0xa0000
c06073c0:	e34c7061 	cmp	ip, #97	@ 0x61
c06073c4:	e34c8061 	cmp	ip, #97	@ 0x61
c06073c8:	e34c5061 	cmp	ip, #97	@ 0x61
c06073cc:	e2802001 	add	r2, r0, #1
c06073d0:	e3a01002 	mov	r1, #2
c06073d4:	e1a00006 	mov	r0, r6
c06073d8:	e30b4384 	tst	fp, #132, 6	@ 0x10000002
c06073dc:	ebedc125 	bl	c0177878 <_find_next_bit_le>
c06073e0:	e5973000 	ldr	r3, [r7]
c06073e4:	e30cce24 	tst	ip, #36, 28	@ 0x240
c06073e8:	e30c1d14 	tst	ip, #20, 26	@ 0x500
c06073ec:	e34c4061 	cmp	ip, #97	@ 0x61
c06073f0:	e3a02000 	mov	r2, #0
c06073f4:	e34cc041 	cmp	ip, #65	@ 0x41
c06073f8:	e34c1041 	cmp	ip, #65	@ 0x41
c06073fc:	e1530000 	cmp	r3, r0
c0607400:	da000014 	ble	c0607458 <ffe_rps_init+0xbc>
c0607404:	e7989100 	ldr	r9, [r8, r0, lsl #2]
c0607408:	e3a0a040 	mov	sl, #64	@ 0x40
c060740c:	e0853009 	add	r3, r5, r9
c0607410:	e2834010 	add	r4, r3, #16
c0607414:	e283e050 	add	lr, r3, #80	@ 0x50
c0607418:	e5830020 	str	r0, [r3, #32]
c060741c:	e583200c 	str	r2, [r3, #12]
c0607420:	e7853009 	str	r3, [r5, r9]
c0607424:	e5833004 	str	r3, [r3, #4]
c0607428:	e5832008 	str	r2, [r3, #8]
c060742c:	e583201c 	str	r2, [r3, #28]
c0607430:	e5832018 	str	r2, [r3, #24]
c0607434:	e583c044 	str	ip, [r3, #68]	@ 0x44
c0607438:	e5833048 	str	r3, [r3, #72]	@ 0x48
c060743c:	e5831064 	str	r1, [r3, #100]	@ 0x64
c0607440:	e583a05c 	str	sl, [r3, #92]	@ 0x5c
c0607444:	e5834010 	str	r4, [r3, #16]
c0607448:	e5834014 	str	r4, [r3, #20]
c060744c:	e583e050 	str	lr, [r3, #80]	@ 0x50
c0607450:	e583e054 	str	lr, [r3, #84]	@ 0x54
c0607454:	eaffffdc 	b	c06073cc <ffe_rps_init+0x30>
c0607458:	e30b5b58 	tst	fp, #88, 22	@ 0x16000
c060745c:	e3a01010 	mov	r1, #16
c0607460:	e34c507a 	cmp	ip, #122	@ 0x7a
c0607464:	e2850004 	add	r0, r5, #4
c0607468:	ebedc584 	bl	c0178a80 <__memzero>
c060746c:	e285600e 	add	r6, r5, #14
c0607470:	e30839dc 	tst	r8, #220, 18	@ 0x370000
c0607474:	e3a09000 	mov	r9, #0
c0607478:	e34c3043 	cmp	ip, #67	@ 0x43
c060747c:	e3e00000 	mvn	r0, #0
c0607480:	e3a02b01 	mov	r2, #1024	@ 0x400
c0607484:	e5937000 	ldr	r7, [r3]
c0607488:	e5852004 	str	r2, [r5, #4]
c060748c:	e3a03003 	mov	r3, #3
c0607490:	e28d8008 	add	r8, sp, #8
c0607494:	e5283004 	str	r3, [r8, #-4]!
c0607498:	e1a01008 	mov	r1, r8
c060749c:	e1a02007 	mov	r2, r7
c06074a0:	ebedc816 	bl	c0179500 <cpumask_next_and>
c06074a4:	e5943000 	ldr	r3, [r4]
c06074a8:	e1500003 	cmp	r0, r3
c06074ac:	aa000006 	bge	c06074cc <ffe_rps_init+0x130>
c06074b0:	e1e600b2 	strh	r0, [r6, #2]!
c06074b4:	e2899001 	add	r9, r9, #1
c06074b8:	e5953008 	ldr	r3, [r5, #8]
c06074bc:	e2833001 	add	r3, r3, #1
c06074c0:	e0833000 	add	r3, r3, r0
c06074c4:	e5853008 	str	r3, [r5, #8]
c06074c8:	eafffff2 	b	c0607498 <ffe_rps_init+0xfc>
c06074cc:	e30e001c 	tst	lr, #28
c06074d0:	e3094484 	tst	r9, #132, 8	@ 0x84000000
c06074d4:	e34c0065 	cmp	ip, #101	@ 0x65
c06074d8:	e34c4066 	cmp	ip, #102	@ 0x66
c06074dc:	e585900c 	str	r9, [r5, #12]
c06074e0:	ebf2ff7d 	bl	c02c72dc <register_netdevice_notifier>
c06074e4:	e5940014 	ldr	r0, [r4, #20]
c06074e8:	e30810d0 	tst	r8, #208	@ 0xd0
c06074ec:	ebeaaaa9 	bl	c00b1f98 <kmem_cache_alloc>
c06074f0:	e2503000 	subs	r3, r0, #0
c06074f4:	0a00001b 	beq	c0607568 <ffe_rps_init+0x1cc>
c06074f8:	e30cc888 	tst	ip, #136, 16	@ 0x880000
c06074fc:	e30d1088 	tst	sp, #136	@ 0x88
c0607500:	e30c2894 	tst	ip, #148, 16	@ 0x940000
c0607504:	e34cc058 	cmp	ip, #88	@ 0x58
c0607508:	e34c1041 	cmp	ip, #65	@ 0x41
c060750c:	e583c000 	str	ip, [r3]
c0607510:	e5831004 	str	r1, [r3, #4]
c0607514:	e34c2058 	cmp	ip, #88	@ 0x58
c0607518:	e3a05001 	mov	r5, #1
c060751c:	e583200c 	str	r2, [r3, #12]
c0607520:	e5835008 	str	r5, [r3, #8]
c0607524:	ebf83436 	bl	c0414604 <ffe_register_cmd>
c0607528:	e5940014 	ldr	r0, [r4, #20]
c060752c:	e30810d0 	tst	r8, #208	@ 0xd0
c0607530:	ebeaaa98 	bl	c00b1f98 <kmem_cache_alloc>
c0607534:	e2503000 	subs	r3, r0, #0
c0607538:	0a00000a 	beq	c0607568 <ffe_rps_init+0x1cc>
c060753c:	e30cc8cc 	tst	ip, #204, 16	@ 0xcc0000
c0607540:	e30c1ffc 	tst	ip, #252, 30	@ 0x3f0
c0607544:	e30c28e0 	tst	ip, #224, 16	@ 0xe00000
c0607548:	e34cc058 	cmp	ip, #88	@ 0x58
c060754c:	e34c1041 	cmp	ip, #65	@ 0x41
c0607550:	e34c2058 	cmp	ip, #88	@ 0x58
c0607554:	e5835008 	str	r5, [r3, #8]
c0607558:	e583c000 	str	ip, [r3]
c060755c:	e5831004 	str	r1, [r3, #4]
c0607560:	e583200c 	str	r2, [r3, #12]
c0607564:	ebf83426 	bl	c0414604 <ffe_register_cmd>
c0607568:	e3a00000 	mov	r0, #0
c060756c:	e28dd008 	add	sp, sp, #8
c0607570:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}

c0607574 <mirror_init>:
c0607574:	e92d4038 	push	{r3, r4, r5, lr}
c0607578:	e30d196c 	tst	sp, #108, 18	@ 0x1b0000
c060757c:	e30b3b6c 	tst	fp, #108, 22	@ 0x1b000
c0607580:	e34c1041 	cmp	ip, #65	@ 0x41
c0607584:	e34c307a 	cmp	ip, #122	@ 0x7a
c0607588:	e3a0000f 	mov	r0, #15
c060758c:	e30e4028 	tst	lr, #40	@ 0x28
c0607590:	e283c008 	add	ip, r3, #8
c0607594:	e2832010 	add	r2, r3, #16
c0607598:	e583c008 	str	ip, [r3, #8]
c060759c:	e583c00c 	str	ip, [r3, #12]
c06075a0:	e3a05000 	mov	r5, #0
c06075a4:	e5832010 	str	r2, [r3, #16]
c06075a8:	e34c4065 	cmp	ip, #101	@ 0x65
c06075ac:	e5832014 	str	r2, [r3, #20]
c06075b0:	e5835004 	str	r5, [r3, #4]
c06075b4:	ebf76b0a 	bl	c03e21e4 <register_ioctl_handler>
c06075b8:	e2840d11 	add	r0, r4, #1088	@ 0x440
c06075bc:	ebf2ff46 	bl	c02c72dc <register_netdevice_notifier>
c06075c0:	e2840d11 	add	r0, r4, #1088	@ 0x440
c06075c4:	e280000c 	add	r0, r0, #12
c06075c8:	ebf79113 	bl	c03eba1c <register_netif_flag_notifier>
c06075cc:	e1a00005 	mov	r0, r5
c06075d0:	e8bd8038 	pop	{r3, r4, r5, pc}

c06075d4 <brSimulationSkbInit>:
c06075d4:	e92d4008 	push	{r3, lr}
c06075d8:	ebf8614a 	bl	c041fb08 <brSimulationSkbNfOpsInit>
c06075dc:	e30e1f5c 	tst	lr, #92, 30	@ 0x170
c06075e0:	e34c1041 	cmp	ip, #65	@ 0x41
c06075e4:	e3a00012 	mov	r0, #18
c06075e8:	ebf76afd 	bl	c03e21e4 <register_ioctl_handler>
c06075ec:	e3a00000 	mov	r0, #0
c06075f0:	e8bd8008 	pop	{r3, pc}

c06075f4 <CSP_PPP_Init>:
c06075f4:	e30b3cf0 	tst	fp, #240, 24	@ 0xf000
c06075f8:	e3a02000 	mov	r2, #0
c06075fc:	e34c307a 	cmp	ip, #122	@ 0x7a
