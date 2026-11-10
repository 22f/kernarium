/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 walk probes (guest_x86_64_load_walk_loop.S,
 * guest_x86_64_store_walk_loop.S and their _control_ twins): sixty-four walk
 * blocks, each a 64-bit load (or with WALK_STORE a store) through rbx, then
 * one register-only instruction of eight families, the block's index modulo
 * eight, then the step's loop branch. Every walk block enters the same memory
 * gadget, so the dispatch out of that gadget has eight successors, and the
 * family is the only thing that tells them apart. Everything else a step runs
 * has one successor.
 *
 * An order table filled before the timed loop names the walk block of every
 * step, reached by an indirect jump. With WALK_RANDOM set it holds a
 * xorshift sequence longer than a branch predictor's history reaches, so the
 * memory gadget's successor is unpredictable; without it the blocks run in
 * index order, so the successor is predictable and the control executes
 * exactly the same instructions. The difference between the two is the
 * price of a memory gadget's dispatch mispredicting.
 *
 * The including file defines WALK_RANDOM and WALK_STORE (0 or 1) and includes
 * x86_64_probe_harness.inc first.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
#define WALK_BLOCKS 64
#define WALK_ORDER 16384
#define WALK_VALUE 0x5a5a

.macro walk_family index
.if ((\index) & 7) == 0
	add	r10, r11
.elseif ((\index) & 7) == 1
	sub	r10, r11
.elseif ((\index) & 7) == 2
	xor	r10, r11
.elseif ((\index) & 7) == 3
	or	r10, r11
.elseif ((\index) & 7) == 4
	and	r10, r11
.elseif ((\index) & 7) == 5
	mov	r10, r11
.elseif ((\index) & 7) == 6
	lea	r10, [r10 + r11 * 2]
.else
	neg	r10
.endif
.endm

	probe_start
	lea	r14, [rip + .Lorder]
	lea	r15, [rip + .Lblocks]
	lea	rbx, [rip + .Lbuffer]
	movabs	rax, 0x7f4a7c159e3779b9
	xor	ecx, ecx
.Lfill:
#if WALK_RANDOM
	mov	rdx, rax
	shl	rdx, 13
	xor	rax, rdx
	mov	rdx, rax
	shr	rdx, 7
	xor	rax, rdx
	mov	rdx, rax
	shl	rdx, 17
	xor	rax, rdx
	mov	rdx, rax
	shr	rdx, 32
#else
	mov	edx, ecx
#endif
	and	edx, WALK_BLOCKS - 1
	mov	word ptr [r14 + rcx * 2], dx
	add	ecx, 1
	cmp	ecx, WALK_ORDER
	jb	.Lfill

	mov	qword ptr [rbx], WALK_VALUE
	mov	r9d, WALK_VALUE
	mov	r11d, 3
	xor	r10d, r10d
	mov	r12d, BENCH_ITERATIONS
	xor	ecx, ecx
.Lloop:
	movzx	eax, word ptr [r14 + rcx * 2]
	add	ecx, 1
	and	ecx, WALK_ORDER - 1
	shl	eax, 5
	add	rax, r15
	jmp	rax

.Ldone:
#if WALK_STORE
	cmp	qword ptr [rbx], WALK_VALUE
#else
	cmp	r9, WALK_VALUE
#endif
	jne	.Lfailure
	probe_finish

	/* Thirty-two bytes a walk block. */
	.p2align 5
.Lblocks:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
#if WALK_STORE
	mov	qword ptr [rbx], r9
#else
	mov	r9, qword ptr [rbx]
#endif
	walk_family walk_index
	sub	r12, 1
	jnz	.Lloop
	jmp	.Ldone
	.set walk_index, walk_index + 1
	.endr

	probe_end

	.bss
	.p2align 12
.Lbuffer:
	.space	64
	.p2align 4
.Lorder:
	.space	WALK_ORDER * 2

	.section .note.GNU-stack, "", @progbits
