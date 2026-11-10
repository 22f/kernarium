// SPDX-License-Identifier: GPL-2.0-or-later
/* Epilogue run heads (engine/run_heads.inc). A block that ends in SP-based
 * 64-bit loads and ADD SP, then a RET through X30, begins that run with a
 * head, which does the whole run and the return when SP is aligned and every
 * byte it reads lies in one page the TLB holds, and otherwise runs the
 * ordinary words behind it. This test checks where the builder puts a head
 * and what it encodes; that the head does the run and the return itself,
 * through the site's cells and through the return ring; that a span across
 * two pages, a misaligned SP and a page the TLB does not hold each fall back
 * and end as the architecture says; that a fault inside the fallback resumes
 * at its instruction; and, over generated runs, that every block ends with
 * the state an independent model of the instructions gives. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_return_x30_cached();
extern "C" void uml_arm64_gadget_return();
extern "C" void uml_arm64_gadget_illegal();
extern "C" void uml_arm64_gadget_run_epilogue_1_0();
extern "C" void uml_arm64_gadget_run_epilogue_1_1();
extern "C" void uml_arm64_gadget_run_epilogue_2_0();
extern "C" void uml_arm64_gadget_run_epilogue_2_1();
extern "C" void uml_arm64_gadget_run_epilogue_3_0();
extern "C" void uml_arm64_gadget_run_epilogue_3_1();
extern "C" void uml_arm64_gadget_run_epilogue_4_0();
extern "C" void uml_arm64_gadget_run_epilogue_4_1();
extern "C" void uml_arm64_gadget_run_epilogue_5_0();
extern "C" void uml_arm64_gadget_run_epilogue_5_1();
extern "C" void uml_arm64_gadget_run_epilogue_6_0();
extern "C" void uml_arm64_gadget_run_epilogue_6_1();
extern "C" void uml_arm64_gadget_run_epilogue_7_0();
extern "C" void uml_arm64_gadget_run_epilogue_7_1();
extern "C" void uml_arm64_gadget_run_epilogue_8_0();
extern "C" void uml_arm64_gadget_run_epilogue_8_1();

namespace {

using uml::emulator::tests::address;
using uml::testing::require;

/* By loads, then ADD SP count. */
constexpr std::array<std::array<void (*)(), 2>, 8> heads{{
	{uml_arm64_gadget_run_epilogue_1_0, uml_arm64_gadget_run_epilogue_1_1},
	{uml_arm64_gadget_run_epilogue_2_0, uml_arm64_gadget_run_epilogue_2_1},
	{uml_arm64_gadget_run_epilogue_3_0, uml_arm64_gadget_run_epilogue_3_1},
	{uml_arm64_gadget_run_epilogue_4_0, uml_arm64_gadget_run_epilogue_4_1},
	{uml_arm64_gadget_run_epilogue_5_0, uml_arm64_gadget_run_epilogue_5_1},
	{uml_arm64_gadget_run_epilogue_6_0, uml_arm64_gadget_run_epilogue_6_1},
	{uml_arm64_gadget_run_epilogue_7_0, uml_arm64_gadget_run_epilogue_7_1},
	{uml_arm64_gadget_run_epilogue_8_0, uml_arm64_gadget_run_epilogue_8_1},
}};

constexpr std::uint64_t code_pc = 0x4000;
constexpr std::uint64_t return_pc = 0x5000;
constexpr std::uint64_t page_size = 4096;
/* Guest pages: `low` and `high` are adjacent and the TLB holds both unless a
 * case says otherwise; `beyond`, above them, it never holds, and nothing can
 * fill it, so an access there faults. */
constexpr std::uint64_t low = 0x7000'0000;
constexpr std::uint64_t high = low + page_size;
constexpr std::uint64_t beyond = high + page_size;

constexpr std::uint32_t ret = 0xd65f'03c0U;
constexpr std::uint32_t svc = 0xd400'0001U;

enum class Mode : std::uint32_t {
	offset = 0xa940'0000U,
	pre = 0xa9c0'0000U,
	post = 0xa8c0'0000U,
	no_allocate = 0xa840'0000U,
};

[[nodiscard]] constexpr std::uint32_t ldp(unsigned first, unsigned second,
	int offset, Mode mode = Mode::offset) noexcept
{
	return static_cast<std::uint32_t>(mode) |
		((static_cast<std::uint32_t>(offset / 8) & 0x7fU) << 15) |
		(second << 10) | (31U << 5) | first;
}
[[nodiscard]] constexpr std::uint32_t ldp32(unsigned first, unsigned second,
	int offset) noexcept
{
	return 0x2940'0000U |
		((static_cast<std::uint32_t>(offset / 4) & 0x7fU) << 15) |
		(second << 10) | (31U << 5) | first;
}
[[nodiscard]] constexpr std::uint32_t ldr(unsigned reg, unsigned offset) noexcept
{
	return 0xf940'0000U | ((offset / 8) << 10) | (31U << 5) | reg;
}
/* LDUR (0x000), LDR post-index (0x400) and pre-index (0xc00). */
[[nodiscard]] constexpr std::uint32_t ldr9(unsigned reg, int offset,
	std::uint32_t kind) noexcept
{
	return 0xf840'0000U | kind |
		((static_cast<std::uint32_t>(offset) & 0x1ffU) << 12) |
		(31U << 5) | reg;
}
[[nodiscard]] constexpr std::uint32_t ldur(unsigned reg, int offset) noexcept
{
	return ldr9(reg, offset, 0x000U);
}
[[nodiscard]] constexpr std::uint32_t ldr_post(unsigned reg, int offset) noexcept
{
	return ldr9(reg, offset, 0x400U);
}
[[nodiscard]] constexpr std::uint32_t ldr_pre(unsigned reg, int offset) noexcept
{
	return ldr9(reg, offset, 0xc00U);
}
[[nodiscard]] constexpr std::uint32_t add_sp(unsigned immediate) noexcept
{
	return 0x9100'03ffU | (immediate << 10);
}

/* Three host pages behind the guest pages low, high and beyond, with a
 * pattern no two 8-byte words share. */
struct Memory final {
	Memory() : bytes{static_cast<std::byte *>(
			   std::aligned_alloc(page_size, 3 * page_size))}
	{
		require(bytes != nullptr);
		for (std::size_t index = 0; index < 3 * page_size; ++index)
			bytes[index] = static_cast<std::byte>(
				(index * 131U + (index >> 8) * 7U + 13U) & 0xffU);
	}
	~Memory() { std::free(bytes); }
	Memory(const Memory &) = delete;
	Memory &operator=(const Memory &) = delete;

	[[nodiscard]] std::byte *host(std::uint64_t guest) const noexcept
	{
		return bytes + (guest - low);
	}
	void put(std::uint64_t guest, std::uint64_t value) const noexcept
	{
		std::memcpy(host(guest), &value, sizeof(value));
	}
	[[nodiscard]] std::uint64_t get(std::uint64_t guest) const noexcept
	{
		std::uint64_t value = 0;
		std::memcpy(&value, host(guest), sizeof(value));
		return value;
	}

	std::byte *bytes;
};

void hold(::uml::memory::Tlb &tlb, const Memory &memory,
	  std::uint64_t page) noexcept
{
	auto &entry = tlb.entries[(page >> 12) & (::uml::memory::tlb_entry_count - 1)];
	entry.read_page = page >> 12;
	entry.write_page = page >> 12;
	entry.read_data_minus_address =
		reinterpret_cast<std::uintptr_t>(memory.host(page)) - page;
	entry.write_data_minus_address = entry.read_data_minus_address;
}

/* What the instructions do, from the architecture: SP must be 16-byte
 * aligned for every access through it, a pair or a single load faults
 * without writing anything when a byte of it is on a page the TLB does not
 * hold, and a load writes back only when it completes. */
struct Expected final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::optional<std::size_t> fault;
	bool alignment{};
};

[[nodiscard]] Expected model(std::span<const std::uint32_t> run,
	const arm64::CpuState &start, const Memory &memory,
	const std::array<bool, 3> &held)
{
	Expected out{};
	std::copy_n(start.registers.begin(), 31, out.registers.begin());
	out.sp = start.sp;
	const auto readable = [&](std::uint64_t address, std::uint64_t bytes) {
		for (auto at = address; at < address + bytes; ++at) {
			if (at < low || at >= low + 3 * page_size ||
			    !held[(at - low) / page_size])
				return false;
		}
		return true;
	};
	for (std::size_t index = 0; index < run.size(); ++index) {
		const auto word = run[index];
		if ((word & 0xff80'03ffU) == 0x9100'03ffU) {
			out.sp += ((word >> 10) & 0xfffU) <<
				((word & 0x0040'0000U) != 0 ? 12U : 0U);
			continue;
		}
		if ((out.sp & 15U) != 0) {
			out.fault = index;
			out.alignment = true;
			return out;
		}
		std::uint64_t address = 0;
		std::uint64_t writeback = out.sp;
		std::uint64_t bytes = 8;
		unsigned first = word & 31U;
		unsigned second = 31U;
		const auto signed_field = [](std::uint32_t value, unsigned bits) {
			const auto shift = 64U - bits;
			return static_cast<std::int64_t>(
				static_cast<std::uint64_t>(value) << shift) >> shift;
		};
		if ((word & 0x3a00'0000U) == 0x2800'0000U) {
			const bool wide = (word >> 30) == 2U;
			const auto scale = wide ? 8 : 4;
			bytes = wide ? 16 : 8;
			second = (word >> 10) & 31U;
			const auto displacement =
				signed_field((word >> 15) & 0x7fU, 7) * scale;
			const auto mode = (word >> 23) & 3U;
			address = mode == 1U ? out.sp : out.sp +
				static_cast<std::uint64_t>(displacement);
			if (mode == 1U || mode == 3U)
				writeback = out.sp +
					static_cast<std::uint64_t>(displacement);
		} else if ((word & 0xffc0'0000U) == 0xf940'0000U) {
			address = out.sp + (((word >> 10) & 0xfffU) << 3);
		} else {
			const auto displacement =
				signed_field((word >> 12) & 0x1ffU, 9);
			const auto kind = (word >> 10) & 3U;
			address = kind == 1U ? out.sp : out.sp +
				static_cast<std::uint64_t>(displacement);
			if (kind != 0U)
				writeback = out.sp +
					static_cast<std::uint64_t>(displacement);
		}
		if (!readable(address, bytes)) {
			out.fault = index;
			return out;
		}
		if (bytes == 8 && second != 31U) {
			/* LDP of two W registers zero-extends each. */
			const auto both = memory.get(address);
			if (first != 31U)
				out.registers[first] = both & 0xffff'ffffU;
			if (second != 31U)
				out.registers[second] = both >> 32;
		} else {
			if (first != 31U)
				out.registers[first] = memory.get(address);
			if (second != 31U)
				out.registers[second] = memory.get(address + 8);
		}
		out.sp = writeback;
	}
	return out;
}

/* Direct targets resolve to records of their own, as the executor's do. */
const gadgets::BlockLink *unlinked(std::uint64_t pc)
{
	static std::map<std::uint64_t, uml::emulator::tests::UnlinkedLink>
		records;
	return &records.try_emplace(pc, pc).first->second.link;
}

struct Site final {
	Site()
	{
		for (auto &entry : site.entries)
			entry = {~std::uint64_t{}, &empty.link};
	}
	gadgets::IndirectLink site{};
	uml::emulator::tests::UnlinkedLink empty{~std::uint64_t{}};
};

[[nodiscard]] gadgets::BuildResult build(std::uint64_t pc,
	std::span<const std::uint32_t> code, Site *site)
{
	if (site == nullptr)
		return gadgets::Builder{}.build(pc, code, unlinked,
			std::pmr::get_default_resource());
	return gadgets::Builder{}.build(pc, code, unlinked,
		std::pmr::get_default_resource(),
		[site](std::uint64_t) { return &site->site; });
}

[[nodiscard]] arm64::CpuState initial(std::uint64_t sp, std::uint64_t pc)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31; ++index)
		cpu.registers[index] = 0x5a5a'0000'0000'0000ULL | index;
	cpu.sp = sp;
	cpu.pc = pc;
	return cpu;
}

struct Outcome final {
	::uml::cpu::EngineEvent event{};
	arm64::CpuState cpu{};
	std::size_t executed{};
};

[[nodiscard]] Outcome run(const gadgets::Program &program,
	arm64::CpuState cpu, ::uml::memory::Tlb &tlb)
{
	std::size_t executed = 0;
	tlb.fault_alignment = 0;
	const auto event = gadgets::execute(program, cpu, tlb, executed);
	gadgets::resolve_fault_position(program, cpu.pc);
	return {event, cpu, executed};
}

/* The block's outcome against the model: a fault at its instruction, or the
 * whole run and a RET whose site misses, which leaves at the X30 the run
 * loaded. */
void check(const char *what, const Outcome &outcome, const Expected &expected,
	   std::uint64_t pc, std::size_t instructions, const Site *site)
{
	const auto fail = [&](const char *why) {
		std::fprintf(stderr, "arm64-gadget-run-heads: %s: %s (event %d, "
			     "PC %#llx, retired %zu, SP %#llx; expected SP %#llx)\n",
			     what, why, static_cast<int>(outcome.event),
			     static_cast<unsigned long long>(outcome.cpu.pc),
			     outcome.executed,
			     static_cast<unsigned long long>(outcome.cpu.sp),
			     static_cast<unsigned long long>(expected.sp));
		std::abort();
	};
	if (expected.fault) {
		if (outcome.event != ::uml::cpu::EngineEvent::memory_fault)
			fail("no memory fault");
		if (outcome.cpu.pc != pc + *expected.fault * 4)
			fail("wrong fault PC");
		/* A block retires whole at entry; the executor takes back what
		 * the fault PC did not reach. */
		if (outcome.executed != instructions)
			fail("wrong retired count at the fault");
	} else {
		if (outcome.event != ::uml::cpu::EngineEvent::continued)
			fail("the return did not exit");
		if (outcome.cpu.pc != expected.registers[30])
			fail("wrong return PC");
		if (outcome.executed != instructions)
			fail("wrong retired count");
		if (site != nullptr && outcome.cpu.gadget_indirect_miss != &site->site)
			fail("the miss did not name its site");
	}
	for (unsigned index = 0; index < 31; ++index)
		if (outcome.cpu.registers[index] != expected.registers[index])
			fail("wrong register");
	if (outcome.cpu.sp != expected.sp)
		fail("wrong SP");
}

/* The head in front of `code`'s run: its loads and ADD SP count, which
 * name it, and where it is, or nullopt. */
struct Placed final {
	std::size_t loads{};
	std::size_t adds{};
	std::size_t at{};
};

[[nodiscard]] std::optional<Placed> head_of(const gadgets::Program &program)
{
	const auto words = program.words();
	for (std::size_t index = 0; index < words.size(); ++index)
		for (std::size_t loads = 1; loads <= heads.size(); ++loads)
			for (std::size_t adds = 0; adds < 2; ++adds)
				if (words[index] == address(heads[loads - 1][adds]))
					return Placed{loads, adds, index};
	return std::nullopt;
}

/* `program` with every word behind the head that the head skips replaced:
 * each group by the illegal exit, which names the group's position, and the
 * RET's entry too, so a run that reaches any of them leaves illegally. */
[[nodiscard]] gadgets::Program without_fallback(const gadgets::Program &program,
	std::size_t groups)
{
	const auto placed = head_of(program);
	require(placed.has_value());
	const auto words = program.words();
	gadgets::ProgramWords copy{std::pmr::get_default_resource()};
	for (const auto word : words)
		copy.push_back(word);
	const auto first = placed->at + 1 + 1 + (placed->loads + 1) / 3;
	for (std::size_t group = 0; group <= groups; ++group) {
		copy[first + 2 * group] = address(uml_arm64_gadget_illegal);
		if (group < groups)
			copy[first + 2 * group + 1] = 0xdead'0000U + 4U * group;
	}
	return gadgets::Program{program.guest_address(),
		program.instruction_count(), std::move(copy), false};
}

/* Where the builder puts a head and the operand words it writes. */
void structure()
{
#if !UML_GADGET_EXECUTION_HISTOGRAM
	Site site;
	{
		const std::array code{ldp(20, 19, 16),
			ldp(29, 30, 32, Mode::post), ret};
		const auto built = build(code_pc, code, &site);
		const auto words = built.program.words();
		require(words.size() == 3 + 4 + 2);
		require(words[0] == address(uml_arm64_gadget_run_epilogue_2_0));
		/* Span 0..31, SP + 32 at exit, then the loads: x20/x19 at slot 2,
		 * then x29/x30 at slot 0. */
		const auto first_load = std::uintptr_t{2} | (20U << 9) | (19U << 14);
		const auto link_load = std::uintptr_t{0} | (29U << 9) | (30U << 14);
		require(words[1] == ((std::uintptr_t{31} << 13) |
			(std::uintptr_t{2} << 25) | (first_load << 37)));
		require(words[2] == link_load);
		require(words[7] == address(uml_arm64_gadget_return_x30_cached));
		require(words[8] == reinterpret_cast<std::uintptr_t>(&site.site));
	}
	/* The pair that loads X30 goes last, whatever its place in the run;
	 * the span starts at the lowest byte, which may be below SP. */
	{
		const std::array code{ldp(29, 30, -16, Mode::pre),
			ldr(19, 16), add_sp(32), ret};
		const auto built = build(code_pc, code, &site);
		const auto words = built.program.words();
		require(words[0] == address(uml_arm64_gadget_run_epilogue_2_1));
		const auto units = static_cast<std::uintptr_t>(-2) & 0x1fffU;
		const auto x19 = std::uintptr_t{2} | (19U << 9) | (19U << 14);
		const auto x30 = std::uintptr_t{0} | (29U << 9) | (30U << 14);
		require(words[1] == (units | (std::uintptr_t{31} << 13) |
			(std::uintptr_t{1} << 25) | (x19 << 37)));
		require(words[2] == x30);
	}
	/* Eight loads in four operand words. */
	{
		const std::array code{ldp(19, 20, 16), ldp(21, 22, 32),
			ldp(23, 24, 48), ldp(25, 26, 64), ldp(27, 28, 80),
			ldr(9, 96), ldr(10, 104), ldp(29, 30, 0), add_sp(112),
			ret};
		const auto built = build(code_pc, code, &site);
		const auto words = built.program.words();
		require(words[0] == address(uml_arm64_gadget_run_epilogue_8_1));
		require(words.size() == 1 + 4 + 2 * 10);
		require((words[4] & 0x7ffffU) ==
			(std::uintptr_t{0} | (29U << 9) | (30U << 14)));
	}
	/* More loads than a head holds: the head takes the last eight, behind
	 * the run's first two loads. */
	{
		const std::array code{ldp(0, 1, 160), ldp(2, 3, 144),
			ldp(4, 5, 128), ldp(6, 7, 112), ldp(19, 20, 96),
			ldp(21, 22, 80), ldp(23, 24, 64), ldp(25, 26, 48),
			ldp(27, 28, 32), ldp(29, 30, 176, Mode::post), ret};
		const auto built = build(code_pc, code, &site);
		const auto placed = head_of(built.program);
		require(placed && placed->loads == 8 && placed->at == 4);
	}
	/* What gets no head: X30 from a single load or as a pair's first
	 * register, no X30 at all, a RET through another register, a block
	 * built without indirect sites, and no load before the RET. */
	for (const auto &code : std::vector<std::vector<std::uint32_t>>{
		     {ldr_post(30, 16), ret},
		     {ldp(30, 29, 16, Mode::post), ret},
		     {ldp(19, 20, 16, Mode::post), ret},
		     {ldp(29, 30, 16, Mode::post), 0xd65f'03a0U},
		     {add_sp(16), ret},
		     {ret}}) {
		require(!head_of(build(code_pc, code, &site).program));
	}
	{
		const std::array code{ldp(29, 30, 16, Mode::post), ret};
		const auto built = build(code_pc, code, nullptr);
		require(!head_of(built.program));
		require(built.program.words()[2] == address(uml_arm64_gadget_return));
	}
	/* A run the head cannot do whole gets a head for the longest suffix it
	 * can: behind a 32-bit pair, a register loaded twice, an SP the ordinary
	 * gadget would fault on, and a second ADD SP. */
	for (const auto &[code, at] :
	     std::vector<std::pair<std::vector<std::uint32_t>, std::size_t>>{
		     {{ldp32(19, 20, 16), ldp(29, 30, 32, Mode::post), ret}, 2},
		     {{ldr(19, 16), ldr(19, 24), ldp(29, 30, 32, Mode::post),
		       ret}, 2},
		     {{add_sp(8), ldp(29, 30, 16, Mode::post), ret}, 2},
		     {{add_sp(16), add_sp(16), ldp(29, 30, 16, Mode::post), ret},
		      2}}) {
		const auto placed = head_of(build(code_pc, code, &site).program);
		require(placed && placed->at == at);
	}
#endif
}

struct Case final {
	const char *what;
	std::vector<std::uint32_t> code; // the run, then RET
};

const std::vector<Case> &cases()
{
	static const std::vector<Case> all{
		{"pair", {ldp(20, 19, 16), ldp(29, 30, 32, Mode::post), ret}},
		{"frame", {ldp(28, 27, 0), ldp(26, 25, 16), ldp(24, 23, 32),
			ldp(22, 21, 48), ldp(20, 19, 64), ldp(29, 30, 80),
			add_sp(96), ret}},
		{"single", {ldr(19, 16), ldp(29, 30, 32, Mode::post), ret}},
		{"mixed", {add_sp(16), ldur(21, 8), ldr_post(22, 16),
			ldp(29, 30, 16, Mode::post), ldr_pre(23, 0), ret}},
		{"no-allocate", {ldp(19, 20, 16, Mode::no_allocate),
			ldp(29, 30, 32, Mode::post), ret}},
		{"eight", {ldp(19, 20, 16), ldp(21, 22, 32), ldp(23, 24, 48),
			ldp(25, 26, 64), ldp(27, 28, 80), ldr(9, 96), ldr(10, 104),
			ldp(29, 30, 0), add_sp(112), ret}},
	};
	return all;
}

[[nodiscard]] std::span<const std::uint32_t> run_of(const Case &item) noexcept
{
	return {item.code.data(), item.code.size() - 1};
}

/* Each case's block where the head does the run: whole, and with nothing
 * behind the head to fall back to, which ends the same only if the head did
 * all of it. */
void head_does_the_run(const Memory &memory)
{
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	hold(*tlb, memory, low);
	hold(*tlb, memory, high);
	for (const auto &item : cases()) {
		Site site;
		const auto built = build(code_pc, item.code, &site);
		require(head_of(built.program).has_value());
		const auto start = initial(low + 0x200, code_pc);
		const auto expected =
			model(run_of(item), start, memory, {true, true, false});
		require(!expected.fault);
		check(item.what, run(built.program, start, *tlb), expected,
			code_pc, item.code.size(), &site);
#if !UML_GADGET_EXECUTION_HISTOGRAM
		const auto bare =
			without_fallback(built.program, run_of(item).size());
		check(item.what, run(bare, start, *tlb), expected, code_pc,
			item.code.size(), &site);
#endif
	}
}

/* The highest SP below `high` at which `run` reads bytes of both low and
 * high: with either page alone it faults, and not for alignment. */
[[nodiscard]] std::uint64_t crossing_sp(std::span<const std::uint32_t> run,
	const Memory &memory)
{
	for (auto sp = high - 16; sp > low; sp -= 16) {
		const auto start = initial(sp, code_pc);
		const auto below = model(run, start, memory, {true, false, false});
		const auto above = model(run, start, memory, {false, true, false});
		if (below.fault && above.fault && !below.alignment &&
		    !above.alignment)
			return sp;
	}
	require(false);
	return 0;
}

/* Each way the head falls back ends as the architecture says, and with
 * nothing behind the head the fallback reaches the run's first word. */
void head_falls_back(const Memory &memory)
{
	struct Fallback final {
		const char *what;
		/* 0 for an SP at which the run reads both low and high. */
		std::uint64_t sp;
		std::array<bool, 3> held;
	};
	const Fallback fallbacks[] = {
		/* The run's bytes cross from low into high; both are held. */
		{"across a page", 0, {true, true, false}},
		{"a misaligned SP", low + 0x208, {true, true, false}},
		/* The TLB does not hold the run's page. */
		{"a page the TLB lacks", high + 0x200, {true, false, false}},
	};
	for (const auto &fallback : fallbacks) {
		for (const auto &item : cases()) {
			auto tlb = std::make_unique<::uml::memory::Tlb>();
			hold(*tlb, memory, low);
			if (fallback.held[1])
				hold(*tlb, memory, high);
			Site site;
			const auto built = build(code_pc, item.code, &site);
			const auto start = initial(fallback.sp != 0 ? fallback.sp :
				crossing_sp(run_of(item), memory), code_pc);
			const auto expected = model(run_of(item), start, memory,
				fallback.held);
			const auto outcome = run(built.program, start, *tlb);
			check(item.what, outcome, expected, code_pc,
				item.code.size(), &site);
			if (expected.alignment)
				require(tlb->fault_alignment != 0);
#if !UML_GADGET_EXECUTION_HISTOGRAM
			const auto bare = without_fallback(built.program,
				run_of(item).size());
			auto fresh = std::make_unique<::uml::memory::Tlb>();
			hold(*fresh, memory, low);
			if (fallback.held[1])
				hold(*fresh, memory, high);
			const auto stopped = run(bare, start, *fresh);
			if (stopped.event !=
				    ::uml::cpu::EngineEvent::illegal_instruction ||
			    stopped.cpu.pc != 0xdead'0000U) {
				std::fprintf(stderr, "arm64-gadget-run-heads: %s, %s: "
					     "the fallback did not reach the run "
					     "(event %d, PC %#llx)\n", item.what,
					     fallback.what,
					     static_cast<int>(stopped.event),
					     static_cast<unsigned long long>(
						     stopped.cpu.pc));
				std::abort();
			}
#endif
		}
	}
}

/* A fault inside the fallback leaves the loads before it done, and the block
 * that starts at the faulting instruction, once its page is there, finishes
 * the run as one uninterrupted run would have. */
void fault_and_resume(const Memory &memory)
{
	for (const auto &item : cases()) {
		const auto run = run_of(item);
		/* SP 48 bytes below the page the TLB lacks: the loads at offsets
		 * from 48 on fault. */
		const auto start = initial(beyond - 48, code_pc);
		const auto expected = model(run, start, memory, {true, true, false});
		if (!expected.fault)
			continue;
		auto tlb = std::make_unique<::uml::memory::Tlb>();
		hold(*tlb, memory, low);
		hold(*tlb, memory, high);
		Site site;
		const auto built = build(code_pc, item.code, &site);
		const auto outcome = ::run(built.program, start, *tlb);
		check(item.what, outcome, expected, code_pc, item.code.size(),
			&site);

		hold(*tlb, memory, beyond);
		const auto resume = *expected.fault;
		const std::span rest{item.code.data() + resume,
			item.code.size() - resume};
		Site again;
		const auto resumed_block = build(outcome.cpu.pc, rest, &again);
		const auto whole = model(run, start, memory, {true, true, true});
		require(!whole.fault);
		auto resumed = ::run(resumed_block.program, outcome.cpu, *tlb);
		check(item.what, resumed, whole, outcome.cpu.pc, rest.size(),
			&again);
	}
}

/* The head returns through the site's cells and through the return ring,
 * as the RET's own entry does. */
void head_returns(const Memory &memory)
{
	const std::array target_code{0xd280'0000U | (77U << 5), svc};
	const auto target = gadgets::Builder{}.build(return_pc, target_code);
	uml::emulator::tests::LinkedBlock block{target.program};
	gadgets::BlockLink link{.guest_pc = return_pc};
	block.link(link);

	auto tlb = std::make_unique<::uml::memory::Tlb>();
	hold(*tlb, memory, low);
	const auto &item = cases()[0];
	const auto sp = low + 0x400;
	/* ldp x29, x30, [sp], #32 loads X30 from SP + 8. */
	const auto saved = memory.get(sp + 8);
	memory.put(sp + 8, return_pc);
	for (const bool ring : {false, true}) {
		Site site;
		if (!ring)
			site.site.entries[0] = {return_pc, &link};
		const auto built = build(code_pc, item.code, &site);
		require(head_of(built.program).has_value());
		auto start = initial(sp, code_pc);
		if (ring) {
			start.gadget_return_stack[0] = {return_pc, &link};
			start.gadget_return_top = 1;
		}
		const auto expected = model(run_of(item), start, memory,
			{true, false, false});
		for (const bool bare : {false, true}) {
#if UML_GADGET_EXECUTION_HISTOGRAM
			if (bare)
				continue;
#endif
			if (ring)
				for (auto &entry : site.site.entries)
					entry = {~std::uint64_t{}, &site.empty.link};
			const auto program = bare ?
				without_fallback(built.program, run_of(item).size()) :
				gadgets::Program{built.program};
			const auto outcome = run(program, start, *tlb);
			require(outcome.event == ::uml::cpu::EngineEvent::system_call);
			require(outcome.cpu.registers[0] == 77);
			require(outcome.cpu.pc == return_pc + 8);
			require(outcome.executed == item.code.size() + 2);
			require(outcome.cpu.sp == expected.sp);
			for (unsigned index = 1; index < 31; ++index)
				require(outcome.cpu.registers[index] ==
					expected.registers[index]);
			/* The ring's record answered, was popped and trained
			 * the site's cell. */
			if (ring) {
				require(outcome.cpu.gadget_return_top == 0);
				require(site.site.entries[0].guest_pc == return_pc);
			}
		}
	}
	memory.put(sp + 8, saved);
}

/* Generated runs of every element form, broken by 32-bit pairs, at SPs
 * across, below and above the held pages and misaligned ones, against the
 * model. */
void generated_runs(const Memory &memory)
{
	std::uint64_t state = 0x9e37'79b9'7f4a'7c15ULL;
	const auto next = [&state](std::uint64_t bound) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state % bound;
	};
	std::size_t with_head = 0;
	constexpr std::size_t count = 4000;
	for (std::size_t iteration = 0; iteration < count; ++iteration) {
		std::array<unsigned, 29> pool{};
		for (unsigned index = 0; index < pool.size(); ++index)
			pool[index] = index;
		for (std::size_t index = pool.size() - 1; index > 0; --index)
			std::swap(pool[index], pool[next(index + 1)]);
		std::size_t taken = 0;
		const auto take = [&]() {
			if (taken > 0 && next(20) == 0)
				return pool[next(taken)];
			return pool[taken++ % pool.size()];
		};
		std::vector<std::uint32_t> code;
		const auto elements = 1 + next(10);
		const auto link = next(10) == 0 ? elements : next(elements);
		for (std::size_t index = 0; index < elements; ++index) {
			const auto pick = next(20);
			const int sixteen = static_cast<int>(next(9)) - 2;
			const int step = next(10) == 0 ? 8 * (2 * sixteen + 1) :
				16 * sixteen;
			if (index == link) {
				const Mode modes[] = {Mode::offset, Mode::pre,
					Mode::post, Mode::no_allocate};
				code.push_back(ldp(29, 30, step, modes[next(4)]));
			} else if (pick < 11) {
				const Mode modes[] = {Mode::offset, Mode::pre,
					Mode::post, Mode::no_allocate};
				const auto first = take();
				auto second = take();
				if (second == first)
					second = first == 0 ? 1 : first - 1;
				code.push_back(ldp(first, second, step,
					modes[next(4)]));
			} else if (pick < 14) {
				code.push_back(ldr(take(), 8 * static_cast<unsigned>(
					next(24))));
			} else if (pick < 17) {
				const int offset = 4 * (static_cast<int>(next(48)) - 16);
				const std::uint32_t kinds[] = {0x000U, 0x400U, 0xc00U};
				code.push_back(ldr9(take(), offset, kinds[next(3)]));
			} else if (pick < 19) {
				code.push_back(add_sp(8 * static_cast<unsigned>(
					next(10))));
			} else {
				/* No element: it ends the run a head may take. */
				const auto first = take();
				auto second = take();
				if (second == first)
					second = first == 0 ? 1 : first - 1;
				code.push_back(ldp32(first, second, 8 * static_cast<int>(
					next(8))));
			}
		}
		code.push_back(ret);
		std::uint64_t sp = low + 16 * next(3 * page_size / 16);
		if (next(16) == 0)
			sp += 8;
		const std::array<bool, 3> held{true, true, next(3) == 0};
		auto tlb = std::make_unique<::uml::memory::Tlb>();
		hold(*tlb, memory, low);
		hold(*tlb, memory, high);
		if (held[2])
			hold(*tlb, memory, beyond);
		Site site;
		const auto built = build(code_pc, code, &site);
		if (head_of(built.program))
			++with_head;
		const auto start = initial(sp, code_pc);
		const std::span run_words{code.data(), code.size() - 1};
		const auto expected = model(run_words, start, memory, held);
		check("generated run", run(built.program, start, *tlb), expected,
			code_pc, code.size(), &site);
		/* Again with the TLB the first run left, which the head then
		 * finds warm. */
		check("generated run, again", run(built.program, start, *tlb),
			expected, code_pc, code.size(), &site);
	}
	std::printf("arm64-gadget-run-heads: %zu of %zu generated runs have a "
		    "head\n", with_head, count);
	require(with_head > count / 4);
}

} // namespace

int main()
{
	const Memory memory;
	structure();
	head_does_the_run(memory);
	head_falls_back(memory);
	fault_and_resume(memory);
	head_returns(memory);
	generated_runs(memory);
	std::puts("arm64-gadget-run-heads: ok");
	return 0;
}
