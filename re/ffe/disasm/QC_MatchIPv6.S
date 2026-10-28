
/tmp/vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c040d228 <CSPKernel_QC_MatchIPv6>:
c040d228:	e92d43f0 	push	{r4, r5, r6, r7, r8, r9, lr}
c040d22c:	e24dd034 	sub	sp, sp, #52	@ 0x34
c040d230:	e591301c 	ldr	r3, [r1, #28]
c040d234:	e3530000 	cmp	r3, #0
c040d238:	13520000 	cmpne	r2, #0
c040d23c:	0a000003 	beq	c040d250 <CSPKernel_QC_MatchIPv6+0x28>
c040d240:	e3500007 	cmp	r0, #7
c040d244:	0a00002b 	beq	c040d2f8 <CSPKernel_QC_MatchIPv6+0xd0>
c040d248:	e3500008 	cmp	r0, #8
c040d24c:	0a000002 	beq	c040d25c <CSPKernel_QC_MatchIPv6+0x34>
c040d250:	e3a00001 	mov	r0, #1
c040d254:	e28dd034 	add	sp, sp, #52	@ 0x34
c040d258:	e8bd83f0 	pop	{r4, r5, r6, r7, r8, r9, pc}
c040d25c:	e5b30008 	ldr	r0, [r3, #8]!
c040d260:	e1a0c002 	mov	ip, r2
c040d264:	e1a09002 	mov	r9, r2
c040d268:	e1a0600d 	mov	r6, sp
c040d26c:	e28d8010 	add	r8, sp, #16
c040d270:	e28d7020 	add	r7, sp, #32
c040d274:	e5931004 	ldr	r1, [r3, #4]
c040d278:	e1a0400d 	mov	r4, sp
c040d27c:	e5932008 	ldr	r2, [r3, #8]
c040d280:	e1a05008 	mov	r5, r8
c040d284:	e593300c 	ldr	r3, [r3, #12]
c040d288:	e1a0e007 	mov	lr, r7
c040d28c:	e8a6000f 	stmia	r6!, {r0, r1, r2, r3}
c040d290:	e5bc0030 	ldr	r0, [ip, #48]!	@ 0x30
c040d294:	e59c1004 	ldr	r1, [ip, #4]
c040d298:	e59c2008 	ldr	r2, [ip, #8]
c040d29c:	e59c300c 	ldr	r3, [ip, #12]
c040d2a0:	e8a5000f 	stmia	r5!, {r0, r1, r2, r3}
c040d2a4:	e5b90040 	ldr	r0, [r9, #64]!	@ 0x40
c040d2a8:	e5991004 	ldr	r1, [r9, #4]
c040d2ac:	e5992008 	ldr	r2, [r9, #8]
c040d2b0:	e599300c 	ldr	r3, [r9, #12]
c040d2b4:	e8ae000f 	stmia	lr!, {r0, r1, r2, r3}
c040d2b8:	e1a01008 	mov	r1, r8
c040d2bc:	e1a00004 	mov	r0, r4
c040d2c0:	e3a02010 	mov	r2, #16
c040d2c4:	ebf5c8e5 	bl	c017f660 <memcmp>
c040d2c8:	e1a01007 	mov	r1, r7
c040d2cc:	e3a02010 	mov	r2, #16
c040d2d0:	e1a05000 	mov	r5, r0
c040d2d4:	e1a00004 	mov	r0, r4
c040d2d8:	ebf5c8e0 	bl	c017f660 <memcmp>
c040d2dc:	e3550000 	cmp	r5, #0
c040d2e0:	e1e00000 	mvn	r0, r0
c040d2e4:	e1a00fa0 	lsr	r0, r0, #31
c040d2e8:	c3a00000 	movgt	r0, #0
c040d2ec:	e2200001 	eor	r0, r0, #1
c040d2f0:	e28dd034 	add	sp, sp, #52	@ 0x34
c040d2f4:	e8bd83f0 	pop	{r4, r5, r6, r7, r8, r9, pc}
c040d2f8:	e5b30018 	ldr	r0, [r3, #24]!
c040d2fc:	e1a0c002 	mov	ip, r2
c040d300:	e1a09002 	mov	r9, r2
c040d304:	e1a0600d 	mov	r6, sp
c040d308:	e28d8010 	add	r8, sp, #16
c040d30c:	e28d7020 	add	r7, sp, #32
c040d310:	e5931004 	ldr	r1, [r3, #4]
c040d314:	e1a0400d 	mov	r4, sp
c040d318:	e5932008 	ldr	r2, [r3, #8]
c040d31c:	e1a05008 	mov	r5, r8
c040d320:	e593300c 	ldr	r3, [r3, #12]
c040d324:	e1a0e007 	mov	lr, r7
c040d328:	e8a6000f 	stmia	r6!, {r0, r1, r2, r3}
c040d32c:	e5bc0010 	ldr	r0, [ip, #16]!
c040d330:	e59c1004 	ldr	r1, [ip, #4]
c040d334:	e59c2008 	ldr	r2, [ip, #8]
c040d338:	e59c300c 	ldr	r3, [ip, #12]
c040d33c:	e8a5000f 	stmia	r5!, {r0, r1, r2, r3}
c040d340:	e5b90020 	ldr	r0, [r9, #32]!
c040d344:	e5991004 	ldr	r1, [r9, #4]
c040d348:	e5992008 	ldr	r2, [r9, #8]
c040d34c:	e599300c 	ldr	r3, [r9, #12]
c040d350:	e8ae000f 	stmia	lr!, {r0, r1, r2, r3}
c040d354:	eaffffd7 	b	c040d2b8 <CSPKernel_QC_MatchIPv6+0x90>
