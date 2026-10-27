
vmlinux-csp.elf:     file format elf32-littlearm


Disassembly of section .kernel:

c0410ed0 <ffe_general_alloc_flow>:
c0410ed0:	e92d47f0 	push	{r4, r5, r6, r7, r8, r9, sl, lr}
c0410ed4:	e30b5838 	tst	fp, #56, 16	@ 0x380000
c0410ed8:	e34c507a 	cmp	ip, #122	@ 0x7a
c0410edc:	e1a06000 	mov	r6, r0
c0410ee0:	e5953000 	ldr	r3, [r5]
c0410ee4:	e3081020 	tst	r8, #32
c0410ee8:	e1a09002 	mov	r9, r2
c0410eec:	e593004c 	ldr	r0, [r3, #76]	@ 0x4c
c0410ef0:	ebf28428 	bl	c00b1f98 <kmem_cache_alloc>
c0410ef4:	e2504000 	subs	r4, r0, #0
c0410ef8:	0a000080 	beq	c0411100 <ffe_general_alloc_flow+0x230>
c0410efc:	e5953000 	ldr	r3, [r5]
c0410f00:	e3a01020 	mov	r1, #32
c0410f04:	e5930044 	ldr	r0, [r3, #68]	@ 0x44
c0410f08:	eb0008ef 	bl	c04132cc <ffe_sharedata_alloc>
c0410f0c:	e3500000 	cmp	r0, #0
c0410f10:	e5840024 	str	r0, [r4, #36]	@ 0x24
c0410f14:	0a000049 	beq	c0411040 <ffe_general_alloc_flow+0x170>
c0410f18:	e3a01030 	mov	r1, #48	@ 0x30
c0410f1c:	ebf59ed7 	bl	c0178a80 <__memzero>
c0410f20:	e5953000 	ldr	r3, [r5]
c0410f24:	e3a01020 	mov	r1, #32
c0410f28:	e5930048 	ldr	r0, [r3, #72]	@ 0x48
c0410f2c:	eb0008e6 	bl	c04132cc <ffe_sharedata_alloc>
c0410f30:	e3500000 	cmp	r0, #0
c0410f34:	e5840068 	str	r0, [r4, #104]	@ 0x68
c0410f38:	0a00003c 	beq	c0411030 <ffe_general_alloc_flow+0x160>
c0410f3c:	e3a0105c 	mov	r1, #92	@ 0x5c
c0410f40:	ebf59ece 	bl	c0178a80 <__memzero>
c0410f44:	e5943068 	ldr	r3, [r4, #104]	@ 0x68
c0410f48:	e3560000 	cmp	r6, #0
c0410f4c:	e3a02010 	mov	r2, #16
c0410f50:	e1c324b2 	strh	r2, [r3, #66]	@ 0x42
c0410f54:	0a000005 	beq	c0410f70 <ffe_general_alloc_flow+0xa0>
c0410f58:	e5960018 	ldr	r0, [r6, #24]
c0410f5c:	e3500000 	cmp	r0, #0
c0410f60:	0a000002 	beq	c0410f70 <ffe_general_alloc_flow+0xa0>
c0410f64:	e596301c 	ldr	r3, [r6, #28]
c0410f68:	e3530000 	cmp	r3, #0
c0410f6c:	1a00004e 	bne	c04110ac <ffe_general_alloc_flow+0x1dc>
c0410f70:	e5953000 	ldr	r3, [r5]
c0410f74:	e3081020 	tst	r8, #32
c0410f78:	e30b8838 	tst	fp, #56, 16	@ 0x380000
c0410f7c:	e34c807a 	cmp	ip, #122	@ 0x7a
c0410f80:	e5930050 	ldr	r0, [r3, #80]	@ 0x50
c0410f84:	ebf28403 	bl	c00b1f98 <kmem_cache_alloc>
c0410f88:	e5983000 	ldr	r3, [r8]
c0410f8c:	e2507000 	subs	r7, r0, #0
c0410f90:	0a000023 	beq	c0411024 <ffe_general_alloc_flow+0x154>
c0410f94:	e3081020 	tst	r8, #32
c0410f98:	e5930050 	ldr	r0, [r3, #80]	@ 0x50
c0410f9c:	ebf283fd 	bl	c00b1f98 <kmem_cache_alloc>
c0410fa0:	e250a000 	subs	sl, r0, #0
c0410fa4:	0a000057 	beq	c0411108 <ffe_general_alloc_flow+0x238>
c0410fa8:	e1a00007 	mov	r0, r7
c0410fac:	e3e01000 	mvn	r1, #0
c0410fb0:	ebfffeb1 	bl	c0410a7c <ffe_init_flow_key>
c0410fb4:	e1a0000a 	mov	r0, sl
c0410fb8:	e3e01000 	mvn	r1, #0
c0410fbc:	ebfffeae 	bl	c0410a7c <ffe_init_flow_key>
c0410fc0:	e3590000 	cmp	r9, #0
c0410fc4:	e5847010 	str	r7, [r4, #16]
c0410fc8:	0a000022 	beq	c0411058 <ffe_general_alloc_flow+0x188>
c0410fcc:	e1a01009 	mov	r1, r9
c0410fd0:	e1a00007 	mov	r0, r7
c0410fd4:	ebfffe3c 	bl	c04108cc <ffe_copy_flow_key>
c0410fd8:	e3500000 	cmp	r0, #0
c0410fdc:	0a00001d 	beq	c0411058 <ffe_general_alloc_flow+0x188>
c0410fe0:	e3560000 	cmp	r6, #0
c0410fe4:	0a00004f 	beq	c0411128 <ffe_general_alloc_flow+0x258>
c0410fe8:	e5960018 	ldr	r0, [r6, #24]
c0410fec:	e3500000 	cmp	r0, #0
c0410ff0:	0a00004c 	beq	c0411128 <ffe_general_alloc_flow+0x258>
c0410ff4:	e596301c 	ldr	r3, [r6, #28]
c0410ff8:	e3530000 	cmp	r3, #0
c0410ffc:	0a000049 	beq	c0411128 <ffe_general_alloc_flow+0x258>
c0411000:	e1a0100a 	mov	r1, sl
c0411004:	ebf28508 	bl	c00b242c <kmem_cache_free>
c0411008:	e5960018 	ldr	r0, [r6, #24]
c041100c:	e1a01007 	mov	r1, r7
c0411010:	ebf28505 	bl	c00b242c <kmem_cache_free>
c0411014:	e5953000 	ldr	r3, [r5]
c0411018:	e5932040 	ldr	r2, [r3, #64]	@ 0x40
c041101c:	e2822001 	add	r2, r2, #1
c0411020:	e5832040 	str	r2, [r3, #64]	@ 0x40
c0411024:	e5930048 	ldr	r0, [r3, #72]	@ 0x48
c0411028:	e5941068 	ldr	r1, [r4, #104]	@ 0x68
c041102c:	eb0008b2 	bl	c04132fc <ffe_sharedata_free>
c0411030:	e5953000 	ldr	r3, [r5]
c0411034:	e5941024 	ldr	r1, [r4, #36]	@ 0x24
c0411038:	e5930044 	ldr	r0, [r3, #68]	@ 0x44
c041103c:	eb0008ae 	bl	c04132fc <ffe_sharedata_free>
c0411040:	e5953000 	ldr	r3, [r5]
c0411044:	e1a01004 	mov	r1, r4
c0411048:	e593004c 	ldr	r0, [r3, #76]	@ 0x4c
c041104c:	ebf284f6 	bl	c00b242c <kmem_cache_free>
c0411050:	e3a00000 	mov	r0, #0
c0411054:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}
c0411058:	e30a30a0 	tst	sl, #160	@ 0xa0
c041105c:	e3a02000 	mov	r2, #0
c0411060:	e34c3061 	cmp	ip, #97	@ 0x61
c0411064:	e2841038 	add	r1, r4, #56	@ 0x38
c0411068:	e593e000 	ldr	lr, [r3]
c041106c:	e3a0c401 	mov	ip, #16777216	@ 0x1000000
c0411070:	e5842000 	str	r2, [r4]
c0411074:	e1a00004 	mov	r0, r4
c0411078:	e5953000 	ldr	r3, [r5]
c041107c:	e584a014 	str	sl, [r4, #20]
c0411080:	e2833f55 	add	r3, r3, #340	@ 0x154
c0411084:	e5846034 	str	r6, [r4, #52]	@ 0x34
c0411088:	e584e018 	str	lr, [r4, #24]
c041108c:	e5842004 	str	r2, [r4, #4]
c0411090:	e1c42ab6 	strh	r2, [r4, #166]	@ 0xa6
c0411094:	e5841038 	str	r1, [r4, #56]	@ 0x38
c0411098:	e584103c 	str	r1, [r4, #60]	@ 0x3c
c041109c:	e584c028 	str	ip, [r4, #40]	@ 0x28
c04110a0:	e1d330b0 	ldrh	r3, [r3]
c04110a4:	e1c43ab4 	strh	r3, [r4, #164]	@ 0xa4
c04110a8:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}
c04110ac:	e3081020 	tst	r8, #32
c04110b0:	ebf283b8 	bl	c00b1f98 <kmem_cache_alloc>
c04110b4:	e2507000 	subs	r7, r0, #0
c04110b8:	0a000018 	beq	c0411120 <ffe_general_alloc_flow+0x250>
c04110bc:	e5960018 	ldr	r0, [r6, #24]
c04110c0:	e3081020 	tst	r8, #32
c04110c4:	ebf283b3 	bl	c00b1f98 <kmem_cache_alloc>
c04110c8:	e250a000 	subs	sl, r0, #0
c04110cc:	0a000006 	beq	c04110ec <ffe_general_alloc_flow+0x21c>
c04110d0:	e1a00007 	mov	r0, r7
c04110d4:	e596101c 	ldr	r1, [r6, #28]
c04110d8:	ebfffe67 	bl	c0410a7c <ffe_init_flow_key>
c04110dc:	e1a0000a 	mov	r0, sl
c04110e0:	e596101c 	ldr	r1, [r6, #28]
c04110e4:	ebfffe64 	bl	c0410a7c <ffe_init_flow_key>
c04110e8:	eaffffb4 	b	c0410fc0 <ffe_general_alloc_flow+0xf0>
c04110ec:	e5960018 	ldr	r0, [r6, #24]
c04110f0:	e1a01007 	mov	r1, r7
c04110f4:	ebf284cc 	bl	c00b242c <kmem_cache_free>
c04110f8:	e5953000 	ldr	r3, [r5]
c04110fc:	eaffffc8 	b	c0411024 <ffe_general_alloc_flow+0x154>
c0411100:	e1a00004 	mov	r0, r4
c0411104:	e8bd87f0 	pop	{r4, r5, r6, r7, r8, r9, sl, pc}
c0411108:	e5983000 	ldr	r3, [r8]
c041110c:	e1a01007 	mov	r1, r7
c0411110:	e5930050 	ldr	r0, [r3, #80]	@ 0x50
c0411114:	ebf284c4 	bl	c00b242c <kmem_cache_free>
c0411118:	e5983000 	ldr	r3, [r8]
c041111c:	eaffffc0 	b	c0411024 <ffe_general_alloc_flow+0x154>
c0411120:	e5953000 	ldr	r3, [r5]
c0411124:	eaffffbe 	b	c0411024 <ffe_general_alloc_flow+0x154>
c0411128:	e7f001f2 	udf	#18
