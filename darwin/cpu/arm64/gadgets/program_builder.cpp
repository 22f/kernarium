// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "afp_twin.hpp"
#include "gadget_selection.hpp"
#include "execution_histogram.hpp"
#include "scalar_forwarding.hpp"

#include "cpu/arm64/floating_controls.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

/* Builder::build() below calls one of these five per guest instruction it
 * translates, and each of them calls on into the gadget selection tables.
 * Before the engine split that whole decode -> select -> emit chain lived in
 * one translation unit and the compiler inlined it freely; separating the
 * families into their own objects put hard call boundaries across the hottest
 * loop in translation and cost measurable score. Interprocedural optimization
 * recovered part of it but left single-core Clang, the most translation-heavy
 * workload, untouched, because ThinLTO imports across modules on a cost model
 * that declines functions this large. Composing them textually here is not a
 * shortcut around that decision: it is the only way to make the inliner see the
 * whole chain rather than leave it to a heuristic that has been measured to
 * decline. Each fragment still owns exactly one decode family. */
/* .ipp rather than .inc: every .inc in this tree is an assembly fragment of the
 * gadget bank, and editors do not treat .inc as C++. */
#include "program_builder_branch_system_impl.ipp"
#include "program_builder_data_processing_impl.ipp"
#include "program_builder_exception_impl.ipp"
#include "program_builder_floating_simd_impl.ipp"
#include "program_builder_load_store_impl.ipp"

namespace uml::cpu::arm64::gadgets {

using namespace selection;
using namespace builder_detail;

DecodeResult builder_detail::implementation_gap(DecodeContext &context)
{
	/* Keep a translated prefix executable. The terminal gadget reports the
	 * first unsupported allocation without charging it as a retired
	 * instruction; instruction_count deliberately remains the number of
	 * successfully translated words.
	 */
	emit(context.words, uml_arm64_gadget_illegal);
	emit(context.words, context.address);
	return DecodeResult::implementation_gap;
}

namespace {

/* Fill in the reserved suffix operand of each inlined conditional branch, now
 * that the block's translated length is final. A taken branch subtracts this
 * from the count its block was precharged, so it is the instructions after the
 * branch itself. */
void resolve_inline_suffixes(ProgramWords &words,
			     const InlineSuffixes &suffixes,
			     std::size_t compiled) noexcept
{
	for (std::uint32_t index = 0; index < suffixes.count; ++index) {
		const auto translated = suffixes.compiled[index];
		const auto suffix = translated >= compiled ?
			0 : compiled - translated;
		if (suffixes.shift[index] == 0)
			words[suffixes.word[index]] = suffix;
		else
			words[suffixes.word[index]] |=
				suffix << suffixes.shift[index];
	}
}

/* The epilogue run a head in front of it can do whole: SP-based 64-bit loads
 * and at most one ADD SP, SP, #imm, then the RET through X30 that ends the
 * block, or a hint the virtual CPU runs as a NOP and then that RET
 * (engine/run_heads.inc has what the head does and its operand words). */
namespace epilogue {

inline constexpr std::size_t maximum_loads = 8;
inline constexpr std::size_t maximum_adds = 1;
/* The elements a run may have, ADD SP included: every one of them and the
 * RET must still be in the builder's ring of group starts. */
inline constexpr std::size_t maximum_elements = 16;
inline constexpr std::size_t ring = 32;
/* Every element and the RET is one decode group of two words, behind its
 * execution_count group in a counting build. */
#if UML_GADGET_EXECUTION_HISTOGRAM
inline constexpr std::size_t count_words = 3;
#else
inline constexpr std::size_t count_words = 0;
#endif
inline constexpr std::size_t group_words = count_words + 2;

/* By loads, then ADD SP count. */
constexpr std::array<std::array<void (*)(), maximum_adds + 1>, maximum_loads>
	heads{{
		{uml_arm64_gadget_run_epilogue_1_0,
		 uml_arm64_gadget_run_epilogue_1_1},
		{uml_arm64_gadget_run_epilogue_2_0,
		 uml_arm64_gadget_run_epilogue_2_1},
		{uml_arm64_gadget_run_epilogue_3_0,
		 uml_arm64_gadget_run_epilogue_3_1},
		{uml_arm64_gadget_run_epilogue_4_0,
		 uml_arm64_gadget_run_epilogue_4_1},
		{uml_arm64_gadget_run_epilogue_5_0,
		 uml_arm64_gadget_run_epilogue_5_1},
		{uml_arm64_gadget_run_epilogue_6_0,
		 uml_arm64_gadget_run_epilogue_6_1},
		{uml_arm64_gadget_run_epilogue_7_0,
		 uml_arm64_gadget_run_epilogue_7_1},
		{uml_arm64_gadget_run_epilogue_8_0,
		 uml_arm64_gadget_run_epilogue_8_1},
	}};

/* One load, at its address from SP at entry. A single load names its
 * register as both halves and owns the first 8 of the 16 bytes the head
 * reads. */
struct Load final {
	std::int64_t address{};
	unsigned first{};
	unsigned second{};
	std::int64_t bytes{};
};

struct Plan final {
	std::size_t loads{};
	std::size_t adds{};
	std::array<Load, maximum_loads> load{};
};

/* A head and its load count, which fixes its operand words. */
struct Head final {
	void (*gadget)(){};
	std::size_t loads{};
};

[[nodiscard]] constexpr bool is_element(std::uint32_t word) noexcept
{
	return (word & 0xfe40'03e0U) == 0xa840'03e0U ||  // LDP Xt, Xt2, [SP..]
		(word & 0xffc0'03e0U) == 0xf940'03e0U ||  // LDR Xt, [SP, #imm]
		((word & 0xffe0'03e0U) == 0xf840'03e0U && // LDUR, LDR Xt with
		 ((word >> 10) & 3U) != 2U) ||            // writeback
		(word & 0xff80'03ffU) == 0x9100'03ffU;    // ADD SP, SP, #imm
}

/* Adds one element, at SP offset `sp`, to `plan` and advances `sp` past
 * its writeback. False for a form the head does not perform: an access
 * through an SP the ordinary gadget would fault on as misaligned, a load
 * into XZR or of one register twice, a ninth load or a second ADD SP. */
[[nodiscard]] bool add(std::uint32_t word, std::int64_t &sp, Plan &plan) noexcept
{
	if ((word & 0xff80'03ffU) == 0x9100'03ffU) {
		if (plan.adds == maximum_adds)
			return false;
		++plan.adds;
		sp += static_cast<std::int64_t>(((word >> 10) & 0xfffU) <<
			((word & 0x0040'0000U) != 0 ? 12U : 0U));
		return true;
	}
	if ((sp & 15) != 0 || plan.loads == maximum_loads)
		return false;
	Load load{};
	if ((word & 0xfe40'03e0U) == 0xa840'03e0U) {
		load.first = word & 31U;
		load.second = (word >> 10) & 31U;
		load.bytes = 16;
		const auto mode = (word >> 23) & 3U;
		const auto displacement =
			sign_extend((word >> 15) & 0x7fU, 7) * 8;
		load.address = mode == 1U ? sp : sp + displacement;
		if (mode == 1U || mode == 3U)
			sp += displacement;
	} else if ((word & 0xffc0'03e0U) == 0xf940'03e0U) {
		load.first = load.second = word & 31U;
		load.bytes = 8;
		load.address = sp + static_cast<std::int64_t>(
			((word >> 10) & 0xfffU) << 3);
	} else {
		load.first = load.second = word & 31U;
		load.bytes = 8;
		const auto mode = (word >> 10) & 3U;
		const auto displacement = sign_extend((word >> 12) & 0x1ffU, 9);
		load.address = mode == 1U ? sp : sp + displacement;
		if (mode != 0U)
			sp += displacement;
	}
	if (load.first == 31U || load.second == 31U ||
	    (load.bytes == 16 && load.first == load.second))
		return false;
	plan.load[plan.loads++] = load;
	return true;
}

/* The head's operand words for the elements [start, ret) of `window`, the
 * RET at `ret`, into `operands`. The result is the head, without a gadget
 * when it cannot do the elements. */
[[nodiscard]] Head encode(std::span<const std::uint32_t> window,
	std::size_t start, std::size_t ret,
	std::array<std::uintptr_t, 4> &operands) noexcept
{
	Plan plan{};
	std::int64_t sp = 0;
	for (std::size_t index = start; index < ret; ++index)
		if (!add(window[index], sp, plan))
			return {};
	/* X30 comes from the second register of exactly one pair, which goes
	 * last so that it is the value the head returns through. */
	std::uint32_t written = 0;
	std::size_t link = maximum_loads;
	for (std::size_t index = 0; index < plan.loads; ++index) {
		const auto &load = plan.load[index];
		const auto registers = (1U << load.first) | (1U << load.second);
		if ((written & registers) != 0)
			return {};
		written |= registers;
		if (load.first == 30U && load.bytes == 8)
			return {};
		if (load.second == 30U && load.bytes == 16)
			link = index;
		else if (load.first == 30U)
			return {};
	}
	if (link == maximum_loads)
		return {};
	std::rotate(plan.load.begin() + static_cast<std::ptrdiff_t>(link),
		plan.load.begin() + static_cast<std::ptrdiff_t>(link) + 1,
		plan.load.begin() + static_cast<std::ptrdiff_t>(plan.loads));
	/* The bytes each load reads are its own; the head's 16-byte reads may
	 * reach past a single load's 8, so they stay in the span but may
	 * overlap another load's. */
	std::int64_t low = plan.load[0].address;
	std::int64_t end = low + 16;
	for (std::size_t index = 0; index < plan.loads; ++index) {
		const auto &load = plan.load[index];
		if ((load.address & 7) != 0)
			return {};
		for (std::size_t other = 0; other < index; ++other) {
			const auto &earlier = plan.load[other];
			if (load.address < earlier.address + earlier.bytes &&
			    earlier.address < load.address + load.bytes)
				return {};
		}
		low = std::min(low, load.address);
		end = std::max(end, load.address + 16);
	}
	const auto last = end - 1 - low;
	const auto low_units = low / 8;
	if (last > 0xfff || low_units < -4096 || low_units > 4095 ||
	    (sp & 15) != 0 || sp / 16 < -2048 || sp / 16 > 2047)
		return {};
	const auto packed = [&](std::size_t index) {
		const auto &load = plan.load[index];
		return static_cast<std::uintptr_t>((load.address - low) / 8) |
			(static_cast<std::uintptr_t>(load.first) << 9) |
			(static_cast<std::uintptr_t>(load.second) << 14);
	};
	operands = {};
	operands[0] = (static_cast<std::uintptr_t>(low_units) & 0x1fffU) |
		(static_cast<std::uintptr_t>(last) << 13) |
		((static_cast<std::uintptr_t>(sp / 16) & 0xfffU) << 25) |
		(packed(0) << 37);
	for (std::size_t index = 1; index < plan.loads; ++index)
		operands[1 + (index - 1) / 3] |=
			packed(index) << (19 * ((index - 1) % 3));
	return {heads[plan.loads - 1][plan.adds], plan.loads};
}

[[nodiscard]] constexpr std::size_t operand_words(std::size_t loads) noexcept
{
	return 1 + (loads + 1) / 3;
}

using Starts = std::array<std::uint32_t, ring>;
#if UML_GADGET_EXECUTION_HISTOGRAM
using Predecessors = std::array<const void *, ring>;
#endif

/* Put a head in front of the longest epilogue run the RET at `ret` ends, now
 * that the RET's group is the last one emitted. `starts` holds where each
 * recent instruction's group begins. The run's words stay where they are,
 * behind the head, and are its fallback. A group that is only the nop gadget
 * right before the RET, which a hint without effect on the virtual CPU
 * translates to (AUTIASP in code built for pointer authentication), may end
 * the run instead; it goes with the head, since it does nothing on either
 * path and the head finds the RET's cell at a fixed distance. */
void place(ProgramWords &words, InlineSuffixes &suffixes,
	std::span<const std::uint32_t> window, std::size_t ret,
	const Starts &starts
#if UML_GADGET_EXECUTION_HISTOGRAM
	, const Predecessors &predecessors
#endif
	)
{
	const auto ret_start = starts[ret % ring];
	if (words.size() != ret_start + group_words ||
	    words[ret_start + count_words] != reinterpret_cast<std::uintptr_t>(
		    uml_arm64_gadget_return_x30_cached))
		return;
	auto end = ret;
	if (ret != 0 && starts[(ret - 1) % ring] + count_words + 1 == ret_start &&
	    words[ret_start - 1] ==
		    reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_nop))
		end = ret - 1;
	std::size_t length = 0;
	while (length < std::min(end, maximum_elements) &&
	       is_element(window[end - 1 - length]))
		++length;
	for (; length != 0; --length) {
		const auto start = end - length;
		bool grouped = true;
		for (std::size_t index = start; index < end; ++index)
			grouped = grouped && starts[(index + 1) % ring] ==
				starts[index % ring] + group_words;
		if (!grouped)
			continue;
		std::array<std::uintptr_t, 4> operands{};
		const auto [gadget, loads] = encode(window, start, end, operands);
		if (gadget == nullptr)
			continue;
		if (end != ret) {
			const auto dropped = starts[end % ring];
			words.erase(dropped, count_words + 1);
#if UML_GADGET_EXECUTION_HISTOGRAM
			/* The RET's group now follows the run's last word. */
			words[dropped + 1] = diagnostics::execution_histogram_slot(
				predecessors[end % ring],
				reinterpret_cast<const void *>(
					uml_arm64_gadget_return_x30_cached));
#endif
			for (std::uint32_t index = 0; index < suffixes.count; ++index)
				if (suffixes.word[index] > dropped)
					suffixes.word[index] -= static_cast<std::uint32_t>(
						count_words + 1);
		}
		const auto head = reinterpret_cast<std::uintptr_t>(gadget);
		const std::size_t at = starts[start % ring];
		const auto inserted = count_words + 1 + operand_words(loads);
		words.insert(at, inserted);
		auto cursor = at;
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The head is counted as a group of no guest words, and the
		 * run's first word names it as its predecessor, so a counting
		 * build tells the head's fallbacks from its runs. */
		words[cursor++] = reinterpret_cast<std::uintptr_t>(
			diagnostics::uml_arm64_gadget_execution_count);
		words[cursor++] = diagnostics::execution_histogram_slot(
			predecessors[start % ring],
			reinterpret_cast<const void *>(head));
		words[cursor++] = 0;
#endif
		words[cursor++] = head;
		for (std::size_t index = 0; index < operand_words(loads); ++index)
			words[cursor++] = operands[index];
#if UML_GADGET_EXECUTION_HISTOGRAM
		words[cursor + 1] = diagnostics::execution_histogram_slot(
			reinterpret_cast<const void *>(head),
			reinterpret_cast<const void *>(words[cursor + count_words]));
#endif
		for (std::uint32_t index = 0; index < suffixes.count; ++index)
			if (suffixes.word[index] >= at)
				suffixes.word[index] +=
					static_cast<std::uint32_t>(inserted);
		return;
	}
}

} // namespace epilogue

} // namespace

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	ProgramWords words{storage};
	InlineSuffixes suffixes{};
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	/* A transfer without a PC word names its instruction by an 8-bit
	 * position in the block, which also keeps every packed inline suffix
	 * within sixteen bits. */
	const auto window = instructions.first(std::min(instructions.size(),
		fault_position_instructions));
	/* A program for SVCR.SM = 1 selects only entries that are safe in
	 * streaming mode; forwarding contracts do not reach into them, and its
	 * executor loads the FP state before it enters, so it needs no FP link
	 * entry. */
	const bool streaming = (options.svcr & svcr_sm) != 0U;
	if (streaming) [[unlikely]] {
		options.scalar_forwarding = false;
		options.vector_forwarding = false;
	}
	const auto finish = [&](std::size_t retired) {
		resolve_inline_suffixes(words, suffixes, retired);
		return Program{guest_address, retired, std::move(words),
			program_accesses_fp_state && !streaming};
	};
#if UML_GADGET_EXECUTION_HISTOGRAM
	const void *previous_gadget = nullptr;
#endif
	std::uint16_t previous_scalar = scalar_no_register;
	std::uint16_t previous_vector = scalar_no_register;
	/* FIZ, AH and NEP are guest state only where the virtual CPU implements
	 * FEAT_AFP; there every entry that would ignore them runs as its twin,
	 * which carries no forwarding contract. */
	const bool afp = implements_afp(options.host_capabilities);
	epilogue::Starts group_starts{};
#if UML_GADGET_EXECUTION_HISTOGRAM
	epilogue::Predecessors group_predecessors{};
#endif
	/* Indexed rather than ranged, because a decoder that fuses a pair needs to
	 * see what follows the instruction it was handed. `compiled` is the loop
	 * position as well as the retired count, and a fusing decoder advances it
	 * past both instructions itself. */
	while (compiled < window.size()) {
		const auto instruction = window[compiled];
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
		group_starts[compiled % epilogue::ring] =
			static_cast<std::uint32_t>(words.size());
#if UML_GADGET_EXECUTION_HISTOGRAM
		group_predecessors[compiled % epilogue::ring] = previous_gadget;
		const auto counter_word = words.size();
		emit(words, diagnostics::uml_arm64_gadget_execution_count);
		emit(words, std::uintptr_t{sizeof(std::uint64_t)});
		emit(words, std::uintptr_t{1});
#endif
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {finish(compiled), true, address, instruction};
		}

		const auto compiled_before = compiled;
		DecodeContext context{
			guest_address,
			address,
			instruction,
			window.subspan(compiled + 1),
			compiled,
			program_accesses_fp_state,
			words,
			resolve_link,
			resolve_indirect_link,
			options,
			suffixes,
		};
		const auto emitted_start = words.size();
		DecodeResult decoded = DecodeResult::decoded;
		switch (production_decode_starts[instruction >> 24]) {
		case ProductionDecodeStart::branch_system:
			decoded = decode_branch_system(context);
			break;
		case ProductionDecodeStart::data_processing_immediate:
		case ProductionDecodeStart::data_processing_register:
			decoded = decode_data_processing(context);
			break;
		case ProductionDecodeStart::data_processing_pointer_auth_overlap:
			decoded = pointer_auth_lr_unavailable(instruction) ?
				decode_branch_system(context) :
				decode_data_processing(context);
			break;
		case ProductionDecodeStart::floating_simd:
			decoded = decode_floating_simd(context);
			break;
		case ProductionDecodeStart::load_store:
			decoded = decode_load_store(context);
			break;
		case ProductionDecodeStart::exception:
			decoded = decode_exception(context);
			break;
		}
		if (afp && words.size() > emitted_start)
			words[emitted_start] = afp_twin(words[emitted_start]);
		const auto current_vector = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			vector_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto vector_mask = current_vector.source_mask(previous_vector);
		previous_vector = current_vector.destination;
		// The selected assembly entry owns its access description. Do not
		// infer it again from opcode bits: aliases and fused groups differ.
		const auto current_scalar = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			scalar_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto source_mask = current_scalar.source_mask(previous_scalar);
		if (source_mask != 0 && options.scalar_forwarding)
			words[emitted_start] = current_scalar.contract->entries[source_mask];
		previous_scalar = current_scalar.destination;
		if (vector_mask != 0 && options.vector_forwarding)
			words[emitted_start] = current_vector.contract->entries[vector_mask];
		if (streaming && words.size() > emitted_start &&
		    !select_streaming_entries(std::span{words.data(),
			    words.size()}.subspan(emitted_start))) [[unlikely]] {
			/* The base entries are unsafe in streaming mode: an
			 * FP&SIMD class legal there takes its generated form, and
			 * any other word ends the block at the illegal exit, as
			 * an unimplemented word does. */
			words.truncate(emitted_start);
			while (suffixes.count != 0 &&
			       suffixes.word[suffixes.count - 1] >= emitted_start)
				--suffixes.count;
			compiled = compiled_before;
			decoded = decode_streaming_fallback(context);
		}
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The entry is final only after forwarding selection above. */
		if (words.size() > emitted_start) {
			words[counter_word + 1] = diagnostics::execution_histogram_slot(
				previous_gadget,
				reinterpret_cast<const void *>(words[emitted_start]));
			words[counter_word + 2] = std::max<std::size_t>(1,
				compiled - compiled_before +
				(decoded == DecodeResult::decoded));
		}
		/* The first word a decode emits is its gadget pointer, which the
		 * execution histogram pairs with its predecessor. */
		if (words.size() > emitted_start)
			previous_gadget = reinterpret_cast<const void *>(
				words[emitted_start]);
#endif
		if (decoded == DecodeResult::terminal && !streaming &&
		    instruction == 0xd65f'03c0U && compiled == compiled_before + 1)
			epilogue::place(words, suffixes, window, compiled_before,
				group_starts
#if UML_GADGET_EXECUTION_HISTOGRAM
				, group_predecessors
#endif
				);
		if (decoded != DecodeResult::decoded)
			return {finish(compiled), decoded == DecodeResult::terminal,
				address, instruction};
		++compiled;
	}
	const auto fallthrough = guest_address + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	return {finish(compiled), true, fallthrough, 0};
}

} // namespace uml::cpu::arm64::gadgets
