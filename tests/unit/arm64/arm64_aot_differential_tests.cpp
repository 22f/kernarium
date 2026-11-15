// SPDX-License-Identifier: GPL-2.0-or-later
/* The ahead-of-time code for every library of the configured set against the
 * gadget engine, which stays the reference: the same registers, the same
 * guest memory and the same faults must end in the same guest state and
 * memory, with the AOT code entered by the running engine. The harness is
 * general: it reads the libraries and functions the build generated and runs
 * the same cases over each. The scenarios are the unhappy paths of
 * docs/architecture.md, "Ahead-of-time code". */

#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/arm64/aot/aot_diagnostics.hpp"
#include "cpu/arm64/aot/aot_runtime.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/gadgets/executor.hpp"

#include <algorithm>
#include <array>
#include <arm_acle.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <signal.h>
#include <unistd.h>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace aot = arm64::aot::diagnostics;
namespace aot_runtime = arm64::aot;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::GenerationMapping;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

constexpr std::uint64_t page_bytes = 4096;
constexpr std::uint64_t stub_offset = 0x4000'0000ULL;
/* Beyond the text of the largest library under test. */
constexpr std::uint64_t arena_offset = 0x1000'0000ULL;
constexpr std::size_t arena_pages = 12;
constexpr std::size_t stack_pages = 2;
constexpr std::size_t max_blocks = 1500;
/* A tagged pointer never hits the AOT code's inline TLB probe (the engine
 * keys its entries by the untagged address), so each such access is one
 * engine step and one block: the AOT run needs far more blocks than the
 * reference for the same work. */
constexpr std::size_t aot_max_blocks = 500000;

struct SplitMix final {
	std::uint64_t state;
	std::uint64_t next() noexcept
	{
		std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		return z ^ (z >> 31);
	}
	std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
};

struct Segment final {
	std::uint64_t vaddr, offset, filesz, memsz;
};

/* The library as the guest maps it: every page of every PT_LOAD from the
 * file at its own offset, zero past the end of the file. */
struct LibraryImage final {
	std::vector<std::byte> bytes;
	std::size_t pages{};
};

[[nodiscard]] LibraryImage load_library(const std::string &path)
{
	std::ifstream in{path, std::ios::binary};
	require(in.good());
	std::vector<std::byte> file;
	{
		std::vector<char> raw{std::istreambuf_iterator<char>{in}, {}};
		file.resize(raw.size());
		std::memcpy(file.data(), raw.data(), raw.size());
	}
	require(file.size() > 64 && std::memcmp(file.data(), "\x7f" "ELF", 4) == 0);
	const auto u16 = [&](std::size_t at) {
		std::uint16_t v;
		std::memcpy(&v, &file[at], 2);
		return v;
	};
	const auto u32 = [&](std::size_t at) {
		std::uint32_t v;
		std::memcpy(&v, &file[at], 4);
		return v;
	};
	const auto u64 = [&](std::size_t at) {
		std::uint64_t v;
		std::memcpy(&v, &file[at], 8);
		return v;
	};
	const auto phoff = u64(32);
	const auto phentsize = u16(54);
	const auto phnum = u16(56);
	std::vector<Segment> segments;
	std::uint64_t end = 0;
	for (unsigned i = 0; i < phnum; ++i) {
		const auto at = phoff + i * phentsize;
		if (u32(at) != 1)
			continue;
		const Segment s{u64(at + 16), u64(at + 8), u64(at + 32),
			u64(at + 40)};
		segments.push_back(s);
		end = std::max(end, s.vaddr + s.memsz);
	}
	LibraryImage image;
	image.pages = (end + page_bytes - 1) / page_bytes;
	image.bytes.assign(image.pages * page_bytes, std::byte{0});
	for (const auto &s : segments) {
		const auto first = s.vaddr / page_bytes;
		const auto last = (s.vaddr + s.filesz + page_bytes - 1) / page_bytes;
		for (auto page = first; page < last; ++page) {
			const auto from = (s.offset & ~(page_bytes - 1)) +
				(page - first) * page_bytes;
			if (from >= file.size())
				break;
			const auto n = std::min<std::uint64_t>(
				page_bytes, file.size() - from);
			std::memcpy(&image.bytes[page * page_bytes], &file[from], n);
		}
	}
	return image;
}

/* The library whose cases are being generated: a case that starts at an
 * instruction which writes SP from a register gets that register set to a
 * stack address, aligned or not (see sp_source). */
const LibraryImage *g_sites_image = nullptr;

/* Addresses in another library mapped in the same address space: some
 * registers (and the return address) of a case hold one. */
std::vector<std::uint64_t> g_cross_targets;

/* The register an instruction writes SP from, or -1: the 64-bit
 * add/sub (immediate and extended register) and logical (immediate) forms
 * whose destination is SP and whose first source is a register. */
[[nodiscard]] int sp_source(const LibraryImage &image,
	std::uint64_t offset) noexcept
{
	if (offset + 4 > image.bytes.size())
		return -1;
	std::uint32_t w;
	std::memcpy(&w, &image.bytes[offset], 4);
	if ((w & 31U) != 31U || (w >> 31) == 0U || ((w >> 5) & 31U) == 31U)
		return -1;
	const bool add_sub_immediate = (w & 0x1f000000U) == 0x11000000U;
	const bool add_sub_extended = (w & 0x1fe00000U) == 0x0b200000U;
	const bool logical_immediate = (w & 0x1f800000U) == 0x12000000U;
	return add_sub_immediate || add_sub_extended || logical_immediate ?
		static_cast<int>((w >> 5) & 31U) : -1;
}

struct Function final {
	std::uint64_t start, end;
};

[[nodiscard]] std::vector<Function> load_functions(
	const std::string &path, std::size_t library)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<Function> out;
	std::size_t index;
	std::string a, b;
	while (in >> index >> a >> b)
		if (index == library)
			out.push_back({std::strtoull(a.c_str(), nullptr, 0),
				std::strtoull(b.c_str(), nullptr, 0)});
	require(!out.empty());
	return out;
}

/* A sample of each kind of instruction the rewriter inlined, per library:
 * `offset` of an instruction of `kind` (decode class and mnemonic). */
struct Site final {
	std::uint64_t offset;
	std::string kind;
};

[[nodiscard]] std::vector<Site> load_sites(const std::string &path,
	std::size_t library)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<Site> out;
	std::size_t index;
	std::string offset, kind;
	while (in >> index >> offset >> kind)
		if (index == library)
			out.push_back({std::strtoull(offset.c_str(), nullptr, 16), kind});
	return out;
}

/* One line per library the build rewrote: index, guest path, host file. */
struct LibraryFile final {
	std::size_t index;
	std::string guest, host;
};

[[nodiscard]] std::vector<LibraryFile> load_libraries(const std::string &path)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<LibraryFile> out;
	std::string line;
	while (std::getline(in, line)) {
		const auto first = line.find('\t');
		const auto second = line.find('\t', first + 1);
		require(first != std::string::npos && second != std::string::npos);
		out.push_back({std::strtoul(line.c_str(), nullptr, 10),
			line.substr(first + 1, second - first - 1),
			line.substr(second + 1)});
	}
	require(!out.empty());
	return out;
}

/* Pages of guest data: each unmapped, read-only or writable, with contents a
 * case seeds. */
class Arena final {
public:
	Arena(unsigned long generation, std::uint64_t base)
		: generation_{generation}, base_{base}
	{
		for (std::size_t i = 0; i < arena_pages + stack_pages; ++i) {
			backings_.push_back(std::make_unique<JitBacking>(1));
			maps_.emplace_back();
			kinds_.push_back(0);
		}
		for (std::size_t i = arena_pages; i < backings_.size(); ++i)
			set_kind(i, 2);
	}

	void set_kind(std::size_t page, std::uint8_t kind)
	{
		if (kinds_[page] == kind)
			return;
		maps_[page].reset();
		kinds_[page] = kind;
		if (kind != 0)
			maps_[page] = std::make_unique<GenerationMapping>(
				generation_, base_ + page * page_bytes,
				*backings_[page], 1,
				kind == 2 ? PROT_READ | PROT_WRITE : PROT_READ,
				false);
	}

	void fill(SplitMix &rng)
	{
		std::array<std::byte, page_bytes> bytes;
		for (auto &backing : backings_) {
			for (std::size_t i = 0; i < bytes.size(); i += 8) {
				/* Mostly small values so loops and counts stay
				 * bounded, with some wide ones. */
				std::uint64_t v = rng.next();
				if (rng.below(4) != 0)
					v &= 0xff;
				std::memcpy(&bytes[i], &v, 8);
			}
			backing->put(0, bytes);
		}
	}

	[[nodiscard]] std::vector<std::byte> snapshot() const
	{
		std::vector<std::byte> out(backings_.size() * page_bytes);
		for (std::size_t i = 0; i < backings_.size(); ++i)
			backings_[i]->get(0,
				std::span{out}.subspan(i * page_bytes, page_bytes));
		return out;
	}

	[[nodiscard]] std::uint64_t base() const noexcept { return base_; }
	[[nodiscard]] std::uint8_t kind(std::size_t page) const
	{
		return kinds_[page];
	}
	[[nodiscard]] std::size_t total() const noexcept
	{
		return backings_.size();
	}

private:
	unsigned long generation_;
	std::uint64_t base_;
	std::vector<std::unique_ptr<JitBacking>> backings_;
	std::vector<std::unique_ptr<GenerationMapping>> maps_;
	std::vector<std::uint8_t> kinds_;
};

/* One vCPU: the guest memory view and the executor of a thread. */
struct Vcpu final {
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
};

/* One address space holding the library at `bias` and a stub that makes a
 * system call, with its own executor. `mapped_pages` maps only the first
 * pages of the library (map_rest() maps the others, as a loader would
 * later); add_library() maps a second library into the same address space. */
struct Environment final {
	Environment(unsigned long generation_, std::uint64_t bias_,
		const LibraryImage &image_,
		std::size_t mapped_pages = SIZE_MAX)
		: generation{generation_}, bias{bias_},
		  backing{image_.pages},
		  stub_backing{1},
		  mapped{std::min(mapped_pages, image_.pages)},
		  map{generation, bias, backing, mapped,
			PROT_READ | PROT_EXEC},
		  stub_map{generation, bias + stub_offset, stub_backing, 1,
			PROT_READ | PROT_EXEC, true}
	{
		backing.put(0, image_.bytes);
		/* The stub: a system call, and after it `ldxr x0, [x1]; br x2`
		 * for the case that opens an exclusive monitor in the engine and
		 * then branches into the library. */
		stub_backing.put32(0, system_call);
		stub_backing.put32(4, 0xc85f7c20U);
		stub_backing.put32(8, 0xd61f0040U);
	}

	/* Map the pages of the library that were left out. */
	void map_rest(const LibraryImage &image)
	{
		const auto pages = image.pages - mapped;
		if (pages == 0)
			return;
		tail_backing = std::make_unique<JitBacking>(pages);
		tail_backing->put(0, std::span{image.bytes}.subspan(
			mapped * page_bytes, pages * page_bytes));
		tail_map = std::make_unique<GenerationMapping>(generation,
			bias + mapped * page_bytes, *tail_backing, pages,
			PROT_READ | PROT_EXEC, true);
		mapped = image.pages;
	}

	void add_library(std::uint64_t other_bias, const LibraryImage &image)
	{
		extra_backing = std::make_unique<JitBacking>(image.pages);
		extra_backing->put(0, image.bytes);
		extra_map = std::make_unique<GenerationMapping>(generation,
			other_bias, *extra_backing, image.pages,
			PROT_READ | PROT_EXEC, true);
		extra_stub_backing = std::make_unique<JitBacking>(1);
		extra_stub_backing->put32(0, system_call);
		extra_stub_map = std::make_unique<GenerationMapping>(generation,
			other_bias + stub_offset, *extra_stub_backing, 1,
			PROT_READ | PROT_EXEC, true);
	}

	unsigned long generation;
	std::uint64_t bias;
	JitBacking backing;
	JitBacking stub_backing;
	std::size_t mapped;
	JitMapping map;
	GenerationMapping stub_map;
	std::unique_ptr<JitBacking> tail_backing;
	std::unique_ptr<GenerationMapping> tail_map;
	std::unique_ptr<JitBacking> extra_backing;
	std::unique_ptr<GenerationMapping> extra_map;
	std::unique_ptr<JitBacking> extra_stub_backing;
	std::unique_ptr<GenerationMapping> extra_stub_map;
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
};

struct Fault final {
	std::uint64_t pc, address;
	std::uint32_t access;
	bool operator==(const Fault &) const = default;
};

struct Result final {
	arm64::CpuState cpu{};
	std::vector<Fault> faults;
	std::vector<std::byte> memory;
	EngineEvent last{EngineEvent::continued};
	bool finished{};
	std::size_t blocks{};
	std::size_t max_block{};

	/* The run ended in a system call or an exit the guest sees, rather than
	 * by running out of blocks. */
	[[nodiscard]] bool done() const noexcept
	{
		return finished || (last != EngineEvent::continued &&
			last != EngineEvent::interrupt);
	}
};

struct Case final {
	arm64::CpuState cpu;
	std::vector<std::uint8_t> kinds;
	std::uint64_t seed;
	std::uint64_t pc_offset;
	bool faulting;
};

struct Snapshot final {
	std::uint64_t pc, sp, x[31];
	std::uint32_t nzcv;
	bool operator==(const Snapshot &) const = default;
};

[[nodiscard]] Snapshot snapshot_of(const arm64::CpuState &cpu)
{
	Snapshot s{};
	s.pc = cpu.pc;
	s.sp = cpu.sp;
	s.nzcv = cpu.nzcv;
	for (std::size_t i = 0; i < 31; ++i)
		s.x[i] = cpu.registers[i];
	return s;
}

using Observer = std::function<void(const arm64::CpuState &)>;

/* The last blocks of a run, for the failure report. */
struct BlockLog final {
	bool enabled{};
	std::vector<std::array<std::uint64_t, 5>> blocks;
} g_log;

[[nodiscard]] Result drive_on(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, std::uint64_t bias,
	Arena &arena, Case c, std::size_t blocks, const Observer &observer = {})
{
	Result r;
	c.cpu.pc = bias + c.pc_offset;
	for (std::size_t i = 0; i < arena.total(); ++i)
		if (i < c.kinds.size())
			arena.set_kind(i, c.kinds[i]);
	SplitMix fill{c.seed};
	arena.fill(fill);
	std::size_t faults_taken = 0;
	for (; r.blocks < blocks; ++r.blocks) {
		const auto before = c.cpu.pc;
		const auto out = run_once(
			executor, memory, generation, c.cpu);
		if (g_log.enabled)
			g_log.blocks.push_back({before,
				static_cast<std::uint64_t>(out.event), out.executed,
				c.cpu.pc, aot::stats().enters});
		r.max_block = std::max(r.max_block, out.executed);
		r.last = out.event;
		if (out.event == EngineEvent::continued ||
		    out.event == EngineEvent::interrupt) {
			if (observer)
				observer(c.cpu);
			continue;
		}
		arm64::restore_native_fp_environment(c.cpu);
		if (out.event == EngineEvent::system_call) {
			r.finished = true;
			break;
		}
		if (out.event != EngineEvent::memory_fault)
			break;
		const auto address = executor.fault_address();
		r.faults.push_back({c.cpu.pc, address,
			static_cast<std::uint32_t>(executor.fault_access())});
		const auto untagged = address & 0x00ff'ffff'ffff'ffffULL;
		if (untagged < arena.base() ||
		    untagged >= arena.base() + arena.total() * page_bytes ||
		    ++faults_taken > 8)
			break;
		const auto page = (untagged - arena.base()) / page_bytes;
		if (arena.kind(page) == 2)
			break;
		arena.set_kind(page, 2);
	}
	arm64::restore_native_fp_environment(c.cpu);
	r.cpu = c.cpu;
	r.memory = arena.snapshot();
	return r;
}

[[nodiscard]] Result drive(Environment &env, Arena &arena, Case c,
	std::size_t blocks, const Observer &observer = {})
{
	return drive_on(env.executor, env.memory, env.generation, env.bias,
		arena, std::move(c), blocks, observer);
}

void describe(const char *what, const Case &c)
{
	std::fprintf(stderr, "%s: seed %llx pc_offset %llx tbi %d faulting %d\n",
		what, static_cast<unsigned long long>(c.seed),
		static_cast<unsigned long long>(c.pc_offset),
		c.cpu.tbi_enabled ? 1 : 0, c.faulting ? 1 : 0);
}

/* Whether two runs ended in the same state, without a report. */
[[nodiscard]] bool identical(const Result &a, const Result &b)
{
	if (a.finished != b.finished || a.last != b.last ||
	    a.faults != b.faults || a.cpu.sp != b.cpu.sp ||
	    a.cpu.pc != b.cpu.pc || a.cpu.nzcv != b.cpu.nzcv ||
	    a.cpu.fpcr != b.cpu.fpcr || a.cpu.fpsr != b.cpu.fpsr ||
	    a.memory != b.memory)
		return false;
	for (std::size_t i = 0; i < a.cpu.registers.size(); ++i)
		if (a.cpu.registers[i] != b.cpu.registers[i])
			return false;
	return std::memcmp(a.cpu.vectors.data(), b.cpu.vectors.data(),
		sizeof(a.cpu.vectors)) == 0;
}

[[nodiscard]] bool same(const Result &a, const Result &b, const Case &c)
{
	bool ok = true;
	const auto say = [&](const char *what) {
		if (ok)
			describe(what, c);
		ok = false;
	};
	if (a.finished != b.finished || a.last != b.last)
		say("event");
	if (a.faults != b.faults)
		say("faults");
	for (std::size_t i = 0; i < a.cpu.registers.size(); ++i)
		if (a.cpu.registers[i] != b.cpu.registers[i]) {
			say("register");
			std::fprintf(stderr, "  x%zu ref %llx aot %llx\n", i,
				static_cast<unsigned long long>(a.cpu.registers[i]),
				static_cast<unsigned long long>(b.cpu.registers[i]));
		}
	if (a.cpu.sp != b.cpu.sp) {
		say("sp");
		std::fprintf(stderr, "  sp ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.sp),
			static_cast<unsigned long long>(b.cpu.sp));
	}
	if (a.cpu.tls != b.cpu.tls) {
		say("tls");
		std::fprintf(stderr, "  tls ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.tls),
			static_cast<unsigned long long>(b.cpu.tls));
	}
	if (a.cpu.pc != b.cpu.pc) {
		say("pc");
		std::fprintf(stderr, "  pc ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.pc),
			static_cast<unsigned long long>(b.cpu.pc));
	}
	if (a.cpu.nzcv != b.cpu.nzcv) {
		say("nzcv");
		std::fprintf(stderr, "  nzcv ref %x aot %x\n", a.cpu.nzcv,
			b.cpu.nzcv);
	}
	if (std::memcmp(a.cpu.vectors.data(), b.cpu.vectors.data(),
		    sizeof(a.cpu.vectors)) != 0)
		say("vectors");
	if (a.cpu.fpcr != b.cpu.fpcr || a.cpu.fpsr != b.cpu.fpsr) {
		say("fpcr/fpsr");
		std::fprintf(stderr, "  fpcr ref %x aot %x fpsr ref %x aot %x\n",
			a.cpu.fpcr, b.cpu.fpcr, a.cpu.fpsr, b.cpu.fpsr);
	}
	if (a.memory != b.memory)
		say("memory");
	return ok;
}

[[nodiscard]] Case make_case_impl(SplitMix &rng, std::uint64_t seed,
	std::uint64_t bias, std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	Case c{};
	c.seed = seed;
	c.pc_offset = pc_offset;
	c.faulting = faulting;
	c.cpu.tbi_enabled = tbi;
	c.kinds.assign(arena_pages + stack_pages, 2);
	/* The data pages and, in a faulting case, the stack pages are each
	 * unmapped, read-only or writable (a store to the stack that faults, a
	 * pre-index pair store that faults after the address was formed). */
	if (faulting)
		for (std::size_t i = 0; i < arena_pages + stack_pages; ++i) {
			const auto roll = rng.below(10);
			c.kinds[i] = roll < 2 ? 0 : roll < 4 ? 1 : 2;
		}
	const auto pointer = [&]() -> std::uint64_t {
		std::uint64_t page = rng.below(arena_pages);
		std::uint64_t offset = rng.below(page_bytes);
		if (rng.below(4) == 0)
			offset = page_bytes - 1 - rng.below(16);
		/* Half the pointers are 8-aligned: the atomics and ordered
		 * accesses take the engine's alignment path for the others. */
		else if (rng.below(2) == 0)
			offset &= ~std::uint64_t{7};
		std::uint64_t p = arena_base + page * page_bytes + offset;
		if (tbi && rng.below(2) != 0)
			p |= (rng.below(256) | 1) << 56;
		return p;
	};
	for (std::size_t i = 0; i < 29; ++i) {
		switch (rng.below(8)) {
		case 0:
		case 1:
		case 2:
			c.cpu.registers[i] = rng.below(65);
			break;
		case 3:
			c.cpu.registers[i] = rng.below(4097);
			break;
		case 4:
		case 5:
		case 6:
			c.cpu.registers[i] = pointer();
			break;
		default:
			c.cpu.registers[i] = rng.below(3) == 0 ? ~0ULL :
				bias + 0x1a000 + rng.below(0x2000);
		}
	}
	const auto stack_top = arena_base + (arena_pages + stack_pages) *
		page_bytes - 0x40;
	c.cpu.sp = stack_top;
	/* One case in eight starts with an SP that is not 16-byte aligned: the
	 * engine faults an SP-based access, so AOT code must not be entered. */
	if (rng.below(8) == 0)
		c.cpu.sp -= 8 * (1 + rng.below(3));
	c.cpu.registers[29] = stack_top;
	/* One case in five makes the frame pointer a stack address that is not
	 * 16-byte aligned: `mov sp, x29` and `add sp, x29, #n` then leave SP
	 * misaligned inside rewritten code, where the engine faults the next
	 * SP-based access. */
	if (rng.below(5) == 0)
		c.cpu.registers[29] = stack_top - 8 * (1 + rng.below(3)) -
			16 * rng.below(8);
	if (g_sites_image != nullptr) {
		/* A case that starts at an SP write takes the value from a stack
		 * address, aligned in one case in three. */
		const int source = sp_source(*g_sites_image, pc_offset);
		if (source >= 0 && source < 29) {
			const bool aligned = rng.below(3) == 0;
			c.cpu.registers[source] = stack_top - 16 * rng.below(8) -
				(aligned ? 0 : 8 * (1 + rng.below(3)));
		}
	}
	/* A thread pointer that points into guest data, so that the stack
	 * protector's canary load works and the function runs to its return. */
	c.cpu.tls = arena_base + rng.below(arena_pages) * page_bytes +
		0x40 * rng.below(32);
	c.cpu.registers[30] = bias + stub_offset;
	if (!g_cross_targets.empty()) {
		for (std::size_t i = 0; i < 29; ++i)
			if (rng.below(6) == 0)
				c.cpu.registers[i] = g_cross_targets[
					rng.below(g_cross_targets.size())];
		if (rng.below(4) == 0)
			c.cpu.registers[30] = g_cross_targets[
				rng.below(g_cross_targets.size())];
	}
	c.cpu.nzcv = static_cast<std::uint32_t>(rng.below(16)) << 28;
	for (auto &v : c.cpu.vectors)
		for (auto &b : v.bytes)
			b = static_cast<std::byte>(rng.next());
	/* Rounding mode, flush-to-zero and default NaN, which the engine and
	 * the rewritten code must honour alike. */
	c.cpu.fpcr = static_cast<std::uint32_t>(
		(rng.below(4) << 22) | (rng.below(2) << 24) | (rng.below(2) << 25));
	/* FIZ, AH and NEP, which rewritten code is not entered under. */
	if (rng.below(8) == 0)
		c.cpu.fpcr |= static_cast<std::uint32_t>(1 + rng.below(7));
	c.cpu.fpsr = 0;
	return c;
}

[[nodiscard]] Case make_case_from_seed(std::uint64_t seed,
	std::uint64_t bias, std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	SplitMix rng{seed};
	return make_case_impl(rng, seed, bias, pc_offset, faulting, tbi,
		arena_base);
}

[[nodiscard]] Case make_case(SplitMix &rng, std::uint64_t bias,
	std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	return make_case_from_seed(rng.next(), bias, pc_offset, faulting, tbi,
		arena_base);
}

[[nodiscard]] std::uint64_t arena_base(std::uint64_t bias)
{
	return bias + arena_offset;
}

/* Where an AOT run first leaves the reference's path: the reference single
 * steps and records every instruction's state; each state the AOT run
 * returns the engine at must be one of them, in order. */
[[nodiscard]] bool diagnose(const Case &c, const LibraryImage &image,
	std::uint64_t bias)
{
	constexpr std::uint64_t id = 5101;
	{
		/* The same case on fresh address spaces: a difference that
		 * goes away is state a former case left behind. */
		Environment fresh_ref{id + 10, bias, image};
		Environment fresh_aot{id + 11, bias, image};
		Arena fresh_ref_arena{fresh_ref.generation, arena_base(bias)};
		Arena fresh_aot_arena{fresh_aot.generation, arena_base(bias)};
		aot::set_enabled(false);
		const auto a = drive(fresh_ref, fresh_ref_arena, c, max_blocks);
		aot::set_enabled(true);
		const auto b = drive(fresh_aot, fresh_aot_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		std::fprintf(stderr, "fresh spaces: ref done %d event %d blocks %zu "
			"faults %zu, aot done %d event %d blocks %zu faults %zu\n",
			a.done(), static_cast<int>(a.last), a.blocks,
			a.faults.size(), b.done(), static_cast<int>(b.last),
			b.blocks, b.faults.size());
		(void)same(a, b, c);
	}
	Environment ref{id, bias, image};
	Environment test{id + 1, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena test_arena{test.generation, arena_base(bias)};
	std::vector<Snapshot> trace;
	aot::set_enabled(false);
	ref.executor.set_single_step(true);
	trace.push_back([&] {
		auto first = c.cpu;
		first.pc = bias + c.pc_offset;
		return snapshot_of(first);
	}());
	(void)drive(ref, ref_arena, c, 300000, [&](const arm64::CpuState &cpu) {
		trace.push_back(snapshot_of(cpu));
	});
	aot::set_enabled(true);
	std::size_t at = 0;
	std::size_t exits = 0;
	bool reported = false;
	g_log.enabled = true;
	g_log.blocks.clear();
	const auto ended = drive(test, test_arena, c, 4000,
		[&](const arm64::CpuState &cpu) {
		if (reported)
			return;
		++exits;
		const auto now = snapshot_of(cpu);
		for (std::size_t i = at; i < trace.size(); ++i)
			if (trace[i] == now) {
				at = i;
				return;
			}
		reported = true;
		std::fprintf(stderr, "first state off the reference path after "
			"exit %zu (last matching reference step %zu of %zu)\n",
			exits, at, trace.size());
		const auto show = [](const char *tag, const Snapshot &s) {
			std::fprintf(stderr, "  %s pc %llx sp %llx nzcv %x\n", tag,
				static_cast<unsigned long long>(s.pc),
				static_cast<unsigned long long>(s.sp), s.nzcv);
		};
		show("aot", now);
		for (std::size_t i = at; i < std::min(trace.size(), at + 6); ++i) {
			show("ref", trace[i]);
			for (std::size_t r = 0; r < 31; ++r)
				if (trace[i].x[r] != now.x[r])
					std::fprintf(stderr, "    x%zu ref %llx aot %llx\n",
						r, static_cast<unsigned long long>(trace[i].x[r]),
						static_cast<unsigned long long>(now.x[r]));
		}
	});
	aot::set_enabled(false);
	g_log.enabled = false;
	for (std::size_t i = g_log.blocks.size() > 6 ? g_log.blocks.size() - 6 : 0;
	     i < g_log.blocks.size(); ++i)
		std::fprintf(stderr, "  block %zu: pc %llx event %llu executed %llu "
			"-> pc %llx enters %llu\n", i,
			static_cast<unsigned long long>(g_log.blocks[i][0]),
			static_cast<unsigned long long>(g_log.blocks[i][1]),
			static_cast<unsigned long long>(g_log.blocks[i][2]),
			static_cast<unsigned long long>(g_log.blocks[i][3]),
			static_cast<unsigned long long>(g_log.blocks[i][4]));
	std::fprintf(stderr, "aot run: event %d blocks %zu faults %zu done %d "
		"pc %llx\n", static_cast<int>(ended.last), ended.blocks,
		ended.faults.size(), ended.done(),
		static_cast<unsigned long long>(ended.cpu.pc));
	for (const auto &f : ended.faults)
		std::fprintf(stderr, "  fault pc %llx address %llx access %u\n",
			static_cast<unsigned long long>(f.pc),
			static_cast<unsigned long long>(f.address), f.access);
	if (!reported) {
		std::fprintf(stderr, "every AOT exit state is on the reference path "
			"(%zu exits, reference step %zu of %zu)\n", exits, at,
			trace.size());
		const auto show = [](const char *tag, const Snapshot &s) {
			std::fprintf(stderr, "  %s pc %llx x8 %llx x9 %llx x10 %llx "
				"x11 %llx\n", tag, static_cast<unsigned long long>(s.pc),
				static_cast<unsigned long long>(s.x[8]),
				static_cast<unsigned long long>(s.x[9]),
				static_cast<unsigned long long>(s.x[10]),
				static_cast<unsigned long long>(s.x[11]));
		};
		show("ref end", trace.back());
		show("ref at", trace[at]);
	}
	return !reported;
}

struct Totals final {
	std::size_t cases{}, compared{}, skipped{}, faulted{}, fault_events{},
		finished{}, nondeterministic{}, slow{};
};

struct Config final {
	std::string libraries, functions, sites;
	/* Run only the library with this index (-1: all). */
	long library{-1};
	/* At most this many functions of a library are sampled. */
	std::size_t sample{400};
	/* seed:pc_offset:faulting:tbi of one case to run alone. */
	std::string only;
	std::size_t starts{12}, interior{8}, sp_sites{6}, site_cases{4};
	std::uint64_t master{0x5eed};
};

[[nodiscard]] Config config(int argc, char **argv)
{
	Config c;
	c.libraries = AOT_TEST_LIBRARIES;
	c.functions = AOT_TEST_FUNCTIONS;
	c.sites = AOT_TEST_SITES;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string key{argv[i]};
		if (key == "--starts")
			c.starts = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--interior")
			c.interior = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--sp-sites")
			c.sp_sites = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--site-cases")
			c.site_cases = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--master")
			c.master = std::strtoull(argv[i + 1], nullptr, 0);
		else if (key == "--case")
			c.only = argv[i + 1];
		else if (key == "--library")
			c.library = std::strtol(argv[i + 1], nullptr, 0);
		else if (key == "--sample")
			c.sample = std::strtoul(argv[i + 1], nullptr, 0);
	}
	return c;
}

/* Every function of the library, from its first instruction and from random
 * interior ones, and a sample of every kind of instruction the rewriter
 * inlined, against random states and fault patterns. */
Totals differential(const Config &config, const LibraryImage &image,
	const std::vector<Function> &functions, const std::vector<Site> &sites)
{
	constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
	Environment ref{4101, bias, image};
	Environment aot_env{4102, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena aot_arena{aot_env.generation, arena_base(bias)};
	SplitMix rng{config.master};
	Totals t;
	aot::reset_stats();
	const auto one = [&](std::uint64_t pc, const char *kind) {
		const bool faulting = rng.below(3) == 0;
		const bool tbi = rng.below(4) != 0;
		const auto c = make_case(rng, bias, pc, faulting, tbi,
			arena_base(bias));
		if (std::getenv("AOT_TRACE") != nullptr) {
			describe("reference", c);
			std::fflush(stderr);
		}
		aot::set_enabled(false);
		const auto want = drive(ref, ref_arena, c, max_blocks);
		++t.cases;
		if (!want.done()) {
			++t.skipped;
			return;
		}
		if (std::getenv("AOT_TRACE") != nullptr) {
			describe("running", c);
			std::fflush(stderr);
		}
		aot::set_enabled(true);
		const auto got = drive(aot_env, aot_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!identical(want, got)) {
			/* The counter registers and the like differ between two runs
			 * of the same code: if the engine disagrees with itself the
			 * case says nothing. */
			aot::set_enabled(false);
			const auto again = drive(ref, ref_arena, c, max_blocks);
			if (!identical(want, again)) {
				++t.skipped;
				++t.nondeterministic;
				return;
			}
		}
		if (!same(want, got, c)) {
			std::fprintf(stderr, "  start %llx (%s)\n",
				static_cast<unsigned long long>(pc), kind);
			const bool consistent = diagnose(c, image, bias);
			/* An AOT run that is still going after the limit but has
			 * left the engine's path nowhere is a run that steps a
			 * great deal, not a wrong one. */
			if (consistent && !got.done()) {
				++t.skipped;
				++t.slow;
				return;
			}
			require(false);
		}
		++t.compared;
		t.finished += want.finished ? 1 : 0;
		t.faulted += faulting ? 1 : 0;
		t.fault_events += want.faults.size();
	};
	for (const auto &function : functions) {
		for (std::size_t i = 0; i < config.starts; ++i)
			one(function.start, "function");
		for (std::size_t i = 0; i < config.interior; ++i)
			one(function.start + 4 * rng.below(
				(function.end - function.start) / 4), "interior");
		/* Every instruction that writes SP from a register, from itself,
		 * with that register aligned or not. */
		for (auto at = function.start; at < function.end; at += 4)
			if (sp_source(image, at) >= 0)
				for (std::size_t i = 0; i < config.sp_sites; ++i)
					one(at, "sp write");
	}
	for (const auto &site : sites)
		for (std::size_t i = 0; i < config.site_cases; ++i)
			one(site.offset, site.kind.c_str());
	return t;
}

/* A run of the same case on a fresh environment, for the scenarios below. */
struct Fixture final {
	static constexpr std::uint64_t bias = 0x7133'0000'0000ULL;
	Fixture(unsigned long generation, const LibraryImage &image,
		std::uint64_t bias_ = bias)
		: env{generation, bias_, image}, arena{generation, arena_base(bias_)}
	{
	}
	Environment env;
	Arena arena;
};

void default_off_and_single_step(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	require(!aot_runtime::enabled());
	SplitMix rng{7};
	const auto bias = Fixture::bias;
	Fixture ref{4201, image}, one{4202, image};
	for (std::size_t f = 0; f < functions.size(); ++f) {
		const auto c = make_case(rng, bias, functions[f].start, false,
			true, arena_base(bias));
		aot::reset_stats();
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(aot::stats().enters == 0);
		if (!want.finished || want.blocks > 40)
			continue;
		/* Enabled, but single-stepping: the engine runs one
		 * instruction a block and the AOT code is never entered. */
		aot::set_enabled(true);
		one.env.executor.set_single_step(true);
		const auto got = drive(one.env, one.arena, c, 400000);
		one.env.executor.set_single_step(false);
		aot::set_enabled(false);
		require(aot::stats().enters == 0);
		require(got.finished);
		require(same(want, got, c));
		return;
	}
	require(false);
}

void preemption_is_bounded(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	/* Any function that loops long on a large count: the blocks the engine
	 * returns from stay short, so an interrupt waits for at most one. */
	SplitMix rng{11};
	const auto bias = Fixture::bias;
	Fixture ref{4211, image}, fast{4212, image};
	std::size_t longest = 0;
	std::size_t entries = 0;
	for (std::size_t f = 0; f < functions.size(); ++f) {
		auto c = make_case(rng, bias, functions[f].start, false, true,
			arena_base(bias));
		for (std::size_t i = 0; i < 8; ++i)
			c.cpu.registers[i] = rng.below(2) ? 3000 + rng.below(1000) :
				c.cpu.registers[i];
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::reset_stats();
		aot::set_enabled(true);
		const auto got = drive(fast.env, fast.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, got, c));
		longest = std::max(longest, got.max_block);
		entries += aot::stats().enters;
	}
	require(entries > 0);
	/* One entry's budget is 4096 instructions; a check at a loop header
	 * or function entry lets a few more through. */
	require(longest <= 4096 + 1024);
}

void two_load_biases(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{13};
	constexpr std::uint64_t bias_a = 0x7143'0000'0000ULL;
	constexpr std::uint64_t bias_b = 0x7155'1234'0000ULL & ~0xfffULL;
	Fixture ref_a{4221, image, bias_a}, ref_b{4222, image, bias_b};
	Fixture a{4223, image, bias_a}, b{4224, image, bias_b};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); f += 3) {
		for (int which = 0; which < 2; ++which) {
			const auto bias = which ? bias_b : bias_a;
			const auto c = make_case(rng, bias, functions[f].start,
				false, true, arena_base(bias));
			aot::set_enabled(false);
			const auto want = drive(which ? ref_b.env : ref_a.env,
				which ? ref_b.arena : ref_a.arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto got = drive(which ? b.env : a.env,
				which ? b.arena : a.arena, c, aot_max_blocks);
			aot::set_enabled(false);
			/* Pointers differ by bias between the two, so each is
			 * compared with its own reference. */
			require(same(want, got, c));
			++compared;
		}
	}
	require(compared > 20);
	const auto st = aot::stats();
	require(st.enters > 0);
	require(st.poisoned == 0);
}

/* The same bytes at a covered address are not what the AOT code stands for
 * when a page differs: no entry, the engine runs it as usual. */
void other_bytes_fall_back(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	LibraryImage other = image;
	/* Change one instruction of every code page: a NOP in place of the
	 * first word of each function, which the engine will run. */
	for (const auto &f : functions) {
		const std::uint32_t nop = 0xd503201fU;
		std::memcpy(&other.bytes[f.start + 4], &nop, 4);
	}
	SplitMix rng{17};
	const auto bias = Fixture::bias;
	Fixture ref{4231, other}, got{4232, other};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); f += 4) {
		const auto c = make_case(rng, bias, functions[f].start, false,
			true, arena_base(bias));
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		++compared;
	}
	require(compared > 10);
	require(aot::stats().enters == 0);
}

/* A guest that rewrites covered text after the AOT code was entered: the
 * change is seen at the next entry, which falls back for good. */
void changed_text_falls_back_for_good(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{19};
	const auto bias = Fixture::bias;
	Fixture ref{4241, image}, got{4242, image};
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	aot::reset_stats();
	aot::set_enabled(true);
	const auto first = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	const auto entered = aot::stats().enters;
	require(entered > 0);
	require(first.done());

	/* Rewrite the first instruction of a function on another code page
	 * in both guests and tell each address space its code changed. */
	const std::uint32_t patched = 0xd2800f00U; /* mov x0, #0x78 */
	std::uint64_t offset = 0;
	for (const auto &f : functions)
		if ((f.start >> 12) != (fn.start >> 12)) {
			offset = f.start;
			break;
		}
	require(offset != 0);
	for (auto *e : {&ref.env, &got.env}) {
		e->backing.put32(offset, patched);
		e->memory.invalidate_code(e->bias + offset);
	}
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, have, c));
	const auto st = aot::stats();
	require(st.poisoned == 1);
	require(st.enters == entered);
	/* ... and stays on the engine: a second run enters nothing either. */
	aot::set_enabled(true);
	const auto again = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, again, c));
	require(aot::stats().enters == entered);
}

/* A loop over a buffer takes the inline hit path: adler32_z over 40000 bytes
 * (the library's own checksum loop, 0xd020), tagged or not, steps only for
 * the pages it first touches. */
void inline_hit_path_is_taken(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	constexpr std::uint64_t adler32_z = 0xd020;
	bool found = false;
	for (const auto &f : functions)
		found = found || f.start == adler32_z;
	require(found);
	const auto bias = Fixture::bias;
	for (int tagged = 0; tagged < 2; ++tagged) {
		Fixture ref{static_cast<unsigned long>(4251 + 2 * tagged), image},
			got{static_cast<unsigned long>(4252 + 2 * tagged), image};
		SplitMix rng{23};
		auto c = make_case(rng, bias, adler32_z, false, true,
			arena_base(bias));
		const std::uint64_t tag = tagged ? 0xb4ULL << 56 : 0;
		c.cpu.registers[0] = 1;
		c.cpu.registers[1] = tag | arena_base(bias);
		c.cpu.registers[2] = 40000;
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		aot::reset_stats();
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "hit path (%s): enters %llu steps %llu blocks %zu\n",
			tagged ? "tagged" : "untagged",
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps), have.blocks);
		require(st.enters > 0);
		require(st.steps < 100);
	}
}

/* Every access that leaves the page of the last one refills, and the
 * refill callback leaves every register and flag the thunk saves scrambled:
 * the results must still be the engine's. */
void refill_keeps_state(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{29};
	const auto bias = Fixture::bias;
	Fixture ref{4261, image}, got{4262, image};
	aot::reset_stats();
	aot::set_refill_stress(true);
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); ++f)
		for (int which = 0; which < 3; ++which) {
			const auto pc = which == 0 ? functions[f].start :
				functions[f].start + 4 * rng.below(
					(functions[f].end - functions[f].start) / 4);
			const auto c = make_case(rng, bias, pc, rng.below(4) == 0,
				true, arena_base(bias));
			aot::set_enabled(false);
			const auto want = drive(ref.env, ref.arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto have = drive(got.env, got.arena, c,
				aot_max_blocks);
			aot::set_enabled(false);
			require(same(want, have, c));
			++compared;
		}
	aot::set_refill_stress(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "refill stress: %zu cases; enters %llu refills %llu "
		"refill failures %llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refills),
		static_cast<unsigned long long>(st.refill_failures),
		static_cast<unsigned long long>(st.steps));
	require(compared > 20);
	require(st.refills > compared);
}

/* A library whose last code page is not mapped yet: the instance cannot be
 * verified, so nothing is entered and the engine runs everything as usual;
 * once the page is mapped the code is entered. */
void partially_mapped_library(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	std::uint64_t code_end = 0;
	for (const auto &f : functions)
		code_end = std::max(code_end, f.end);
	const auto last_page = (code_end - 1) / page_bytes;
	require(last_page >= 1);
	std::vector<Function> below;
	for (const auto &f : functions)
		if (f.end <= last_page * page_bytes)
			below.push_back(f);
	require(below.size() > 8);
	SplitMix rng{31};
	const auto bias = Fixture::bias;
	Environment ref{4281, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Environment got{4282, bias, image, last_page};
	Arena got_arena{got.generation, arena_base(bias)};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < below.size() && compared < 6; ++f) {
		const auto c = make_case(rng, bias, below[f].start, false, true,
			arena_base(bias));
		aot::set_enabled(false);
		auto want = drive(ref, ref_arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		/* The code ran with the tail of the library missing: only
		 * calls into the missing page differ from a full mapping, so
		 * compare cases that never fault on an instruction fetch. */
		if (have.faults.empty())
			require(same(want, have, c));
		++compared;
	}
	require(compared > 3);
	auto st = aot::stats();
	require(st.enters == 0);
	require(st.unreadable > 0);
	/* The loader maps the rest: the next verification that is due binds
	 * the library and the code is entered. */
	got.map_rest(image);
	std::size_t attempts = 0;
	while (aot::stats().enters == 0 && attempts < 4000) {
		const auto c = make_case(rng, bias,
			below[attempts % below.size()].start, false, true,
			arena_base(bias));
		aot::set_enabled(true);
		(void)drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		++attempts;
	}
	st = aot::stats();
	std::fprintf(stderr, "partial mapping: after the rest was mapped, entered "
		"after %zu cases; unreadable %llu\n", attempts,
		static_cast<unsigned long long>(st.unreadable));
	require(st.enters > 0);
	require(st.poisoned == 0);
}

/* Two vCPUs of one address space run rewritten code at once, each with its
 * own binder, TLB and arena; every case ends as the engine's would. */
void two_vcpus(const LibraryImage &image, const std::vector<Function> &functions)
{
	constexpr std::uint64_t bias = 0x7163'0000'0000ULL;
	constexpr std::size_t per_thread = 120;
	const std::uint64_t base[2] = {arena_base(bias),
		arena_base(bias) + 0x40'0000};
	Environment ref{4291, bias, image};
	Arena ref_arena[2] = {{ref.generation, base[0]},
		{ref.generation, base[1]}};
	Environment env{4292, bias, image};
	Arena arena[2] = {{env.generation, base[0]}, {env.generation, base[1]}};
	Vcpu second;
	SplitMix rng{37};
	std::vector<Case> cases[2];
	std::vector<Result> want[2], have[2];
	aot::set_enabled(false);
	for (int t = 0; t < 2; ++t)
		for (std::size_t i = 0; i < per_thread; ++i) {
			const auto &f = functions[rng.below(functions.size())];
			cases[t].push_back(make_case(rng, bias, f.start +
				4 * rng.below((f.end - f.start) / 4), rng.below(3) == 0,
				rng.below(4) != 0, base[t]));
			want[t].push_back(drive_on(ref.executor, ref.memory,
				ref.generation, bias, ref_arena[t], cases[t].back(),
				max_blocks));
		}
	aot::reset_stats();
	aot::set_enabled(true);
	std::atomic<bool> go{false};
	std::thread threads[2];
	for (int t = 0; t < 2; ++t)
		threads[t] = std::thread{[&, t] {
			auto &executor = t == 0 ? env.executor : second.executor;
			auto &memory = t == 0 ? env.memory : second.memory;
			while (!go.load(std::memory_order_acquire))
				std::this_thread::yield();
			for (const auto &c : cases[t])
				have[t].push_back(drive_on(executor, memory,
					env.generation, bias, arena[t], c,
					aot_max_blocks));
		}};
	go.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	aot::set_enabled(false);
	std::size_t compared = 0;
	for (int t = 0; t < 2; ++t)
		for (std::size_t i = 0; i < per_thread; ++i) {
			if (!want[t][i].done())
				continue;
			require(same(want[t][i], have[t][i], cases[t][i]));
			++compared;
		}
	const auto st = aot::stats();
	std::fprintf(stderr, "two vCPUs: %zu cases compared; enters %llu binds "
		"%llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.binds),
		static_cast<unsigned long long>(st.steps));
	require(compared > per_thread);
	require(st.enters > compared / 2);
	require(st.binds >= 2);
	require(st.poisoned == 0);
}

/* Two libraries in one address space, each reaching the other through a
 * register (as a call through a PLT does): the branch stays in rewritten
 * code when the other library is verified. */
void cross_library_branches(const LibraryImage &image,
	const std::vector<Function> &functions, const LibraryImage &other,
	const std::vector<Function> &other_functions)
{
	constexpr std::uint64_t bias = 0x7173'0000'0000ULL;
	constexpr std::uint64_t other_bias = 0x7183'0000'0000ULL;
	Environment ref{4301, bias, image};
	Environment got{4302, bias, image};
	ref.add_library(other_bias, other);
	got.add_library(other_bias, other);
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena got_arena{got.generation, arena_base(bias)};
	g_cross_targets.clear();
	for (const auto &f : other_functions)
		g_cross_targets.push_back(other_bias + f.start);
	SplitMix rng{41};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t round = 0; round < 3; ++round)
		for (std::size_t i = 0; i < functions.size(); ++i) {
			/* Some cases start in the other library, so that its
			 * instance is verified before library one branches. */
			const bool in_other = rng.below(4) == 0;
			const auto &f = in_other ?
				other_functions[rng.below(other_functions.size())] :
				functions[i];
			const auto at = in_other ? other_bias : bias;
			auto c = make_case(rng, at, f.start, false, true,
				arena_base(bias));
			c.pc_offset = f.start;
			aot::set_enabled(false);
			const auto want = drive_on(ref.executor, ref.memory,
				ref.generation, at, ref_arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto have = drive_on(got.executor, got.memory,
				got.generation, at, got_arena, c, aot_max_blocks);
			aot::set_enabled(false);
			require(same(want, have, c));
			++compared;
		}
	g_cross_targets.clear();
	const auto st = aot::stats();
	std::fprintf(stderr, "cross-library: %zu cases; enters %llu steps %llu "
		"cross hits %llu cross misses %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps),
		static_cast<unsigned long long>(st.cross_hits),
		static_cast<unsigned long long>(st.cross_misses));
	require(compared > 20);
	require(st.cross_hits > 0);
	require(st.poisoned == 0);
}

/* A loop whose backward branch lands inside another unwind range
 * (tools/aot/fixture.py): every pass through it is charged against the
 * entry's budget, so a long run comes back to the engine about every 4096
 * instructions and ends where the engine's run ends. */
void loop_through_another_range(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	/* The two ranges of the loop, the PLT stub after them and the
	 * instruction forms. */
	require(functions.size() >= 3);
	require(functions[0].start == 0x1000);
	const auto bias = Fixture::bias;
	Fixture ref{4311, image}, got{4312, image};
	SplitMix rng{43};
	auto c = make_case(rng, bias, functions[0].start, false, true,
		arena_base(bias));
	c.cpu.registers[0] = 0;
	c.cpu.registers[1] = 0;
	c.cpu.registers[3] = 50000;
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.finished);
	require(want.cpu.registers[0] == 50000);
	aot::reset_stats();
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "loop through another range: %llu entries, longest "
		"block %zu instructions, %zu blocks\n",
		static_cast<unsigned long long>(st.enters), have.max_block,
		have.blocks);
	require(st.enters >= 40);
	require(have.max_block <= 4096 + 64);
}

/* The instructions between two polls are the instructions that ran: the
 * framework of a diagnostics build counts each one it starts. */
constexpr std::uint64_t entry_bound = 4096 + 32 + 2; /* RUN_LIMIT, rewrite.py */

/* A state rewritten code may be entered with: SP aligned and none of the
 * FPCR controls the entry refuses. */
void enterable(Case &c)
{
	c.cpu.sp &= ~0xfULL;
	c.cpu.fpcr &= ~static_cast<std::uint32_t>(arm64::fpcr_afp_controls);
}

/* Forty instructions and a RET that returns to the start of the run: a lap
 * the budget used to charge four for (libz 0x11638..0x116d8). */
void return_loop_is_bounded(const LibraryImage &image)
{
	constexpr std::uint64_t first = 0x11638, last = 0x116d8;
	std::uint32_t word;
	std::memcpy(&word, &image.bytes[last], 4);
	require(word == 0xd65f03c0U); /* ret */
	const auto bias = Fixture::bias;
	Fixture got{4321, image};
	SplitMix rng{47};
	auto c = make_case(rng, bias, first, false, true, arena_base(bias));
	c.cpu.registers[30] = bias + first;
	enterable(c);
	aot::reset_stats();
	aot::set_enabled(true);
	const auto run = drive(got.env, got.arena, c, 60);
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "return loop: %llu entries, longest entry %llu "
		"instructions (%zu blocks)\n",
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.max_retired), run.blocks);
	require(st.enters >= 20);
	require(st.max_retired > 1000);
	require(st.max_retired <= entry_bound);
}

/* STP X29, X30, [SP, #-32]! (libz 0x8028) entered with SP = 8 (mod 16): the
 * engine faults without writing, so rewritten code is not entered. */
void misaligned_sp_at_a_pair_store(const LibraryImage &image)
{
	constexpr std::uint64_t at = 0x8028;
	std::uint32_t word;
	std::memcpy(&word, &image.bytes[at], 4);
	require(word == 0xa9be7bfdU);
	const auto bias = Fixture::bias;
	Fixture ref{4331, image}, got{4332, image};
	SplitMix rng{53};
	auto c = make_case(rng, bias, at, false, true, arena_base(bias));
	c.cpu.sp = (c.cpu.sp & ~0xfULL) - 8;
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(!want.faults.empty());
	require(same(want, have, c));
	const auto st = aot::stats();
	require(st.refusals[1] > 0);
}

/* An exclusive monitor the gadget engine opened (LDXR, then a branch into
 * the library) is the engine's to complete: nothing is entered. */
void open_exclusive_is_not_entered(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto bias = Fixture::bias;
	Fixture ref{4341, image}, got{4342, image};
	SplitMix rng{59};
	auto c = make_case(rng, bias, stub_offset + 4, false, true,
		arena_base(bias));
	enterable(c);
	c.cpu.registers[1] = arena_base(bias) + 0x40;
	c.cpu.registers[2] = bias + functions[3].start;
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, have, c));
	const auto st = aot::stats();
	require(st.refusals[2] > 0);
	require(st.enters == 0);
}

/* A task's floating-point state the runtime has not loaded yet
 * (CpuState::defer_fp_load) keeps its FPCR aside: the register file's
 * `fpcr` is stale until the first access, and the entry conditions must
 * read the guest's. An FPCR with FIZ, AH or NEP is the engine's alone;
 * the same state without them is entered, and either way the result is
 * the engine's. */
void deferred_fp_state_is_judged_by_its_guest_fpcr(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto bias = Fixture::bias;
	for (const bool afp : {true, false}) {
		Fixture ref{afp ? 4361UL : 4363UL, image},
			got{afp ? 4362UL : 4364UL, image};
		SplitMix rng{67};
		auto c = make_case(rng, bias, functions[3].start, false, true,
			arena_base(bias));
		enterable(c);
		std::vector<std::byte> saved(sizeof(c.cpu.vectors));
		std::memcpy(saved.data(), c.cpu.vectors.data(), saved.size());
		/* AH (bit 1) in the saved FPCR, the register file's own zero. */
		const std::uint32_t guest_fpcr = (c.cpu.fpcr & ~0x7U) |
			(afp ? 0x2U : 0U);
		c.cpu.defer_fp_load(saved.data(), guest_fpcr, 0, ~0U);
		c.cpu.fpcr = 0;
		c.cpu.fpsr = 0;
		require(!c.cpu.fp_loaded);
		aot::reset_stats();
		aot::set_enabled(false);
		auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::set_enabled(true);
		auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		/* Whether a run reached the state or not, compare the guest's. */
		want.cpu.access_fp_state();
		have.cpu.access_fp_state();
		require(want.done());
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "deferred FP state, AH %d: %llu entries, "
			"%llu fp refusals\n", afp ? 1 : 0,
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.refusals[3]));
		if (afp) {
			require(st.refusals[3] > 0);
			require(st.enters == 0);
		} else {
			require(st.refusals[3] == 0);
			require(st.enters > 0);
		}
	}
}

/* Another vCPU's code change, published in the window between the
 * dispatcher reading the sequences and AOT looking its entry up. */
struct WindowChange final {
	Environment *env;
	std::uint64_t offset;
	std::uint32_t word;
	bool fired;
};

void publish_in_the_window(void *context)
{
	auto &change = *static_cast<WindowChange *>(context);
	if (change.fired)
		return;
	change.fired = true;
	change.env->backing.put32(change.offset, change.word);
	change.env->memory.invalidate_code(change.env->bias + change.offset);
}

/* `isb; mov x0, #1; ret` is entered after a change to the MOV was published
 * in the window. The instance was verified before the change and the
 * engine's ISB sees the change (it compares with the sequence the dispatcher
 * read), so the run must leave at the ISB and look the MOV up again at the
 * new sequence, which finds it changed; rewritten code must not run it. */
void code_change_in_the_dispatch_window(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto form = functions.back();
	std::uint32_t words[2];
	std::memcpy(words, &image.bytes[form.start], sizeof(words));
	require(words[0] == 0xd5033fdfU); /* isb */
	require(words[1] == 0xd2800020U); /* mov x0, #1 */
	const std::uint32_t patched = 0xd2800f00U; /* mov x0, #0x78 */
	const auto bias = Fixture::bias;
	Fixture ref{4371, image}, got{4372, image};
	SplitMix rng{71};
	auto c = make_case(rng, bias, form.start, false, true, arena_base(bias));
	enterable(c);
	/* The engine, running the changed code. */
	ref.env.backing.put32(form.start + 4, patched);
	ref.env.memory.invalidate_code(ref.env.bias + form.start + 4);
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.finished);
	require(want.cpu.registers[0] == 0x78);
	/* AOT code verified against the original text, then the window. */
	aot::reset_stats();
	aot::set_enabled(true);
	const auto warm = drive(got.env, got.arena, c, aot_max_blocks);
	require(warm.finished);
	require(warm.cpu.registers[0] == 1);
	const auto entered = aot::stats().enters;
	require(entered > 0);
	WindowChange change{&got.env, form.start + 4, patched, false};
	aot::set_run_hook(publish_in_the_window, &change);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_run_hook(nullptr, nullptr);
	aot::set_enabled(false);
	require(change.fired);
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "code change in the dispatch window: %llu entries "
		"before, %llu after\n", static_cast<unsigned long long>(entered),
		static_cast<unsigned long long>(st.enters));
	/* Entered once, at the ISB; the changed page is no library page. */
	require(st.enters == entered + 1);
}

/* The 64-bit page digests find a library; they authorize nothing. Two words
 * of a page are changed (the first to something the engine will run, the
 * second to make the digest come out the same), and the page is not the one
 * the framework was built from. */
[[nodiscard]] std::uint64_t digest_of(const std::byte *page)
{
	std::uint32_t low = 0, high = 0x9e3779b9U;
	for (std::size_t offset = 0; offset < page_bytes; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, 8);
		low = __crc32cd(low, word);
		high = __crc32cd(high, word);
	}
	return (std::uint64_t{high} << 32) | low;
}

/* The CRC of a page with zero initial state, which is linear in the page. */
[[nodiscard]] std::uint32_t crc_of(const std::byte *page)
{
	std::uint32_t crc = 0;
	for (std::size_t offset = 0; offset < page_bytes; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, 8);
		crc = __crc32cd(crc, word);
	}
	return crc;
}

void equal_digest_is_not_equal_code(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	LibraryImage other = image;
	const auto start = functions[2].start;
	const auto page = start / page_bytes * page_bytes;
	const auto fixup = page + page_bytes - 4;
	require(start + 4 < fixup);
	std::array<std::byte, page_bytes> original;
	std::memcpy(original.data(), &image.bytes[page], page_bytes);
	/* mov x0, #120 in place of the function's first word. */
	const std::uint32_t mov = 0xd2800f00U;
	std::memcpy(&other.bytes[start], &mov, 4);
	/* The CRC is linear: find the word at the end of the page that cancels
	 * the change (its effect is a bijection of 32-bit values). */
	std::array<std::byte, page_bytes> work;
	std::memcpy(work.data(), &other.bytes[page], page_bytes);
	const auto wanted = crc_of(original.data());
	std::array<std::uint32_t, 32> image_of_bit{};
	const auto base_crc = crc_of(work.data());
	for (unsigned bit = 0; bit < 32; ++bit) {
		std::array<std::byte, page_bytes> flipped = work;
		std::uint32_t value;
		std::memcpy(&value, &flipped[page_bytes - 4], 4);
		value ^= 1U << bit;
		std::memcpy(&flipped[page_bytes - 4], &value, 4);
		image_of_bit[bit] = crc_of(flipped.data()) ^ base_crc;
	}
	/* Solve: choose bits so that the xor of their images is base ^ wanted. */
	std::uint32_t target = base_crc ^ wanted;
	std::array<std::uint32_t, 32> rows = image_of_bit;
	std::array<std::uint32_t, 32> combo{};
	for (unsigned i = 0; i < 32; ++i)
		combo[i] = 1U << i;
	std::uint32_t chosen = 0;
	for (unsigned bit = 0; bit < 32; ++bit) {
		/* Find a row with this bit as pivot. */
		unsigned pivot = bit;
		while (pivot < 32 && ((rows[pivot] >> bit) & 1U) == 0)
			++pivot;
		require(pivot < 32);
		std::swap(rows[bit], rows[pivot]);
		std::swap(combo[bit], combo[pivot]);
		for (unsigned other_row = 0; other_row < 32; ++other_row)
			if (other_row != bit && ((rows[other_row] >> bit) & 1U) != 0) {
				rows[other_row] ^= rows[bit];
				combo[other_row] ^= combo[bit];
			}
	}
	for (unsigned bit = 0; bit < 32; ++bit)
		if ((target >> bit) & 1U) {
			/* rows[bit] is the unit vector for bit. */
			chosen ^= combo[bit];
		}
	std::uint32_t value;
	std::memcpy(&value, &work[page_bytes - 4], 4);
	value ^= chosen;
	std::memcpy(&work[page_bytes - 4], &value, 4);
	std::memcpy(&other.bytes[fixup], &value, 4);
	require(digest_of(work.data()) == digest_of(original.data()));
	require(std::memcmp(work.data(), original.data(), page_bytes) != 0);

	SplitMix rng{61};
	const auto bias = Fixture::bias;
	Fixture ref{4351, other}, got{4352, other};
	auto c = make_case(rng, bias, start, false, true, arena_base(bias));
	enterable(c);
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(want.done());
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "equal digest, different code: %llu entries, "
		"poisoned %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.poisoned));
	require(st.poisoned == 1);
	require(st.enters == 0);
}

} // namespace

/* A host fault inside rewritten code is a bug; say where before dying. */
void report_host_fault(int signal, siginfo_t *info, void *context)
{
	const auto *uc = static_cast<const ucontext_t *>(context);
	char text[512];
	const auto &ss = uc->uc_mcontext->__ss;
	const int n = std::snprintf(text, sizeof(text),
		"HOST FAULT signal %d address %p pc %llx x0 %llx x24 %llx x25 %llx "
		"x26 %llx x27 %llx x28 %llx lr %llx\n", signal, info->si_addr,
		static_cast<unsigned long long>(ss.__pc),
		static_cast<unsigned long long>(ss.__x[0]),
		static_cast<unsigned long long>(ss.__x[24]),
		static_cast<unsigned long long>(ss.__x[25]),
		static_cast<unsigned long long>(ss.__x[26]),
		static_cast<unsigned long long>(ss.__x[27]),
		static_cast<unsigned long long>(ss.__x[28]),
		static_cast<unsigned long long>(ss.__lr));
	(void)!write(2, text, static_cast<std::size_t>(n));
	_exit(99);
}

int main(int argc, char **argv)
{
	if (std::getenv("AOT_TRACE") != nullptr) {
		struct sigaction action {};
		action.sa_sigaction = report_host_fault;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		sigaction(SIGBUS, &action, nullptr);
		sigaction(SIGSEGV, &action, nullptr);
	}
	const uml::testing::RuntimeTestRoute runtime_route;
	const auto cfg = config(argc, argv);
	const auto libraries = load_libraries(cfg.libraries);
	require(!aot_runtime::enabled());
	if (!cfg.only.empty()) {
		/* One case from a failure report, under the diagnosis. */
		unsigned long long seed = 0, pc = 0;
		int faulting = 0, tbi = 0;
		require(std::sscanf(cfg.only.c_str(), "%llx:%llx:%d:%d", &seed,
			&pc, &faulting, &tbi) == 4);
		constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
		const auto image = load_library(
			libraries[cfg.library < 0 ? 0 : cfg.library].host);
		g_sites_image = &image;
		const auto c = make_case_from_seed(seed, bias, pc, faulting != 0,
			tbi != 0, arena_base(bias));
		(void)diagnose(c, image, bias);
		return 0;
	}
	bool scenarios_done = false;
	for (const auto &file : libraries) {
		if (cfg.library >= 0 &&
		    static_cast<long>(file.index) != cfg.library)
			continue;
		const auto image = load_library(file.host);
		g_sites_image = &image;
		const auto functions = load_functions(cfg.functions, file.index);
		std::fprintf(stderr, "library %zu %s: %zu functions\n", file.index,
			file.guest.c_str(), functions.size());
		/* A sample of a large library: evenly spaced functions. */
		std::vector<Function> sample;
		const auto stride = std::max<std::size_t>(
			1, functions.size() / cfg.sample);
		for (std::size_t i = 0; i < functions.size(); i += stride)
			sample.push_back(functions[i]);
		if (!scenarios_done) {
			default_off_and_single_step(image, sample);
			scenarios_done = true;
		}
		aot::reset_stats();
		const auto sites = load_sites(cfg.sites, file.index);
		const auto t = differential(cfg, image, sample, sites);
		const auto st = aot::stats();
		std::fprintf(stderr,
			"differential: %zu cases, %zu compared, %zu unbounded "
			"(%zu not repeatable, %zu too slow), %zu finished, %zu faulting, %zu faults; enters %llu steps %llu "
			"binds %llu verifications %llu retired %llu refills %llu "
			"refusals sp %llu fp %llu poisoned %llu unreadable %llu\n",
			t.cases, t.compared, t.skipped, t.nondeterministic, t.slow,
			t.finished, t.faulted,
			t.fault_events, static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps),
			static_cast<unsigned long long>(st.binds),
			static_cast<unsigned long long>(st.verifications),
			static_cast<unsigned long long>(st.retired),
			static_cast<unsigned long long>(st.refills),
			static_cast<unsigned long long>(st.refusals[1]),
			static_cast<unsigned long long>(st.refusals[3]),
			static_cast<unsigned long long>(st.poisoned),
			static_cast<unsigned long long>(st.unreadable));
		require(t.compared > t.cases / 2);
		require(st.enters > t.compared / 2);
		require(st.poisoned == 0);
		/* No entry ran longer than the budget allows, counted one
		 * instruction at a time. */
		require(st.max_retired <= entry_bound);
		/* Both entry refusals are exercised by every library's cases. */
		require(st.refusals[1] > 0);
		require(st.refusals[3] > 0);
		if (file.guest.ends_with("/libaotfixture.so")) {
			loop_through_another_range(image, functions);
			code_change_in_the_dispatch_window(image, functions);
		}
		/* The scenarios use the first library: they are about the
		 * mechanism, not about the code. */
		if (file.index == libraries.front().index) {
			if (file.guest.ends_with("/libz.so")) {
				inline_hit_path_is_taken(image, functions);
				return_loop_is_bounded(image);
				misaligned_sp_at_a_pair_store(image);
				open_exclusive_is_not_entered(image, functions);
				deferred_fp_state_is_judged_by_its_guest_fpcr(image,
					functions);
				equal_digest_is_not_equal_code(image, functions);
			}
			preemption_is_bounded(image, sample);
			two_load_biases(image, sample);
			other_bytes_fall_back(image, sample);
			changed_text_falls_back_for_good(image, functions);
			refill_keeps_state(image, sample);
			partially_mapped_library(image, functions);
			two_vcpus(image, sample);
		}
	}
	if (libraries.size() > 1 && cfg.library < 0) {
		const auto &first = libraries[0];
		const auto &second = libraries[1];
		const auto image = load_library(first.host);
		const auto other = load_library(second.host);
		cross_library_branches(image, load_functions(cfg.functions,
			first.index), other, load_functions(cfg.functions,
			second.index));
	}
	return 0;
}
