
/tmp/vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c041be8c <npu_cap_nat6_front_create>:
c041be8c:	e92d4010 	push	{r4, lr}
c041be90:	e24dd020 	sub	sp, sp, #32
c041be94:	e1a04001 	mov	r4, r1
c041be98:	e3a01010 	mov	r1, #16
c041be9c:	e1a0000d 	mov	r0, sp
c041bea0:	ebf572f6 	bl	c0178a80 <__memzero>
c041bea4:	e3a01010 	mov	r1, #16
c041bea8:	e28d0010 	add	r0, sp, #16
c041beac:	ebf572f3 	bl	c0178a80 <__memzero>
c041beb0:	e5943028 	ldr	r3, [r4, #40]	@ 0x28
c041beb4:	e3130c07 	tst	r3, #1792	@ 0x700
c041beb8:	0a000038 	beq	c041bfa0 <npu_cap_nat6_front_create+0x114>
c041bebc:	e5942010 	ldr	r2, [r4, #16]
c041bec0:	e5d23007 	ldrb	r3, [r2, #7]
c041bec4:	e0821003 	add	r1, r2, r3
c041bec8:	e2810018 	add	r0, r1, #24
c041becc:	e58d0004 	str	r0, [sp, #4]
c041bed0:	e5d1c006 	ldrb	ip, [r1, #6]
c041bed4:	e2810008 	add	r0, r1, #8
c041bed8:	e58d0000 	str	r0, [sp]
c041bedc:	e35c0006 	cmp	ip, #6
c041bee0:	e5cdc00c 	strb	ip, [sp, #12]
c041bee4:	e7d20003 	ldrb	r0, [r2, r3]
c041bee8:	e5d13001 	ldrb	r3, [r1, #1]
c041beec:	e200100f 	and	r1, r0, #15
c041bef0:	e1a03223 	lsr	r3, r3, #4
c041bef4:	e1833201 	orr	r3, r3, r1, lsl #4
c041bef8:	e5cd300d 	strb	r3, [sp, #13]
c041befc:	05d23009 	ldrbeq	r3, [r2, #9]
c041bf00:	0a00002a 	beq	c041bfb0 <npu_cap_nat6_front_create+0x124>
c041bf04:	e35c0011 	cmp	ip, #17
c041bf08:	13a03000 	movne	r3, #0
c041bf0c:	11cd30b8 	strhne	r3, [sp, #8]
c041bf10:	11cd30ba 	strhne	r3, [sp, #10]
c041bf14:	0a000024 	beq	c041bfac <npu_cap_nat6_front_create+0x120>
c041bf18:	e5942014 	ldr	r2, [r4, #20]
c041bf1c:	e5d23007 	ldrb	r3, [r2, #7]
c041bf20:	e0820003 	add	r0, r2, r3
c041bf24:	e2801018 	add	r1, r0, #24
c041bf28:	e58d1014 	str	r1, [sp, #20]
c041bf2c:	e5d0e006 	ldrb	lr, [r0, #6]
c041bf30:	e280c008 	add	ip, r0, #8
c041bf34:	e58dc010 	str	ip, [sp, #16]
c041bf38:	e35e0006 	cmp	lr, #6
c041bf3c:	e5cde01c 	strb	lr, [sp, #28]
c041bf40:	e7d2c003 	ldrb	ip, [r2, r3]
c041bf44:	e5d03001 	ldrb	r3, [r0, #1]
c041bf48:	e20c000f 	and	r0, ip, #15
c041bf4c:	e1a03223 	lsr	r3, r3, #4
c041bf50:	e1833200 	orr	r3, r3, r0, lsl #4
c041bf54:	e5cd301d 	strb	r3, [sp, #29]
c041bf58:	05d23009 	ldrbeq	r3, [r2, #9]
c041bf5c:	0a000020 	beq	c041bfe4 <npu_cap_nat6_front_create+0x158>
c041bf60:	e35e0011 	cmp	lr, #17
c041bf64:	13a03000 	movne	r3, #0
c041bf68:	11cd31b8 	strhne	r3, [sp, #24]
c041bf6c:	11cd31ba 	strhne	r3, [sp, #26]
c041bf70:	0a00001a 	beq	c041bfe0 <npu_cap_nat6_front_create+0x154>
c041bf74:	e59d0004 	ldr	r0, [sp, #4]
c041bf78:	e3a02010 	mov	r2, #16
c041bf7c:	ebf58db7 	bl	c017f660 <memcmp>
c041bf80:	e3500000 	cmp	r0, #0
c041bf84:	1a00000f 	bne	c041bfc8 <npu_cap_nat6_front_create+0x13c>
c041bf88:	e59d0000 	ldr	r0, [sp]
c041bf8c:	e3a02010 	mov	r2, #16
c041bf90:	e59d1010 	ldr	r1, [sp, #16]
c041bf94:	ebf58db1 	bl	c017f660 <memcmp>
c041bf98:	e3500000 	cmp	r0, #0
c041bf9c:	1a000009 	bne	c041bfc8 <npu_cap_nat6_front_create+0x13c>
c041bfa0:	e3a00000 	mov	r0, #0
c041bfa4:	e28dd020 	add	sp, sp, #32
c041bfa8:	e8bd8010 	pop	{r4, pc}
c041bfac:	e5d2300a 	ldrb	r3, [r2, #10]
c041bfb0:	e0821003 	add	r1, r2, r3
c041bfb4:	e19230b3 	ldrh	r3, [r2, r3]
c041bfb8:	e1cd30b8 	strh	r3, [sp, #8]
c041bfbc:	e1d130b2 	ldrh	r3, [r1, #2]
c041bfc0:	e1cd30ba 	strh	r3, [sp, #10]
c041bfc4:	eaffffd3 	b	c041bf18 <npu_cap_nat6_front_create+0x8c>
c041bfc8:	e594307c 	ldr	r3, [r4, #124]	@ 0x7c
c041bfcc:	e3a00001 	mov	r0, #1
c041bfd0:	e3c33001 	bic	r3, r3, #1
c041bfd4:	e584307c 	str	r3, [r4, #124]	@ 0x7c
c041bfd8:	e28dd020 	add	sp, sp, #32
c041bfdc:	e8bd8010 	pop	{r4, pc}
c041bfe0:	e5d2300a 	ldrb	r3, [r2, #10]
c041bfe4:	e0820003 	add	r0, r2, r3
c041bfe8:	e19230b3 	ldrh	r3, [r2, r3]
c041bfec:	e1cd31b8 	strh	r3, [sp, #24]
c041bff0:	e1d030b2 	ldrh	r3, [r0, #2]
c041bff4:	e1cd31ba 	strh	r3, [sp, #26]
c041bff8:	eaffffdd 	b	c041bf74 <npu_cap_nat6_front_create+0xe8>
