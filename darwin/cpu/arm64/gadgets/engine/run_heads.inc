/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Run heads, in a part of their own behind every other part of the region,
 * so that no established entry moves for them.
 *
 * An epilogue head stands in front of a run the builder recognized: SP-based
 * 64-bit pair loads, 64-bit single loads and at most one ADD SP, SP, #imm,
 * then the RET through X30 that ends the block. The run's own words and the
 * RET's follow the head unchanged; a hint between them that the virtual CPU
 * runs as a NOP, such as AUTIASP, leaves no word. When SP is 16-byte aligned and the TLB
 * maps every byte the head reads in one guest page for reading, the head does
 * the whole run: each load, the final SP and the return, which takes X30 from
 * the register the last pair load left it in rather than from CpuState.
 * Otherwise it dispatches the run's first word, and the run executes, faults
 * and retires exactly as it would without a head.
 *
 * A head is named for its loads and its ADD SP count, which fix where the
 * RET's cell is, so the cell's load waits for no operand word.
 *
 * The page test is transfer_translate's with a span the builder computes: the
 * entry is indexed by the page of the first byte and keyed by the page of the
 * last, so a span that crosses a page, or wraps, can only miss.
 *
 * The first operand word:
 *   bits 12:0   the first byte the head reads, in 8-byte units from SP at
 *               entry, signed
 *   bits 21:13  the last byte it reads, in bytes from the first
 *   bits 33:22  SP at exit, in 16-byte units from SP at entry, signed
 *   bits 48:34  the first load
 *   bits 63:49  the second load
 * then four loads a word, at bits 0, 15, 30 and 45. A load is its address in
 * 8-byte units from the first byte (bits 4:0), then the CpuState indices of
 * Rt (9:5) and Rt2 (14:10). Every load reads 16 bytes and stores Rt2 before
 * Rt, so a single load names its register twice and keeps its own 8 bytes;
 * the builder counts the other 8 in the span. The pair that loads X30 as
 * Rt2 is the last load, so X30 is in x10 when the loads are done.
 */
	.pushsection GADGET_SECTION_RUNS

/* Each element and the RET is a group of two words, behind its
 * execution_count group in a counting build. */
#if UML_GADGET_EXECUTION_HISTOGRAM
	.set Lrun_count_words, 3
#else
	.set Lrun_count_words, 0
#endif

.macro run_epilogue_load word, shift
	ubfx x11, \word, #\shift, #5
	ubfx x12, \word, #(\shift + 5), #5
	ubfx x13, \word, #(\shift + 10), #5
	add x11, x16, x11, lsl #3
	ldp x9, x10, [x11]
	str x10, [_vcpu, x13, lsl #3]
	str x9, [_vcpu, x12, lsl #3]
.endm

.macro run_epilogue loads, adds
	.set Lrun_operand_words\@, 1 + (\loads + 1) / 4
	/* The cell, in words from the first operand word: past the operands,
	 * the elements' groups and the RET's counting group and gadget word. */
	.set Lrun_elements\@, (\loads + \adds) * (Lrun_count_words + 2)
	.set Lrun_cell\@, Lrun_operand_words\@ + Lrun_elements\@ + Lrun_count_words + 1
gadget run_epilogue_\loads\()_\adds
	ldr x8, [_stream]
	ldr x17, [_vcpu, #UML_GADGET_CPU_SP]
	sbfx x9, x8, #0, #13
	ubfx x10, x8, #13, #9
	tst x17, #0xf
	b.ne Lrun_epilogue_fallback\@
	add x16, x17, x9, lsl #3
	add x11, x16, x10
	ubfx x9, x16, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	ldp x12, x13, [x10, #UML_GADGET_TLB_ENTRY_READ_PAGE]
	lsr x11, x11, #UML_GADGET_GUEST_PAGE_SHIFT
	cmp x12, x11
	b.ne Lrun_epilogue_fallback\@
	add x16, x13, x16
	sbfx x15, x8, #22, #12
	add x17, x17, x15, lsl #4
	run_epilogue_load x8, 34
	.if \loads > 1
	run_epilogue_load x8, 49
	.endif
	.if \loads > 2
	ldr x14, [_stream, #8]
	run_epilogue_load x14, 0
	.endif
	.if \loads > 3
	run_epilogue_load x14, 15
	.endif
	.if \loads > 4
	run_epilogue_load x14, 30
	.endif
	.if \loads > 5
	run_epilogue_load x14, 45
	.endif
	.if \loads > 6
	ldr x14, [_stream, #16]
	run_epilogue_load x14, 0
	.endif
	.if \loads > 7
	run_epilogue_load x14, 15
	.endif
	str x17, [_vcpu, #UML_GADGET_CPU_SP]
	ldr x9, [_stream, #(8 * Lrun_cell\@)]
	b Lreturn_x30_follow
Lrun_epilogue_fallback\@:
	/* Past the operand words, to the run's first word: in a counting
	 * build its execution_count group, which names the head as the
	 * first word's predecessor, so the census counts the fallbacks. */
	add _stream, _stream, #(8 * Lrun_operand_words\@)
	next_gadget
.endm

	.irp loads, 1, 2, 3, 4, 5, 6, 7, 8
	run_epilogue \loads, 0
	run_epilogue \loads, 1
	.endr

.purgem run_epilogue
.purgem run_epilogue_load

	.popsection
