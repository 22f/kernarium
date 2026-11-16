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
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <arm_acle.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <pthread.h>
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
/* Whether the library under test is the fixture (tools/aot/fixture.py). */
bool g_fixture_library = false;

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

	/* The kinds and contents of another arena, as a run left them. */
	void adopt(const Arena &other)
	{
		std::array<std::byte, page_bytes> bytes;
		for (std::size_t i = 0; i < backings_.size(); ++i) {
			set_kind(i, other.kinds_[i]);
			other.backings_[i]->get(0, bytes);
			backings_[i]->put(0, bytes);
		}
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

/* What the engine reports of a fault: where, which access, whether it was an
 * alignment fault, and the floating-point syndrome (zero unless a trapped
 * floating-point exception ended the block). */
struct Fault final {
	std::uint64_t pc, address;
	std::uint32_t access;
	bool alignment;
	std::uint32_t syndrome;
	/* The state the fault leaves, which the resumed run then builds on: the
	 * integer registers, the stack pointer, the flags, all vector registers
	 * (as a hash) and the arena (as a hash). A partial copy or set, or a
	 * vector access that changed a register before its fault, must leave
	 * exactly the engine's: the resumed run would repair it. */
	std::array<std::uint64_t, 31> registers;
	std::uint64_t sp;
	std::uint32_t nzcv;
	std::uint64_t vectors;
	std::uint64_t memory;
	bool operator==(const Fault &) const = default;
};

[[nodiscard]] std::uint64_t hash_of(const void *data, std::size_t size)
{
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	const auto *bytes = static_cast<const unsigned char *>(data);
	for (std::size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
	return hash;
}

[[nodiscard]] std::uint64_t hash_bytes(const std::vector<std::byte> &bytes)
{
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8) {
		std::uint64_t word;
		std::memcpy(&word, &bytes[i], 8);
		hash = (hash ^ word) * 0x100000001b3ULL;
		hash ^= hash >> 29;
	}
	return hash;
}

struct Result final {
	arm64::CpuState cpu{};
	std::vector<Fault> faults;
	std::vector<std::byte> memory;
	EngineEvent last{EngineEvent::continued};
	/* The floating-point syndrome and alignment flag the engine reported
	 * for the event that ended the run. */
	std::uint32_t end_syndrome{};
	bool end_alignment{};
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

/* run_once, through the ahead-of-time step while it is switched on, as the
 * engine's entry would choose (GadgetExecutor::aot_selected). */
[[nodiscard]] Outcome run_selected(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = aot_runtime::enabled() ?
		executor.run_aot_block(cpu, memory, generation, executed) :
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

using Observer = std::function<void(const arm64::CpuState &)>;

/* The last blocks of a run, for the failure report. */
struct BlockLog final {
	bool enabled{};
	std::vector<std::array<std::uint64_t, 5>> blocks;
} g_log;

/* The blocks of a run from the state in `cpu`, until the guest makes a system
 * call, an exit it sees, a fault no page can repair or `blocks` of them have
 * run. A fault inside the arena gets its page (writable) and the run goes on
 * at the same address, unless `stop_at_fault`: the run then ends at the first
 * memory fault, with the state it leaves, to be resumed by another call. */
[[nodiscard]] Result run_from(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, Arena &arena,
	arm64::CpuState cpu, std::size_t blocks, const Observer &observer = {},
	bool stop_at_fault = false)
{
	Result r;
	std::size_t faults_taken = 0;
	for (; r.blocks < blocks; ++r.blocks) {
		const auto before = cpu.pc;
		const auto out = run_selected(
			executor, memory, generation, cpu);
		if (g_log.enabled)
			g_log.blocks.push_back({before,
				static_cast<std::uint64_t>(out.event), out.executed,
				cpu.pc, aot::stats().enters});
		r.max_block = std::max(r.max_block, out.executed);
		r.last = out.event;
		if (out.event == EngineEvent::continued ||
		    out.event == EngineEvent::interrupt) {
			if (observer)
				observer(cpu);
			continue;
		}
		arm64::restore_native_fp_environment(cpu);
		r.end_syndrome = executor.floating_point_syndrome();
		r.end_alignment = executor.fault_alignment();
		if (out.event == EngineEvent::system_call) {
			r.finished = true;
			break;
		}
		if (out.event != EngineEvent::memory_fault)
			break;
		const auto address = executor.fault_address();
		Fault fault{cpu.pc, address,
			static_cast<std::uint32_t>(executor.fault_access()),
			executor.fault_alignment(),
			executor.floating_point_syndrome(), {}, cpu.sp, cpu.nzcv,
			hash_of(cpu.vectors.data(), sizeof(cpu.vectors)),
			hash_bytes(arena.snapshot())};
		for (std::size_t i = 0; i < fault.registers.size(); ++i)
			fault.registers[i] = cpu.registers[i];
		r.faults.push_back(fault);
		if (stop_at_fault)
			break;
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
	arm64::restore_native_fp_environment(cpu);
	r.cpu = cpu;
	r.memory = arena.snapshot();
	return r;
}

[[nodiscard]] Result drive_on(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, std::uint64_t bias,
	Arena &arena, Case c, std::size_t blocks, const Observer &observer = {},
	bool stop_at_fault = false)
{
	c.cpu.pc = bias + c.pc_offset;
	for (std::size_t i = 0; i < arena.total(); ++i)
		if (i < c.kinds.size())
			arena.set_kind(i, c.kinds[i]);
	SplitMix fill{c.seed};
	arena.fill(fill);
	return run_from(executor, memory, generation, arena, c.cpu, blocks,
		observer, stop_at_fault);
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
	    a.end_syndrome != b.end_syndrome ||
	    a.end_alignment != b.end_alignment ||
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
	if (a.end_syndrome != b.end_syndrome ||
	    a.end_alignment != b.end_alignment) {
		say("syndrome");
		std::fprintf(stderr, "  syndrome ref %x aot %x alignment ref %d "
			"aot %d\n", a.end_syndrome, b.end_syndrome,
			a.end_alignment ? 1 : 0, b.end_alignment ? 1 : 0);
	}
	if (a.faults != b.faults) {
		say("faults");
		std::fprintf(stderr, "  faults ref %zu aot %zu\n", a.faults.size(),
			b.faults.size());
		for (std::size_t i = 0; i < std::min(a.faults.size(),
		     b.faults.size()); ++i) {
			const auto &x = a.faults[i];
			const auto &y = b.faults[i];
			if (x == y)
				continue;
			std::fprintf(stderr, "  fault %zu: pc %llx/%llx address %llx/%llx "
				"access %u/%u alignment %d/%d syndrome %x/%x nzcv %x/%x "
				"sp %llx/%llx vectors %s memory %s\n", i,
				static_cast<unsigned long long>(x.pc),
				static_cast<unsigned long long>(y.pc),
				static_cast<unsigned long long>(x.address),
				static_cast<unsigned long long>(y.address), x.access,
				y.access, x.alignment ? 1 : 0, y.alignment ? 1 : 0,
				x.syndrome, y.syndrome, x.nzcv, y.nzcv,
				static_cast<unsigned long long>(x.sp),
				static_cast<unsigned long long>(y.sp),
				x.vectors == y.vectors ? "same" : "DIFFERENT",
				x.memory == y.memory ? "same" : "DIFFERENT");
			for (std::size_t r = 0; r < x.registers.size(); ++r)
				if (x.registers[r] != y.registers[r])
					std::fprintf(stderr, "    x%zu ref %llx aot %llx\n", r,
						static_cast<unsigned long long>(x.registers[r]),
						static_cast<unsigned long long>(y.registers[r]));
			break;
		}
	}
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
	/* The fixture's page-edge functions return through x15 and one branches
	 * through x16 (tools/aot/fixture.py). A case may start after the
	 * instruction that sets x15, so give both a valid target: the random
	 * ones include addresses of all ones, which the engine's cached
	 * indirect-branch gadget takes for an empty cell. */
	if (g_fixture_library) {
		c.cpu.registers[15] = bias + stub_offset;
		c.cpu.registers[16] = bias + stub_offset;
	}
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
	/* A floating-point trap enable (IOE, DZE, OFE, UFE, IXE, IDE): the
	 * engine runs the code one instruction at a time and reports the
	 * syndrome of an exception that traps. */
	if (rng.below(16) == 0)
		c.cpu.fpcr |= static_cast<std::uint32_t>(
			(1 + rng.below(31)) << 8) | (rng.below(2) << 15);
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
		std::fprintf(stderr, "  fault pc %llx address %llx access %u "
			"alignment %d syndrome %x\n",
			static_cast<unsigned long long>(f.pc),
			static_cast<unsigned long long>(f.address), f.access,
			f.alignment ? 1 : 0, f.syndrome);
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
	/* Change one instruction of every code page: a NOP (a BRK where it is
	 * one already) in place of the first word of each function, and of
	 * the first word on every page a function reaches, which the engine
	 * will run. */
	const auto change = [&](std::uint64_t at) {
		std::uint32_t word;
		std::memcpy(&word, &other.bytes[at], 4);
		word = word == 0xd503201fU ? 0xd4200000U : 0xd503201fU;
		std::memcpy(&other.bytes[at], &word, 4);
	};
	for (const auto &f : functions) {
		change(f.start + 4);
		for (auto page = (f.start / page_bytes + 1) * page_bytes;
		     page < f.end; page += page_bytes)
			change(page);
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
 * page it changed is no longer the code the framework stands for, so the
 * engine runs it, and the pages that did not change are still entered. When
 * the bytes are back the page is compared again and entered. */
void changed_text_falls_back(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{19};
	const auto bias = Fixture::bias;
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	/* A function on another code page, whose first instruction is
	 * rewritten below. */
	const std::uint32_t patched = 0xd2800f00U; /* mov x0, #0x78 */
	std::uint64_t offset = 0;
	for (const auto &f : functions)
		if ((f.start >> 12) != (fn.start >> 12)) {
			offset = f.start;
			break;
		}
	require(offset != 0);
	std::uint32_t original;
	std::memcpy(&original, &image.bytes[offset], 4);
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[offset], &patched, 4);
	const auto d = make_case(rng, bias, offset, false, true, arena_base(bias));
	/* What the engine does with the bytes the guest holds before and after,
	 * on fresh guests (invalidate_code publishes in the generation the
	 * thread ran last, so the guest under test runs last before each). */
	Fixture ref_changed{4241, changed}, ref_original{4243, image};
	aot::set_enabled(false);
	const auto want_c = drive(ref_changed.env, ref_changed.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, d,
		max_blocks);
	const auto want_back = drive(ref_original.env, ref_original.arena, d,
		max_blocks);

	Fixture got{4242, image};
	aot::reset_stats();
	aot::set_enabled(true);
	const auto first = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	auto entered = aot::stats().enters;
	require(entered > 0);
	require(first.done());
	const auto publish = [&](std::uint32_t word) {
		got.env.backing.put32(offset, word);
		got.env.memory.invalidate_code(got.env.bias + offset);
	};
	publish(patched);
	/* fn's page did not change: still entered. */
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_c, have, c));
	auto st = aot::stats();
	require(st.poisoned == 0);
	require(st.enters > entered);
	entered = st.enters;
	/* The changed page itself: a function there is the engine's. */
	aot::set_enabled(true);
	const auto have_changed = drive(got.env, got.arena, d, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_changed, have_changed, d));
	st = aot::stats();
	require(st.enters == entered);
	require(st.poisoned == 0);
	/* The bytes are back and published: the page is compared again. */
	publish(original);
	aot::set_enabled(true);
	const auto have_back = drive(got.env, got.arena, d, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_back, have_back, d));
	st = aot::stats();
	require(st.enters > entered);
	require(st.poisoned == 0);
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

/* A library whose last code page is not mapped yet: the code on the pages
 * that are mapped is entered, a function on the missing page is the
 * engine's, and once the page is mapped its code is entered too. */
void partially_mapped_library(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	std::uint64_t code_end = 0;
	for (const auto &f : functions)
		code_end = std::max(code_end, f.end);
	const auto last_page = (code_end - 1) / page_bytes;
	require(last_page >= 1);
	/* Whole functions below the last page, and a pc on the last page in
	 * each function that reaches it. */
	std::vector<Function> below;
	std::vector<std::uint64_t> on_last;
	for (const auto &f : functions) {
		if (f.end <= last_page * page_bytes)
			below.push_back(f);
		else
			on_last.push_back(std::max(f.start, last_page * page_bytes));
	}
	require(below.size() > 8);
	require(!on_last.empty());
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
	require(st.enters > 0);
	require(st.poisoned == 0);
	/* The missing page is not entered: it cannot be read. */
	const auto entered = st.enters;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const auto c = make_case(rng, bias, on_last[0], false, true,
			arena_base(bias));
		aot::set_enabled(true);
		(void)drive(got, got_arena, c, 40);
		aot::set_enabled(false);
	}
	st = aot::stats();
	require(st.enters == entered);
	require(st.finds[2] > 0); /* not_ready */
	/* The loader maps the rest: the next time the page is due it is
	 * compared and the code on it is entered. */
	got.map_rest(image);
	std::size_t attempts = 0;
	while (aot::stats().enters == entered && attempts < 4000) {
		const auto c = make_case(rng, bias,
			on_last[attempts % on_last.size()], false, true,
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
	require(st.enters > entered);
	require(st.poisoned == 0);
	/* And the code on it behaves as the engine's. */
	for (std::size_t f = 0; f < on_last.size(); ++f) {
		const auto c = make_case(rng, bias, on_last[f], false, true,
			arena_base(bias));
		aot::set_enabled(false);
		const auto want = drive(ref, ref_arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
	}
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

/* The instructions between two polls are the instructions that ran. A
 * framework made with UML_AOT_COUNT_RETIRED counts each one it starts; the
 * product's layout reports the budget the entry spent, which the same bound
 * covers. */
constexpr std::uint64_t entry_bound = 4096 + 32 + 2; /* RUN_LIMIT, rewrite.py */

/* A state rewritten code may be entered with: SP aligned and none of the
 * FPCR controls the entry refuses. */
void enterable(Case &c)
{
	c.cpu.sp &= ~0xfULL;
	c.cpu.fpcr &= ~static_cast<std::uint32_t>(arm64::fpcr_afp_controls);
	/* Nor under a floating-point trap enable, which the engine runs one
	 * instruction at a time. */
	c.cpu.fpcr &= ~0x9f00U;
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
 * the same state without them is entered, whatever the stale value holds
 * (both directions: stale zero under current bits, stale bits under a
 * current zero), and either way the result is the engine's. */
void deferred_fp_state_is_judged_by_its_guest_fpcr(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto bias = Fixture::bias;
	struct Combination final {
		std::uint32_t stale, guest;
	};
	constexpr Combination combinations[] = {
		{0, 1}, {0, 2}, {0, 4}, {0, 7}, {1, 0}, {2, 0}, {4, 0}, {7, 0},
		{7, 7}, {0, 0}};
	unsigned long generation = 4500;
	for (const auto &combination : combinations) {
		Fixture ref{generation++, image}, got{generation++, image};
		SplitMix rng{67};
		auto c = make_case(rng, bias, functions[3].start, false, true,
			arena_base(bias));
		enterable(c);
		std::vector<std::byte> saved(sizeof(c.cpu.vectors));
		std::memcpy(saved.data(), c.cpu.vectors.data(), saved.size());
		const std::uint32_t guest_fpcr =
			(c.cpu.fpcr & ~0x7U) | combination.guest;
		c.cpu.defer_fp_load(saved.data(), guest_fpcr, 0, ~0U);
		/* The register file's own, stale value: not loaded yet. */
		c.cpu.fpcr = combination.stale;
		c.cpu.fpsr = 0;
		require(!c.cpu.fp_loaded);
		const bool refused = combination.guest != 0;
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
		std::fprintf(stderr, "deferred FP state, stale %u guest %u: %llu "
			"entries, %llu fp refusals\n", combination.stale,
			combination.guest, static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.refusals[3]));
		if (refused) {
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
	/* The fixture's `isb; mov x0, #1` form (tools/aot/fixture.py). */
	const Function *found = nullptr;
	for (const auto &f : functions) {
		std::uint32_t words[2];
		std::memcpy(words, &image.bytes[f.start], sizeof(words));
		if (words[0] == 0xd5033fdfU && words[1] == 0xd2800020U)
			found = &f;
	}
	require(found != nullptr);
	const auto form = *found;
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

/* The fixture's page edges (tools/aot/fixture.py: EDGES): functions on one
 * text page that leave it for the next page by each kind of edge. The
 * target there adds 100 to x0 and returns through x15; the function that
 * falls into the page adds 4 on it (1 and 2 on the page before). */
struct PageEdge final {
	const char *name;
	std::uint64_t at;
};
constexpr PageEdge page_edges[] = {
	{"fall", 0x3ff8}, {"b", 0x3000}, {"bl", 0x3020}, {"bcond", 0x3040},
	{"cbz", 0x3060}, {"tbz", 0x3080}, {"br", 0x30a0}, {"blr", 0x30c0},
	{"ret", 0x30e0}};
constexpr std::uint64_t edge_next_page = 0x4000, edge_target = 0x4100;

[[nodiscard]] LibraryImage with_next_page_changed(const LibraryImage &image)
{
	LibraryImage other = image;
	const std::uint32_t eight = 0x91002000U;       /* add x0, x0, #8 */
	const std::uint32_t two_hundred = 0x91032000U; /* add x0, x0, #200 */
	std::memcpy(&other.bytes[edge_next_page], &eight, 4);
	std::memcpy(&other.bytes[edge_target], &two_hundred, 4);
	return other;
}

[[nodiscard]] Case edge_case(const PageEdge &edge, std::uint64_t seed,
	std::uint64_t bias)
{
	SplitMix rng{seed};
	auto c = make_case(rng, bias, edge.at, false, true, arena_base(bias));
	enterable(c);
	c.cpu.registers[0] = 0;
	c.cpu.registers[1] = 5;
	c.cpu.registers[30] = bias + stub_offset;
	return c;
}

/* Every kind of edge from a page that matches the framework to a page whose
 * mapped bytes differ from it. The code of the page it left is entered, the
 * edge ends the entry, and the engine runs the other page's bytes; an edge
 * that went on in the rewritten code of that page would add the old
 * amount. */
void edges_into_a_page_that_differs(const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	const auto bias = Fixture::bias;
	unsigned long generation = 4400;
	for (const auto &edge : page_edges) {
		Fixture ref{generation++, other}, got{generation++, other};
		const auto c = edge_case(edge, 73, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] ==
			(std::string{edge.name} == "fall" ? 11U : 200U));
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!same(want, have, c))
			std::fprintf(stderr, "  edge %s\n", edge.name);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "edge %s into a changed page: %llu entries\n",
			edge.name, static_cast<unsigned long long>(st.enters));
		require(st.enters >= 1);
		require(st.poisoned == 0);
	}
}

/* The same edges into a page that matches: it is compared when the edge
 * first reaches it, and the code on it is entered. */
void edges_into_a_page_that_matches(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	unsigned long generation = 4420;
	for (const auto &edge : page_edges) {
		Fixture ref{generation++, image}, got{generation++, image};
		const auto c = edge_case(edge, 79, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] ==
			(std::string{edge.name} == "fall" ? 7U : 100U));
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		require(st.enters >= 2);
		require(st.verifications >= 2);
		require(st.poisoned == 0);
	}
}

/* A page that changes after the page it is entered from was compared: the
 * edge into it is the engine's from then on; when the bytes are back, it is
 * compared again. */
void next_page_changes_after_its_neighbour_was_compared(
	const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	const auto bias = Fixture::bias;
	for (const auto &edge : page_edges) {
		Fixture ref_old{4440, image}, ref_new{4441, other};
		Fixture got{4442, image};
		const auto c = edge_case(edge, 83, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto old_result = drive(ref_old.env, ref_old.arena, c,
			max_blocks);
		const auto new_result = drive(ref_new.env, ref_new.arena, c,
			max_blocks);
		require(old_result.finished && new_result.finished);
		aot::set_enabled(true);
		const auto first = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(old_result, first, c));
		auto entered = aot::stats().enters;
		require(entered >= 2);
		const auto change = [&](const LibraryImage &to) {
			for (const auto offset : {edge_next_page, edge_target}) {
				std::uint32_t word;
				std::memcpy(&word, &to.bytes[offset], 4);
				got.env.backing.put32(offset, word);
			}
			got.env.memory.invalidate_code(got.env.bias + edge_next_page);
		};
		change(other);
		aot::set_enabled(true);
		const auto second = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!same(new_result, second, c))
			std::fprintf(stderr, "  edge %s after the change\n", edge.name);
		require(same(new_result, second, c));
		/* The page the edge starts on is still entered. */
		auto st = aot::stats();
		require(st.enters > entered);
		require(st.poisoned == 0);
		entered = st.enters;
		change(image);
		aot::set_enabled(true);
		const auto third = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(old_result, third, c));
		st = aot::stats();
		require(st.enters >= entered + 2);
		require(st.poisoned == 0);
	}
}

/* An indirect branch into another instance of the library (a second load
 * address in the same address space) lands on a page that matches, or on
 * one that differs: either way the result is the engine's, and the code of
 * a page that differs is not entered. */
void edge_into_another_instance(const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	constexpr std::uint64_t bias_b = 0x7155'0000'0000ULL;
	const auto bias = Fixture::bias;
	for (const bool differs : {true, false}) {
		const unsigned long generation = differs ? 4460 : 4462;
		Environment ref{generation, bias, image};
		Environment got{generation + 1, bias, image};
		ref.add_library(bias_b, differs ? other : image);
		got.add_library(bias_b, differs ? other : image);
		Arena ref_arena{ref.generation, arena_base(bias)};
		Arena got_arena{got.generation, arena_base(bias)};
		/* The second instance is bound by running a function on its
		 * first code page. */
		const PageEdge warm{"b", 0x3000};
		auto w = edge_case(warm, 89, bias_b);
		aot::reset_stats();
		aot::set_enabled(true);
		(void)drive_on(got.executor, got.memory, got.generation, bias_b,
			got_arena, w, aot_max_blocks);
		aot::set_enabled(false);
		const auto warmed = aot::stats().enters;
		require(warmed >= 1);
		const PageEdge via_register{"reg", 0x3100};
		auto c = edge_case(via_register, 97, bias);
		c.cpu.registers[16] = bias_b + edge_target;
		aot::set_enabled(false);
		const auto want = drive_on(ref.executor, ref.memory, ref.generation,
			bias, ref_arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] == (differs ? 200U : 100U));
		aot::set_enabled(true);
		const auto have = drive_on(got.executor, got.memory, got.generation,
			bias, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "edge into another instance (page %s): %llu "
			"entries, %llu cross misses\n", differs ? "differs" : "matches",
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.cross_misses));
		require(st.cross_misses >= 1);
		require(st.poisoned == 0);
	}
}

/* ---- scenarios on the fixture's shared-memory loops and CAS forms ------- */

/* tools/aot/fixture.py: the loops that add one to the cell at [x2] x0 times,
 * and the first of the instruction forms (cas_<size>_<ordering> is form
 * 177 + 2 * (4 * size + ordering); the tooling test pins it). */
constexpr std::uint64_t hammer_add = 0x2a00, hammer_cas = 0x2a40,
	hammer_llsc = 0x2a80;
constexpr std::uint64_t forms_at = 0x1100, form_size = 0x14;

[[nodiscard]] constexpr std::uint64_t cas_form(unsigned size, unsigned order)
{
	return forms_at + (177 + 2 * (4 * size + order)) * form_size;
}

/* The library mapped at `bias` with a stub of its own, in an address space
 * `generation` that other copies and an arena share. */
struct Copy final {
	Copy(unsigned long generation, std::uint64_t bias,
		const LibraryImage &image)
		: text{image.pages}, stub{1},
		  text_map{generation, bias, text, image.pages,
			PROT_READ | PROT_EXEC, true},
		  stub_map{generation, bias + stub_offset, stub, 1,
			PROT_READ | PROT_EXEC, true}
	{
		text.put(0, image.bytes);
		stub.put32(0, system_call);
	}
	JitBacking text, stub;
	GenerationMapping text_map, stub_map;
};

/* Run `c` on `executor` through the AOT dispatcher until the guest returns to
 * its stub, whatever interrupts or other blocks come between. Memory is
 * whatever the other vCPUs and the arena hold: nothing is reset. */
std::size_t run_until_stub(GadgetExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, arm64::CpuState &cpu,
	std::size_t *interrupts = nullptr)
{
	std::size_t blocks = 0;
	for (;; ++blocks) {
		require(blocks < 20'000'000);
		require(executor.begin_execution(generation, cpu.pc));
		std::size_t executed = 0;
		const auto event =
			executor.run_aot_block(cpu, memory, generation, executed);
		executor.end_execution();
		if (event == EngineEvent::system_call)
			break;
		require(event == EngineEvent::continued ||
			event == EngineEvent::interrupt);
		if (event == EngineEvent::interrupt && interrupts != nullptr)
			++*interrupts;
	}
	arm64::restore_native_fp_environment(cpu);
	return blocks;
}

/* A state at a loop of the fixture that rewritten code is entered under. */
[[nodiscard]] Case loop_case(std::uint64_t seed, std::uint64_t code_bias,
	std::uint64_t offset, std::uint64_t arena_at)
{
	SplitMix rng{seed};
	auto c = make_case(rng, code_bias, offset, false, true, arena_at);
	enterable(c);
	c.cpu.pc = code_bias + offset;
	c.cpu.registers[30] = code_bias + stub_offset;
	return c;
}

[[nodiscard]] std::uint64_t cell_value(const Arena &arena, std::size_t at)
{
	const auto bytes = arena.snapshot();
	std::uint64_t value;
	std::memcpy(&value, &bytes[at], 8);
	return value;
}

/* Rewritten code and the engine add to one cell from two vCPUs. The second
 * vCPU runs a copy of the library whose bytes differ from the framework's
 * (so the engine runs every instruction of it), or the same copy as the
 * first. A lost update would leave the count short. */
void atomics_are_shared_between_aot_and_engine(const LibraryImage &image)
{
	constexpr std::uint64_t bias_a = 0x7173'0000'0000ULL;
	constexpr std::uint64_t bias_b = 0x7174'0000'0000ULL;
	constexpr std::uint64_t iterations = 30000;
	constexpr unsigned long generation = 4510;
	auto changed = image;
	const std::uint32_t nop = 0xd503201fU;
	std::memcpy(&changed.bytes[0x2ff0], &nop, 4); /* filler on the loops' page */
	struct Pair {
		bool second_changed;
		std::uint64_t first, second;
		const char *name;
	};
	const Pair pairs[] = {
		{true, hammer_add, hammer_add, "ldadd / ldadd"},
		{true, hammer_add, hammer_cas, "ldadd / cas"},
		{true, hammer_cas, hammer_add, "cas / ldadd"},
		{true, hammer_cas, hammer_llsc, "cas / ldxr+stxr"},
		{true, hammer_llsc, hammer_add, "ldxr+stxr / ldadd"},
		{true, hammer_llsc, hammer_llsc, "ldxr+stxr / ldxr+stxr"},
		{false, hammer_add, hammer_add, "ldadd / ldadd (both rewritten)"},
		{false, hammer_add, hammer_cas, "ldadd / cas (both rewritten)"},
		{false, hammer_cas, hammer_cas, "cas / cas (both rewritten)"},
		{false, hammer_llsc, hammer_add, "ldxr+stxr / ldadd (both rewritten)"},
	};
	Copy first_copy{generation, bias_a, image};
	Copy second_copy{generation, bias_b, changed};
	Arena arena{generation, arena_base(bias_a)};
	StaticGuestMemory memory_a, memory_b;
	GadgetExecutor executor_a, executor_b;
	const auto cell = arena_base(bias_a) + 0x100;
	for (std::size_t i = 0; i < arena.total(); ++i)
		arena.set_kind(i, 2);
	aot::set_enabled(true);
	/* The changed copy is the engine's alone. */
	{
		aot::reset_stats();
		SplitMix fill{3};
		arena.fill(fill);
		auto c = loop_case(7, bias_b, hammer_add, arena_base(bias_a));
		c.cpu.registers[0] = 100;
		c.cpu.registers[2] = cell;
		run_until_stub(executor_b, memory_b, generation, c.cpu);
		require(aot::stats().enters == 0);
		auto d = loop_case(7, bias_a, hammer_add, arena_base(bias_a));
		d.cpu.registers[0] = 100;
		d.cpu.registers[2] = cell;
		run_until_stub(executor_a, memory_a, generation, d.cpu);
		require(aot::stats().enters > 0);
	}
	for (const auto &pair : pairs) {
		SplitMix fill{11};
		arena.fill(fill);
		const auto initial = cell_value(arena, 0x100);
		aot::reset_stats();
		auto a = loop_case(21, bias_a, pair.first, arena_base(bias_a));
		auto b = loop_case(22, pair.second_changed ? bias_b : bias_a,
			pair.second, arena_base(bias_a));
		a.cpu.registers[0] = b.cpu.registers[0] = iterations;
		a.cpu.registers[2] = b.cpu.registers[2] = cell;
		std::atomic<bool> go{false};
		std::thread other{[&] {
			while (!go.load(std::memory_order_acquire))
				std::this_thread::yield();
			run_until_stub(executor_b, memory_b, generation, b.cpu);
		}};
		go.store(true, std::memory_order_release);
		run_until_stub(executor_a, memory_a, generation, a.cpu);
		other.join();
		const auto total = cell_value(arena, 0x100);
		const auto st = aot::stats();
		std::fprintf(stderr, "shared cell, %s: %llu of %llu, enters %llu "
			"steps %llu\n", pair.name,
			static_cast<unsigned long long>(total - initial),
			static_cast<unsigned long long>(2 * iterations),
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps));
		require(total - initial == 2 * iterations);
		require(a.cpu.registers[0] == 0 && b.cpu.registers[0] == 0);
		require(st.enters > 0);
		require(st.poisoned <= 1);
		/* The exclusive pair is the engine's even in rewritten code. */
		if (pair.first == hammer_llsc ||
		    (!pair.second_changed && pair.second == hammer_llsc))
			require(st.steps > 0);
	}
	aot::set_enabled(false);
}

/* CAS with the fixture's forms at every width and ordering: the compare
 * value matching and not, on a writable page, a read-only one, an unmapped
 * one and a misaligned address. Rewritten code and the engine end in the
 * same registers, memory and fault; the engine's own results are checked
 * against the architecture's too. */
void cas_outcomes_and_permissions(const LibraryImage &image)
{
	constexpr unsigned long generation_ref = 4530, generation_aot = 4531;
	const auto bias = Fixture::bias;
	Fixture ref{generation_ref, image}, got{generation_aot, image};
	const std::uint64_t masks[4] = {0xff, 0xffff, 0xffff'ffffULL, ~0ULL};
	std::size_t compared = 0, faulted = 0;
	enum Where { writable, read_only, unmapped, misaligned, count };
	aot::reset_stats();
	for (unsigned size = 0; size < 4; ++size)
		for (unsigned order = 0; order < 4; ++order)
			for (int where = 0; where < count; ++where)
				for (const bool match : {true, false}) {
					if (where == misaligned && size == 0)
						continue;
					SplitMix rng{1000 + 64 * size + 16 * order +
						4 * where + (match ? 1 : 0)};
					auto c = loop_case(rng.next(), bias,
						cas_form(size, order), arena_base(bias));
					c.kinds.assign(arena_pages + stack_pages, 2);
					c.kinds[0] = where == read_only ? 1 :
						where == unmapped ? 0 : 2;
					const auto cell = arena_base(bias) +
						(where == misaligned ? 0x103 : 0x100);
					/* What the arena holds at the cell. */
					SplitMix seeded{c.seed};
					ref.arena.fill(seeded);
					std::uint64_t old;
					const auto bytes = ref.arena.snapshot();
					std::memcpy(&old, &bytes[cell - arena_base(bias)], 8);
					old &= masks[size];
					c.cpu.registers[1] = cell;
					c.cpu.registers[2] = 0x1122'3344'5566'7788ULL;
					c.cpu.registers[3] = match ? old : (old ^ 1);
					aot::set_enabled(false);
					const auto want = drive(ref.env, ref.arena, c,
						max_blocks);
					require(want.done());
					aot::set_enabled(true);
					const auto have = drive(got.env, got.arena, c,
						aot_max_blocks);
					aot::set_enabled(false);
					require(same(want, have, c));
					++compared;
					/* The architecture's answer, from the engine's run. */
					const auto fresh = [&] {
						std::uint64_t value;
						std::memcpy(&value,
							&want.memory[cell - arena_base(bias)], 8);
						return value & masks[size];
					}();
					if (where == writable) {
						require(want.faults.empty());
						require(want.cpu.registers[3] == old);
						require(fresh == (match ?
							(c.cpu.registers[2] & masks[size]) : old));
					} else if (where == read_only || where == unmapped) {
						/* A write fault, whether or not the compare
						 * would have matched. */
						require(want.faults.size() >= 1);
						require(want.faults.front().address == cell);
						++faulted;
					}
				}
	const auto st = aot::stats();
	std::fprintf(stderr, "cas matrix: %zu cases compared, %zu faulting; "
		"enters %llu\n", compared, faulted,
		static_cast<unsigned long long>(st.enters));
	require(compared == 4 * 4 * 2 * count - 4 * 2);
	require(faulted == 4 * 4 * 2 * 2);
	require(st.enters > compared / 2);
	require(st.poisoned == 0);
}

/* A naturally misaligned CAS inside one 16-byte granule: the engine faults
 * it under the baseline profile (no FEAT_LSE2) and performs it under the
 * profile the runtime probes. Rewritten code leaves such an
 * access to the engine, so it ends as the engine does under either. */
void misaligned_cas_follows_the_profile(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto probed = ::uml::platform::apple::probed_host_capabilities();
	for (const bool lse2 : {false, true}) {
		if (lse2 && !probed.has(::uml::cpu::ArmFeature::lse2))
			continue;
		const ::uml::cpu::HostCapabilitiesScope scope{
			lse2 ? probed : ::uml::cpu::baseline_host_capabilities()};
		Fixture ref{4570 + (lse2 ? 2UL : 0UL), image};
		Fixture got{4571 + (lse2 ? 2UL : 0UL), image};
		SplitMix rng{91};
		auto c = loop_case(rng.next(), bias, cas_form(3, 0),
			arena_base(bias));
		c.cpu.registers[1] = arena_base(bias) + 0x103;
		c.cpu.registers[2] = 0x1122'3344'5566'7788ULL;
		c.cpu.registers[3] = 0;
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(want.done());
		require(same(want, have, c));
		std::fprintf(stderr, "misaligned cas, lse2 %d: %zu faults, "
			"alignment %d\n", lse2 ? 1 : 0, want.faults.size(),
			want.faults.empty() ? 0 : want.faults.front().alignment ? 1 : 0);
		if (lse2) {
			require(want.faults.empty());
		} else {
			require(!want.faults.empty() && want.faults.front().alignment);
		}
	}
}

/* More instances than the binder holds: the library mapped at 70 biases is
 * run copy by copy, twice. The ones it dropped are bound again, and every
 * run ends as the engine's does. */
void more_instances_than_the_binder_holds(const LibraryImage &image)
{
	constexpr std::size_t copies = 70;
	constexpr unsigned long generation = 4540;
	const auto first_bias = 0x7300'0000'0000ULL;
	const auto bias_of = [&](std::size_t index) {
		return first_bias + index * 0x1'0000'0000ULL;
	};
	std::vector<std::unique_ptr<Copy>> maps;
	for (std::size_t i = 0; i < copies; ++i)
		maps.push_back(std::make_unique<Copy>(generation, bias_of(i), image));
	Arena arena{generation, arena_base(bias_of(0))};
	StaticGuestMemory memory;
	GadgetExecutor executor;
	std::vector<Case> cases;
	for (std::size_t i = 0; i < copies; ++i) {
		/* The first loop of the fixture, counting to x3. */
		auto c = loop_case(500 + i, bias_of(i), 0x1000, arena_base(bias_of(0)));
		c.cpu.registers[3] = 50 + i;
		cases.push_back(c);
	}
	std::vector<Result> want;
	aot::set_enabled(false);
	for (std::size_t i = 0; i < copies; ++i)
		want.push_back(drive_on(executor, memory, generation, bias_of(i),
			arena, cases[i], max_blocks));
	aot::reset_stats();
	aot::set_enabled(true);
	for (int round = 0; round < 2; ++round)
		for (std::size_t i = 0; i < copies; ++i) {
			const auto have = drive_on(executor, memory, generation,
				bias_of(i), arena, cases[i], aot_max_blocks);
			require(want[i].done());
			require(same(want[i], have, cases[i]));
		}
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "%zu instances: binds %llu enters %llu\n", copies,
		static_cast<unsigned long long>(st.binds),
		static_cast<unsigned long long>(st.enters));
	/* Each copy is bound the first time, and the ones the 64 slots dropped
	 * again in the second round. */
	require(st.binds >= copies + (copies - 64));
	require(st.enters >= 2 * copies);
	require(st.poisoned == 0);
}

/* A shootdown (a mapping change by another thread) arrives while rewritten
 * code runs a long loop: the vCPU leaves at its next return to the
 * dispatcher, the writer is never kept waiting, and the loop ends in the
 * state the engine reaches. */
void a_shootdown_reaches_a_native_loop(const LibraryImage &image)
{
	constexpr unsigned long generation = 4560;
	constexpr std::uint64_t iterations = 20'000'000;
	const auto bias = Fixture::bias;
	Fixture got{generation, image};
	JitBacking spare{1};
	std::atomic<bool> done{false};
	std::atomic<std::size_t> shootdowns{0};
	/* A loop of three instructions per count: x0 counts up to x3. */
	auto c = loop_case(77, bias, 0x1000, arena_base(bias));
	c.cpu.registers[3] = iterations;
	for (std::size_t i = 0; i < got.arena.total(); ++i)
		got.arena.set_kind(i, 2);
	aot::reset_stats();
	aot::set_enabled(true);
	std::thread writer{[&] {
		/* Mappings elsewhere in the address space, made and removed, each
		 * of which closes the execution gate and interrupts the vCPU. */
		while (!done.load(std::memory_order_acquire) &&
		       shootdowns.load() < 20000) {
			GenerationMapping map{generation,
				got.env.bias + 0x3000'0000ULL, spare, 1,
				PROT_READ | PROT_WRITE, false};
			shootdowns.fetch_add(1);
			std::this_thread::yield();
		}
	}};
	std::size_t interrupts = 0;
	const auto began = std::chrono::steady_clock::now();
	std::size_t blocks = run_until_stub(got.env.executor, got.env.memory,
		generation, c.cpu, &interrupts);
	done.store(true, std::memory_order_release);
	writer.join();
	aot::set_enabled(false);
	const auto seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - began).count();
	const auto st = aot::stats();
	std::fprintf(stderr, "shootdowns during a native loop: %zu mapping "
		"changes, %zu interrupts, %zu blocks, %.2f s, enters %llu\n",
		shootdowns.load(), interrupts, blocks, seconds,
		static_cast<unsigned long long>(st.enters));
	require(c.cpu.registers[0] == iterations);
	require(st.enters > 0);
	require(shootdowns.load() > 10);
	require(interrupts > 0);
	require(seconds < 120);
	require(st.poisoned == 0);
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

/* Set the word at `fixup` of `work` (one page) so that the page's CRC, which
 * is linear in the page, is `wanted`: the word's effect on the CRC is a
 * bijection of 32-bit values. */
void cancel_crc_with_word(std::array<std::byte, page_bytes> &work,
	std::uint32_t wanted, std::size_t fixup)
{
	std::array<std::uint32_t, 32> image_of_bit{};
	const auto base_crc = crc_of(work.data());
	for (unsigned bit = 0; bit < 32; ++bit) {
		std::array<std::byte, page_bytes> flipped = work;
		std::uint32_t value;
		std::memcpy(&value, &flipped[fixup], 4);
		value ^= 1U << bit;
		std::memcpy(&flipped[fixup], &value, 4);
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
	std::memcpy(&value, &work[fixup], 4);
	value ^= chosen;
	std::memcpy(&work[fixup], &value, 4);
}

/* `image` with the first word of the code at `start` changed (to
 * `mov x0, #120`) and the last word of its page chosen so that the page's
 * digest is the one the framework was built from: a page that is not the
 * stored one and that no digest tells from it. */
[[nodiscard]] LibraryImage with_equal_digest(const LibraryImage &image,
	std::uint64_t start)
{
	LibraryImage other = image;
	const auto page = start / page_bytes * page_bytes;
	const auto fixup = page + page_bytes - 4;
	require(start + 4 < fixup);
	std::array<std::byte, page_bytes> original;
	std::memcpy(original.data(), &image.bytes[page], page_bytes);
	/* mov x0, #120 in place of the function's first word. */
	const std::uint32_t mov = 0xd2800f00U;
	std::memcpy(&other.bytes[start], &mov, 4);
	std::array<std::byte, page_bytes> work;
	std::memcpy(work.data(), &other.bytes[page], page_bytes);
	cancel_crc_with_word(work, crc_of(original.data()), page_bytes - 4);
	std::memcpy(&other.bytes[fixup], &work[page_bytes - 4], 4);
	require(digest_of(work.data()) == digest_of(original.data()));
	require(std::memcmp(work.data(), original.data(), page_bytes) != 0);

	return other;
}

/* `image` with bit `bit` of the byte at `byte` of the page at `page` flipped
 * and a second word of the page, 32 bytes from the first within its 64,
 * chosen so that the page's digest is the one the framework was built from:
 * a page that differs from the stored one by that bit and the few of the
 * word that cancels it, which no digest tells from it. Where the bit is
 * neither in the first nor in the last 48 bytes, the page keeps its first and
 * last 16 bytes. */
[[nodiscard]] LibraryImage with_a_bit_changed_and_equal_digest(
	const LibraryImage &image, std::uint64_t page, std::uint64_t byte,
	unsigned bit)
{
	LibraryImage other = image;
	std::array<std::byte, page_bytes> original;
	std::memcpy(original.data(), &image.bytes[page], page_bytes);
	std::array<std::byte, page_bytes> work = original;
	work[byte] = static_cast<std::byte>(
		std::to_integer<unsigned>(work[byte]) ^ (1U << bit));
	const std::size_t fixup = (byte & ~std::uint64_t{3}) ^ 0x20U;
	require(fixup + 4 <= page_bytes);
	cancel_crc_with_word(work, crc_of(original.data()), fixup);
	require(digest_of(work.data()) == digest_of(original.data()));
	require(std::memcmp(work.data(), original.data(), page_bytes) != 0);
	std::memcpy(&other.bytes[page], work.data(), page_bytes);
	return other;
}

void equal_digest_is_not_equal_code(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto start = functions[2].start;
	const auto other = with_equal_digest(image, start);
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

/* One changed bit anywhere in a compared page, however far from its first
 * and last bytes, is found by the hash of the page. The page's 64-bit digest
 * is made to come out the one the framework was built from (a word 32 bytes
 * from the bit cancels the bit's effect on it), so the digest finds the
 * library and only the stored hash can tell the page from the stored one; the
 * change is published and the history then drops what it held, so the binder
 * hashes the page again. The instance is the engine's from then on and the
 * guest's result is the engine's.
 *
 * The positions: the first and last byte, the byte before the middle, the
 * middle, the bytes either side of every 512-byte boundary, both bits at the
 * ends of a byte, and a seeded spread. */
void a_changed_bit_anywhere_in_a_page_is_refused(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto fn = functions[5];
	const auto page = fn.start / page_bytes * page_bytes;
	require(page + page_bytes <= image.bytes.size());
	std::vector<std::pair<std::uint64_t, unsigned>> positions{
		{0, 0}, {0, 7}, {page_bytes - 1, 0}, {page_bytes - 1, 7},
		{page_bytes / 2 - 1, 3}, {page_bytes / 2, 3}, {page_bytes / 2, 0}};
	for (std::uint64_t edge = 512; edge < page_bytes; edge += 512) {
		positions.emplace_back(edge - 1, 7);
		positions.emplace_back(edge, 0);
	}
	SplitMix pick{73};
	for (int i = 0; i < 24; ++i)
		positions.emplace_back(pick.below(page_bytes),
			static_cast<unsigned>(pick.below(8)));
	const auto bias = Fixture::bias;
	SplitMix rng{31};
	const auto c = make_case(rng, bias, fn.start, false, true,
		arena_base(bias));
	std::size_t refused = 0;
	for (const auto &[byte, bit] : positions) {
		const auto changed =
			with_a_bit_changed_and_equal_digest(image, page, byte, bit);
		/* Where the bit is in the middle, the first and the last 16 bytes
		 * of the page are the stored ones. */
		if (byte >= 48 && byte < page_bytes - 48) {
			require(std::memcmp(&image.bytes[page], &changed.bytes[page],
				16) == 0);
			require(std::memcmp(&image.bytes[page + page_bytes - 16],
				&changed.bytes[page + page_bytes - 16], 16) == 0);
		}
		Fixture ref{4601, changed}, got{4602, image};
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::reset_stats();
		aot::set_enabled(true);
		const auto first = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(first.done());
		const auto entered = aot::stats().enters;
		require(entered > 0);
		require(aot::stats().poisoned == 0);
		/* The page becomes the other one, and its publication is
		 * followed by more than the history holds. */
		for (std::uint64_t word = page; word < page + page_bytes; word += 4) {
			std::uint32_t was, now;
			std::memcpy(&was, &image.bytes[word], 4);
			std::memcpy(&now, &changed.bytes[word], 4);
			if (was != now)
				got.env.backing.put32(word, now);
		}
		got.env.memory.invalidate_code(got.env.bias + page);
		for (std::size_t i = 0; i < 40; ++i)
			got.env.memory.invalidate_code(
				bias + 0x2000'0000ULL + (400 + i) * 0x10000);
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (want.done())
			require(same(want, have, c));
		const auto st = aot::stats();
		require(st.poisoned == 1);
		require(st.enters == entered);
		++refused;
	}
	std::fprintf(stderr, "changed bit refused: %zu positions\n", refused);
	require(refused == positions.size());
}

/* A text page changes after it was compared, and more code changes are
 * published than the history keeps: the binder cannot tell which pages the
 * dropped ones named, so it compares them again. The change is one that a
 * page digest does not show (the page's digest is the framework's), so the
 * exact comparison is all that stands between the stored code and the
 * guest's. */
void code_change_history_overflows(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{29};
	const auto bias = Fixture::bias;
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	std::uint64_t offset = 0;
	for (const auto &f : functions)
		if ((f.start >> 12) != (fn.start >> 12) &&
		    f.start % page_bytes < page_bytes - 64) {
			offset = f.start;
			break;
		}
	require(offset != 0);
	const auto collided = with_equal_digest(image, offset);
	const auto page = offset / page_bytes * page_bytes;
	const auto fixup = page + page_bytes - 4;
	std::uint32_t patched, fixed, original, original_fixup;
	std::memcpy(&patched, &collided.bytes[offset], 4);
	std::memcpy(&fixed, &collided.bytes[fixup], 4);
	std::memcpy(&original, &image.bytes[offset], 4);
	std::memcpy(&original_fixup, &image.bytes[fixup], 4);
	const auto d = make_case(rng, bias, offset, false, true, arena_base(bias));
	Fixture ref_changed{4551, collided}, ref_original{4553, image};
	aot::set_enabled(false);
	const auto want_c = drive(ref_original.env, ref_original.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, d,
		max_blocks);
	const auto want_original = drive(ref_original.env, ref_original.arena, d,
		max_blocks);
	require(want_changed.cpu.registers[0] != want_original.cpu.registers[0]);

	Fixture got{4552, image};
	const auto publish_elsewhere = [&](std::size_t count, std::size_t from) {
		for (std::size_t i = 0; i < count; ++i)
			got.env.memory.invalidate_code(
				bias + 0x2000'0000ULL + (from + i) * 0x10000);
	};
	const auto run = [&](const Case &which) {
		aot::set_enabled(true);
		auto result = drive(got.env, got.arena, which, aot_max_blocks);
		aot::set_enabled(false);
		return result;
	};
	aot::reset_stats();
	require(same(want_c, run(c), c));
	require(same(want_original, run(d), d));
	auto st = aot::stats();
	require(st.enters > 0);
	/* A few unrelated publications: what was compared stays compared. */
	publish_elsewhere(3, 0);
	const auto hashed = st.pages_hashed;
	require(same(want_c, run(c), c));
	require(same(want_original, run(d), d));
	st = aot::stats();
	require(st.pages_hashed == hashed);
	/* The page of d becomes the digest-equal other one, and its
	 * publication is followed by more than the history holds. */
	got.env.backing.put32(offset, patched);
	got.env.backing.put32(fixup, fixed);
	got.env.memory.invalidate_code(got.env.bias + offset);
	publish_elsewhere(40, 100);
	const auto before = aot::stats().enters;
	require(same(want_changed, run(d), d));
	st = aot::stats();
	require(st.enters == before);
	/* The exact comparison found the page different: the instance is
	 * the engine's from then on, whatever the guest does next. */
	require(st.poisoned == 1);
	require(st.pages_hashed > hashed);
	require(same(want_c, run(c), c));
	got.env.backing.put32(offset, original);
	got.env.backing.put32(fixup, original_fixup);
	got.env.memory.invalidate_code(got.env.bias + offset);
	publish_elsewhere(40, 200);
	require(same(want_original, run(d), d));
	st = aot::stats();
	require(st.enters == before);
}


/* ---- memory copies and sets ---------------------------------------------- */

/* tools/aot/fixture.py (MOPS_AT, MOPS_ROWS): the three stages of CPYF, CPY and
 * SET from registers 0, 2, 1 (destination, count, source or fill), the same
 * from 17, 16, 15, and a SET whose fill is the zero register. */
constexpr std::uint64_t mops_at = 0x2b00, mops_step = 0x20;
enum MopsRow : std::size_t {
	row_cpyf, row_cpy, row_set, row_cpyf_high, row_cpy_high, row_set_high,
	row_set_zero, mops_rows,
};

[[nodiscard]] bool mops_is_set(std::size_t row)
{
	return row == row_set || row == row_set_high || row == row_set_zero;
}

/* tools/aot/fixture.py (SIMD_WARM_AT): byte load from x2, then the structure
 * access from x1. */
constexpr std::uint64_t simd_warm_at = 0x2f00, simd_warm_step = 0x10;
constexpr std::size_t simd_warm_rows = 4;

struct MopsRun final {
	std::size_t compared{}, faulted{};
};

/* Where in the row a start is made (0 the prologue, 8 the epilogue) and, if
 * set, the flags it starts with. */
struct MopsStart final {
	std::uint64_t at{0};
	std::optional<std::uint32_t> nzcv{};
};

/* One start of a row: destination, source and count, and the kind of each
 * page of the arena (0 unmapped, 1 read-only, 2 writable). The engine runs
 * it alone and then with rewritten code; the end state and the state at every
 * fault (registers, flags, memory) must be the same. */
void mops_row(Fixture &ref, Fixture &got, SplitMix &rng, MopsRun &run,
	std::size_t row, std::uint64_t destination, std::uint64_t source,
	std::uint64_t count, const std::vector<std::uint8_t> &kinds,
	std::uint64_t fill = 0x1234'5678'90ab'cdefULL, MopsStart start = {})
{
	const auto bias = Fixture::bias;
	auto c = make_case_from_seed(rng.next(), bias,
		mops_at + row * mops_step + start.at, true, true, arena_base(bias));
	enterable(c);
	if (start.nzcv)
		c.cpu.nzcv = *start.nzcv;
	c.kinds = kinds;
	const bool high = row == row_cpyf_high || row == row_cpy_high ||
		row == row_set_high;
	const unsigned d = high ? 17 : 0, n = high ? 16 : 2, s = high ? 15 : 1;
	c.cpu.registers[d] = destination;
	c.cpu.registers[n] = count;
	if (row != row_set_zero)
		c.cpu.registers[s] = mops_is_set(row) ? fill : source;
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	if (!want.done() || !same(want, have, c)) {
		std::fprintf(stderr, "  memory copy/set row %zu at +%llu flags %x: "
			"destination %llx source %llx count %llx\n", row,
			static_cast<unsigned long long>(start.at),
			start.nzcv ? *start.nzcv : 0xffU,
			static_cast<unsigned long long>(destination),
			static_cast<unsigned long long>(source),
			static_cast<unsigned long long>(count));
		require(false);
	}
	++run.compared;
	run.faulted += want.faults.empty() ? 0 : 1;
}

/* The engine alone and then rewritten code, both from this case. */
void compare_case(Fixture &ref, Fixture &got, const Case &c, const char *what)
{
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	if (!want.done() || !same(want, have, c)) {
		std::fprintf(stderr, "  %s\n", what);
		require(false);
	}
}

void memory_copy_and_set_match_the_engine(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4601, image}, got{4602, image};
	SplitMix rng{31};
	MopsRun run;
	aot::reset_stats();
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	const std::vector<std::uint8_t> plain(arena_pages + stack_pages, 2);
	const auto with = [&](std::initializer_list<
		std::pair<std::size_t, std::uint8_t>> changes) {
		auto kinds = plain;
		for (const auto &[page, kind] : changes)
			kinds[page] = kind;
		return kinds;
	};
	const std::uint64_t counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17,
		31, 32, 33, 63, 64, 65, 100, 128, 255, 1000, 4095, 4096, 4097,
		5000, 9000, 1ULL << 20};
	const std::uint64_t tags[] = {0, 0x5aULL << 56, 0xa3ULL << 56};
	for (std::size_t row = 0; row < mops_rows; ++row) {
		const bool set = mops_is_set(row);
		const bool high = row == row_cpyf_high || row == row_cpy_high ||
			row == row_set_high;
		for (const auto n : counts) {
			if (high && n > 129 && n != 4097)
				continue;
			for (const auto tag : tags) {
				const auto tagged = [&](std::uint64_t address) {
					return address | tag;
				};
				/* Inside one page each, at several alignments. */
				for (const auto &[so, dof] : std::vector<
				     std::pair<std::uint64_t, std::uint64_t>>{
				     {0x100, 0x300}, {0x103, 0x7fd}, {0, 0},
				     {0x4f, 0x60}}) {
					if (n > 4096 && so != 0x100)
						continue;
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) + dof), tagged(page_at(2) + so),
						n, plain);
				}
				if (n == 0)
					continue;
				/* The destination, the source or both end in a page that
				 * is unmapped or read-only: the fault comes at the first,
				 * a middle and the last byte. */
				std::vector<std::uint64_t> leads{0, n / 2, n - 1};
				if (n == 1)
					leads = {0};
				if (n > 4096)
					leads = {0, 1, 4095};
				for (const auto lead : leads) {
					for (const std::uint8_t kind : {0, 1, 2}) {
						/* The destination: `lead` bytes before the
						 * boundary of page 5, the rest in page 5. */
						mops_row(ref, got, rng, run, row,
							tagged(page_at(5) - lead), tagged(page_at(2) + 0x40),
							n, with({{5, kind}}));
						if (set || kind == 1)
							continue;
						/* The source likewise. */
						mops_row(ref, got, rng, run, row,
							tagged(page_at(8) + 0x30), tagged(page_at(5) - lead),
							n, with({{5, kind}}));
						/* Both cross, at different places. */
						for (const auto other : leads)
							mops_row(ref, got, rng, run, row,
								tagged(page_at(8) - other),
								tagged(page_at(5) - lead), n,
								with({{5, kind}, {8, 0}}));
					}
					/* Past the first boundary the next page is unmapped
					 * too: progress, then the fault. */
					if (n > 4096)
						mops_row(ref, got, rng, run, row,
							tagged(page_at(5) - lead),
							tagged(page_at(2) + 0x40), n,
							with({{6, 0}}));
				}
				if (set || n > 200)
					continue;
				/* Source and destination overlap, the destination above or
				 * below, in one page and across a boundary. */
				for (const std::uint64_t delta : {0, 1, 2, 7, 8, 15, 16, 17,
				     63, 64, 65}) {
					mops_row(ref, got, rng, run, row,
						tagged(page_at(3) + 0x400 + delta),
						tagged(page_at(3) + 0x400), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(3) + 0x400),
						tagged(page_at(3) + 0x400 + delta), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) - n / 2 + delta),
						tagged(page_at(4) - n / 2), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) - n / 2),
						tagged(page_at(4) - n / 2 + delta), n,
						with({{4, 0}}));
				}
			}
		}
	}
	/* A set whose fill value has other bits than its low byte. */
	for (const std::uint64_t fill : {0x00ULL, 0xffULL, ~0ULL, 0x100ULL,
	     0xdead'beef'0000'0042ULL})
		for (const std::uint64_t n : {1, 9, 33, 64, 65})
			mops_row(ref, got, rng, run, row_set, page_at(4) + 0x21,
				0, n, plain, fill);
	const auto st = aot::stats();
	std::fprintf(stderr, "memory copy and set: %zu starts compared, %zu "
		"faulted; enters %llu steps %llu slow %llu\n", run.compared,
		run.faulted, static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps),
		static_cast<unsigned long long>(st.mops_slow));
	require(run.compared > 5000);
	require(run.faulted > 500);
	require(st.enters > run.compared / 2);
	require(st.mops_slow > 0);

	/* The thunk's own path: a copy and a set of up to 64 bytes inside one
	 * page each, with the pages in the TLB, leave neither a step nor a call
	 * to the host routine; a larger one calls it once and does not step. */
	MopsRun small;
	for (const auto n : {1ULL, 7ULL, 16ULL, 33ULL, 64ULL}) {
		for (const auto row : {row_cpyf, row_cpy, row_set}) {
			/* Twice: the first start fills the TLB entries. */
			mops_row(ref, got, rng, small, row, page_at(4) + 0x300,
				page_at(2) + 0x100, n, plain);
			aot::reset_stats();
			mops_row(ref, got, rng, small, row, page_at(4) + 0x300,
				page_at(2) + 0x100, n, plain);
			const auto once = aot::stats();
			require(once.enters > 0);
			require(once.mops_slow == 0);
			require(once.steps == 0);
		}
	}
	aot::reset_stats();
	mops_row(ref, got, rng, small, row_cpy, page_at(4) + 0x300,
		page_at(2) + 0x100, 100, plain);
	require(aot::stats().mops_slow == 1);
	require(aot::stats().steps == 0);

	/* A copy that faults on every page: the engine runs the instruction
	 * again at each resume, and rewritten code is entered for it once. */
	{
		auto kinds = plain;
		for (std::size_t page = 6; page < 11; ++page)
			kinds[page] = 0;
		aot::reset_stats();
		mops_row(ref, got, rng, small, row_cpyf, page_at(6), page_at(0),
			5 * page_bytes, kinds);
		const auto resumed = aot::stats();
		require(resumed.mops_fault >= 1);
		require(resumed.mops_fault <= 2);
	}
}

/* A signal handler can leave a copy in the middle of its epilogue with the
 * flags or the count edited, so the stage is entered in states the prologue
 * never formats: the flags clear with bytes left (an exception), N set on a
 * forward copy (ignored), a count above the limit. The engine decides each;
 * rewritten code must agree, including the exception syndrome. */
void memory_copy_and_set_started_at_the_epilogue(const LibraryImage &image)
{
	constexpr std::uint32_t flag_n = 0x8000'0000U, flag_z = 0x4000'0000U,
		flag_c = 0x2000'0000U, flag_v = 0x1000'0000U;
	constexpr std::uint64_t copy_limit = 0x007f'ffff'ffff'ffffULL,
		set_limit = 0x7fff'ffff'ffff'ffffULL;
	const auto bias = Fixture::bias;
	Fixture ref{4611, image}, got{4612, image};
	SplitMix rng{37};
	MopsRun run;
	aot::reset_stats();
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	const std::vector<std::uint8_t> plain(arena_pages + stack_pages, 2);
	auto broken = plain;
	broken[5] = 0;
	const std::uint32_t flags[] = {0, flag_c, flag_n | flag_c,
		flag_z | flag_c, flag_v | flag_c, flag_n, flag_z,
		flag_n | flag_z | flag_c | flag_v};
	for (std::size_t row = 0; row < mops_rows; ++row) {
		const auto limit = mops_is_set(row) ? set_limit : copy_limit;
		const std::uint64_t counts[] = {0, 1, 64, 65, 4097, limit,
			limit + 1, ~0ULL};
		for (const auto nzcv : flags)
			for (const auto n : counts) {
				const MopsStart start{8, nzcv};
				/* Inside one page each. */
				mops_row(ref, got, rng, run, row, page_at(4) + 0x300,
					page_at(2) + 0x100, n, plain, 0x1234, start);
				/* The destination ends in an unmapped page. */
				mops_row(ref, got, rng, run, row, page_at(5) - 20,
					page_at(2) + 0x40, n, broken, 0x1234, start);
				/* The source below an overlapping destination. */
				mops_row(ref, got, rng, run, row, page_at(3) + 0x410,
					page_at(3) + 0x400, n, plain, 0x1234, start);
			}
	}
	const auto st = aot::stats();
	std::fprintf(stderr, "epilogue starts: %zu compared, %zu faulted; "
		"enters %llu steps %llu\n", run.compared, run.faulted,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(run.compared > 1000);
	require(run.faulted > 100);
	require(st.enters > run.compared / 2);
}

/* Advanced SIMD structure accesses that span two pages, with the TLB entry
 * of one of them already filled by an earlier byte load: the first page
 * unmapped and the last cached, and the reverse, for loads and stores, at
 * every split of the access. */
void structure_accesses_over_a_boundary_match_the_engine(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4621, image}, got{4622, image};
	SplitMix rng{43};
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	std::size_t compared = 0;
	aot::reset_stats();
	for (std::size_t row = 0; row < simd_warm_rows; ++row)
		for (const std::uint64_t lead : {1ULL, 8ULL, 15ULL, 16ULL, 17ULL,
		     32ULL, 48ULL, 63ULL, 64ULL})
			for (const std::uint8_t first : {0, 1, 2})
				for (const std::uint8_t last : {0, 1, 2})
					for (const bool cache_first : {false, true})
						for (const std::uint64_t tag : {0ULL, 0x5aULL << 56}) {
							auto c = make_case_from_seed(rng.next(), bias,
								simd_warm_at + row * simd_warm_step, true,
								true, base);
							enterable(c);
							c.kinds.assign(arena_pages + stack_pages, 2);
							c.kinds[5] = first;
							c.kinds[6] = last;
							c.cpu.registers[1] = (page_at(6) - lead) | tag;
							c.cpu.registers[2] = cache_first ?
								page_at(5) + 0x10 : page_at(6) + 0x20;
							compare_case(ref, got, c,
								"structure access over a boundary");
							++compared;
						}
	const auto st = aot::stats();
	std::fprintf(stderr, "structure accesses over a boundary: %zu compared; "
		"enters %llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(st.enters > compared / 2);
}

/* A function that retires a handful of instructions costs more to enter than
 * the engine takes to run it: after two such entries in a row the engine runs
 * it, and rewritten code is entered again one time in 64. */
void short_entries_are_left_to_the_engine(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4701, image}, got{4702, image};
	SplitMix rng{37};
	auto c = make_case_from_seed(rng.next(), bias, mops_at + row_cpyf * mops_step,
		true, true, arena_base(bias));
	enterable(c);
	const auto base = arena_base(bias);
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[2] = 7;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.kinds.assign(arena_pages + stack_pages, 2);
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.done());
	aot::set_enabled(true);
	aot::reset_stats();
	for (int i = 0; i < 200; ++i)
		require(same(want, drive(got.env, got.arena, c, aot_max_blocks), c));
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "short entries: 200 runs, enters %llu, left to the "
		"engine %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refusals[4]));
	require(st.enters >= 2);
	require(st.enters < 100);
	require(st.refusals[4] > 100);
}

/* The history the engine keeps for admission (the address a stepped
 * instruction faulted at, and the addresses whose entries were short) is a
 * hint keyed by the address alone. Nothing resets it when the text under the
 * address changes, a mapping goes or the executor runs in another address
 * space, so the rows below warm it without ever toggling diagnostics (which
 * resets it) and then change each of those: the run must be the engine's,
 * and entering must resume once the text is the framework's again. */
void admission_history_survives_changes_as_a_hint_only(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto row_pc = mops_at + row_cpyf * mops_step;
	const auto changed_at = row_pc + 8;
	constexpr std::uint32_t nop = 0xd503201fU;
	SplitMix rng{53};
	auto c = make_case_from_seed(rng.next(), bias, row_pc, true, true, base);
	enterable(c);
	c.kinds.assign(arena_pages + stack_pages, 2);
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.cpu.registers[2] = 16;
	std::uint32_t original;
	std::memcpy(&original, &image.bytes[changed_at], 4);
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[changed_at], &nop, 4);
	/* What the engine does with each text, computed before AOT is on. */
	aot::set_enabled(false);
	Fixture ref_original{4901, image}, ref_changed{4902, changed};
	const auto want_original = drive(ref_original.env, ref_original.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, c,
		max_blocks);
	require(want_original.done() && want_changed.done());
	require(!identical(want_original, want_changed));
	/* The page that holds the row is not mapped at all. */
	const std::size_t row_page = row_pc / page_bytes;
	Environment hole_ref_env{4903, bias, image, row_page};
	Arena hole_ref_arena{4903, base};
	const auto want_hole = drive(hole_ref_env, hole_ref_arena, c, max_blocks);
	require(want_hole.done());
	require(!identical(want_original, want_hole));

	Fixture got{4904, image};
	Fixture other{4905, changed};
	Environment hole_env{4906, bias, image, row_page};
	Arena hole_arena{4906, base};
	aot::reset_stats();
	aot::set_enabled(true);
	const auto run = [&](Environment &env, Arena &arena,
	                     const Result &want, std::size_t times,
	                     const char *what) {
		for (std::size_t i = 0; i < times; ++i) {
			/* The executor is got's in every row; the address space is
			 * the one named. */
			const auto have = drive_on(got.env.executor, env.memory,
				env.generation, env.bias, arena, c, aot_max_blocks);
			if (!same(want, have, c)) {
				std::fprintf(stderr, "  %s, run %zu\n", what, i);
				require(false);
			}
		}
	};
	/* The change is published in got's own address space by name: the
	 * address space the publication reaches by default is the one the
	 * thread touched last, which is not got's here. */
	const auto publish = [&](std::uint32_t word) {
		got.env.backing.put32(changed_at, word);
		require(darwin_uml_static_vm_invalidate_code_generation(
			got.env.generation, got.env.bias + changed_at, 4) == 0);
	};
	/* The history is warm: the entries are refused and the verdicts live. */
	run(got.env, got.arena, want_original, 200, "warming");
	const auto warm = aot::stats();
	require(warm.enters >= 2);
	require(warm.refusals[4] > 100);
	/* The text under the address changes (and is published). A refused entry
	 * is the engine's; the one in 64 that is tried finds the page changed. */
	publish(nop);
	run(got.env, got.arena, want_changed, 200, "text changed");
	const auto after_change = aot::stats();
	require(after_change.refusals[4] > warm.refusals[4]);
	/* The bytes are back: rewritten code is entered again. */
	publish(original);
	run(got.env, got.arena, want_original, 200, "text restored");
	require(aot::stats().enters > after_change.enters);
	/* Another address space whose text differs at the address. */
	const auto before_other = aot::stats();
	run(other.env, other.arena, want_changed, 200, "other address space");
	require(aot::stats().refusals[4] > before_other.refusals[4]);
	/* Another address space in which the page is not mapped. */
	run(hole_env, hole_arena, want_hole, 200, "unmapped text");
	/* And back in the first one. */
	run(got.env, got.arena, want_original, 200, "first address space again");
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "admission history: enters %llu, left to the "
		"engine %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refusals[4]));
	require(st.poisoned == 0);
}

/* The address a stepped instruction faulted at is kept as a hint too: the
 * guest resumes at it once the fault is handled, and the engine runs it
 * again rather than entering rewritten code. Nothing resets it when the text
 * at that address changes, the page is replaced, or the executor goes on in
 * another address space, so each row stops on a stepped memory fault, changes
 * what is under the faulting address in one of those ways, and resumes at the
 * same address on the same executor without toggling diagnostics (which
 * resets the hint). The run must be what the engine alone does, whatever the
 * hint says, and rewritten code must be entered again once the guest has
 * moved on. */
void a_stepped_fault_hint_survives_changes_as_a_hint_only(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto row_pc = mops_at + row_cpyf * mops_step;
	/* ldr x3, [x0]: x0 is the destination the row's first instruction
	 * faults at, so the address faults again, now as a plain load. */
	constexpr std::uint32_t load = 0xf9400003U;
	const std::size_t row_page = row_pc / page_bytes;
	SplitMix rng{59};
	auto c = make_case_from_seed(rng.next(), bias, row_pc, true, true, base);
	enterable(c);
	c.kinds.assign(arena_pages + stack_pages, 2);
	c.kinds[4] = 0;
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.cpu.registers[2] = 16;
	/* Where the engine stops: the instruction of the row that faults. */
	std::uint64_t fault_at = 0;
	{
		aot::set_enabled(false);
		Fixture probe{4999, image};
		const auto first = drive_on(probe.env.executor, probe.env.memory,
			probe.env.generation, bias, probe.arena, c, max_blocks, {}, true);
		require(first.last == EngineEvent::memory_fault);
		require(first.faults.size() == 1);
		fault_at = first.faults[0].pc - bias;
		require(fault_at >= row_pc && fault_at < row_pc + mops_step);
	}
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[fault_at], &load, 4);

	enum Change : std::size_t {
		nothing, text_patched, page_replaced, other_space, page_unmapped,
		changes,
	};
	static constexpr const char *what[changes] = {"nothing changed",
		"text patched and published", "page replaced",
		"another address space with other text",
		"another address space without the page"};
	struct Ending final {
		Result stopped, ended;
	};
	/* Stop at the fault, change what is under it, resume there. */
	const auto sequence = [&](Change change,
	                          gadgets::GadgetExecutor &executor,
	                          unsigned long id, std::size_t blocks) {
		Environment env{id, bias, image};
		Arena arena{id, base};
		JitBacking replacement{1};
		replacement.put(0, std::span{changed.bytes}.subspan(
			row_page * page_bytes, page_bytes));
		Environment other{id + 1, bias, changed};
		Arena other_arena{id + 1, base};
		Environment hole{id + 2, bias, image, row_page};
		Arena hole_arena{id + 2, base};
		Ending out;
		out.stopped = drive_on(executor, env.memory, env.generation, bias,
			arena, c, blocks, {}, true);
		auto *memory = &env.memory;
		auto generation = env.generation;
		auto *where = &arena;
		switch (change) {
		case nothing:
			break;
		case text_patched:
			env.backing.put32(fault_at, load);
			require(darwin_uml_static_vm_invalidate_code_generation(
				env.generation, bias + fault_at, 4) == 0);
			break;
		case page_replaced:
			env.map.replace_page(bias + row_page * page_bytes,
				replacement);
			break;
		case other_space:
			other_arena.adopt(arena);
			memory = &other.memory;
			generation = other.generation;
			where = &other_arena;
			break;
		case page_unmapped:
			hole_arena.adopt(arena);
			memory = &hole.memory;
			generation = hole.generation;
			where = &hole_arena;
			break;
		case changes:
			break;
		}
		out.ended = run_from(executor, *memory, generation, *where,
			out.stopped.cpu, blocks);
		return out;
	};
	/* What the engine does, on fresh guests, before AOT is on. */
	aot::set_enabled(false);
	gadgets::GadgetExecutor ref_executor;
	std::array<Ending, changes> want;
	for (std::size_t i = 0; i < changes; ++i)
		want[i] = sequence(static_cast<Change>(i), ref_executor,
			5000 + 10 * i, max_blocks);
	for (const auto &w : want) {
		require(w.stopped.last == EngineEvent::memory_fault);
		require(w.stopped.faults.size() == 1);
		require(w.stopped.faults[0].pc == bias + fault_at);
		require(w.ended.done());
	}
	require(!identical(want[nothing].ended, want[text_patched].ended));
	require(!identical(want[nothing].ended, want[page_unmapped].ended));

	gadgets::GadgetExecutor executor;
	aot::reset_stats();
	aot::set_enabled(true);
	for (std::size_t i = 0; i < changes; ++i) {
		const auto steps = aot::stats().steps;
		const auto have = sequence(static_cast<Change>(i), executor,
			5100 + 10 * i, aot_max_blocks);
		/* The fault was a stepped instruction's, which is what arms the
		 * hint. */
		if (aot::stats().steps <= steps) {
			std::fprintf(stderr, "  %s: no stepped instruction\n", what[i]);
			require(false);
		}
		if (!same(want[i].stopped, have.stopped, c) ||
		    !same(want[i].ended, have.ended, c)) {
			std::fprintf(stderr, "  %s\n", what[i]);
			require(false);
		}
	}
	const auto after_changes = aot::stats();
	require(after_changes.poisoned == 0);
	/* Back in an address space with the framework's text, with every page
	 * mapped: the guest has moved on, and rewritten code is entered. */
	auto ok = c;
	ok.kinds.assign(arena_pages + stack_pages, 2);
	Fixture again{5200, image};
	aot::set_enabled(true);
	for (int i = 0; i < 200; ++i)
		(void)drive_on(executor, again.env.memory, again.env.generation,
			bias, again.arena, ok, aot_max_blocks);
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "stepped fault hint: %zu changes, enters %llu, "
		"steps %llu\n", static_cast<std::size_t>(changes),
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(st.enters > after_changes.enters);
	require(st.poisoned == 0);
}

/* The guest kernel's threads run the executor on a 64 KiB stack, with its own
 * frames and a signal frame above ours: nothing on the way into rewritten
 * code may need much of it (a 64 KiB temporary in the executor killed the
 * first boot of a guest). The scenarios below run on the last 64 KiB of a
 * mapping whose first megabyte is inaccessible, painted beforehand, so an
 * overflow faults and the high-water mark is measured: a first entry (the
 * verification of a page and a refill), a native copy, the host routine
 * for a longer one, a fault that is resolved and resumed, a step the
 * engine takes (an exception), and structure accesses that span a
 * boundary. */
void rewritten_code_stays_within_a_stack_budget(const LibraryImage &image)
{
	constexpr std::size_t stack_bytes = 64 * 1024;
	constexpr std::size_t moat_bytes = 1024 * 1024;
	/* What the executor, the harness and the host routines may use beyond
	 * the thread's own start-up. */
	constexpr std::size_t budget = 16 * 1024;
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	struct Scenario final {
		std::unique_ptr<Fixture> fixture;
		Case c;
	};
	std::vector<Scenario> scenarios;
	unsigned long generation = 4800;
	SplitMix rng{47};
	const auto add = [&](std::uint64_t pc_offset) -> Case & {
		scenarios.push_back({std::make_unique<Fixture>(++generation, image),
			make_case_from_seed(rng.next(), bias, pc_offset, true, true,
				base)});
		auto &c = scenarios.back().c;
		enterable(c);
		c.kinds.assign(arena_pages + stack_pages, 2);
		return c;
	};
	const auto row_at = [&](std::size_t row) {
		return mops_at + row * mops_step;
	};
	{	/* A first entry: verification, refill, the native copy. */
		auto &c = add(row_at(row_cpyf));
		c.cpu.registers[0] = page_at(4) + 0x300;
		c.cpu.registers[1] = page_at(2) + 0x100;
		c.cpu.registers[2] = 16;
	}
	{	/* The host routine: a longer copy across a boundary. */
		auto &c = add(row_at(row_cpy));
		c.cpu.registers[0] = page_at(4) + 0xff0;
		c.cpu.registers[1] = page_at(2) + 0x100;
		c.cpu.registers[2] = 3 * page_bytes;
	}
	{	/* A fault, resolved and resumed. */
		auto &c = add(row_at(row_cpyf));
		c.kinds[5] = 0;
		c.cpu.registers[0] = page_at(5) - 20;
		c.cpu.registers[1] = page_at(2) + 0x40;
		c.cpu.registers[2] = 5000;
	}
	{	/* The engine takes an exception: bytes left with the flags clear. */
		auto &c = add(row_at(row_set) + 8);
		c.cpu.nzcv = 0;
		c.cpu.registers[0] = page_at(4) + 0x300;
		c.cpu.registers[2] = 65;
	}
	{	/* A structure access over a boundary, first page unmapped. */
		auto &c = add(simd_warm_at);
		c.kinds[5] = 0;
		c.cpu.registers[1] = page_at(6) - 16;
		c.cpu.registers[2] = page_at(6) + 0x20;
	}
	{	/* A structure store over a boundary, last page read-only. */
		auto &c = add(simd_warm_at + simd_warm_step);
		c.kinds[6] = 1;
		c.cpu.registers[1] = page_at(6) - 48;
		c.cpu.registers[2] = page_at(5) + 0x20;
	}
	aot::set_enabled(true);
	aot::reset_stats();
	const auto run = [&](std::size_t count) -> std::size_t {
		void *const mapping = mmap(nullptr, moat_bytes + stack_bytes,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED);
		require(mprotect(mapping, moat_bytes, PROT_NONE) == 0);
		auto *const stack = static_cast<std::uint64_t *>(
			static_cast<void *>(static_cast<char *>(mapping) + moat_bytes));
		constexpr std::uint64_t paint = 0xa5a5'5a5a'c3c3'3c3cULL;
		for (std::size_t i = 0; i < stack_bytes / 8; ++i)
			stack[i] = paint;
		struct Job final {
			std::vector<Scenario> *scenarios;
			std::size_t count;
		} job{&scenarios, count};
		pthread_attr_t attributes;
		require(pthread_attr_init(&attributes) == 0);
		require(pthread_attr_setstack(&attributes, stack, stack_bytes) == 0);
		pthread_t thread;
		require(pthread_create(&thread, &attributes, [](void *argument) -> void * {
			auto *work = static_cast<Job *>(argument);
			for (std::size_t i = 0; i < work->count; ++i) {
				auto &one = (*work->scenarios)[i];
				(void)drive(one.fixture->env, one.fixture->arena, one.c,
					aot_max_blocks);
			}
			return nullptr;
		}, &job) == 0);
		require(pthread_join(thread, nullptr) == 0);
		pthread_attr_destroy(&attributes);
		std::size_t untouched = 0;
		while (untouched < stack_bytes / 8 && stack[untouched] == paint)
			++untouched;
		require(munmap(mapping, moat_bytes + stack_bytes) == 0);
		return stack_bytes - untouched * 8;
	};
	/* An empty run measures what the thread itself uses. The scenarios then
	 * run one after another on a fresh stack, each on its own fixture, so
	 * each one verifies its pages on that stack; the mark is the deepest. */
	const auto start_up = run(0);
	const auto high_water = run(scenarios.size());
	aot::set_enabled(false);
	std::fprintf(stderr, "stack: thread start-up %zu bytes, high water "
		"with the executor %zu bytes (budget %zu above start-up)\n",
		start_up, high_water, budget);
	require(aot::stats().enters > 0);
	require(high_water <= start_up + budget);
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
	char more[400];
	const int m = std::snprintf(more, sizeof(more),
		"x8 %llx x9 %llx x10 %llx x11 %llx x12 %llx x13 %llx x14 %llx x15 %llx x29 %llx\n",
		(unsigned long long)ss.__x[8], (unsigned long long)ss.__x[9], (unsigned long long)ss.__x[10], (unsigned long long)ss.__x[11], (unsigned long long)ss.__x[12], (unsigned long long)ss.__x[13], (unsigned long long)ss.__x[14], (unsigned long long)ss.__x[15], (unsigned long long)ss.__x[29]);
	(void)!write(2, more, static_cast<std::size_t>(m));
	void *frames[24];
	const int depth = backtrace(frames, 24);
	backtrace_symbols_fd(frames, depth, 2);
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
	/* libz when the build holds it (some scenarios name its functions),
	 * otherwise the first library. */
	std::size_t scenario_index = libraries.front().index;
	for (const auto &file : libraries) {
		if (file.guest.ends_with("/libz.so")) {
			scenario_index = file.index;
			break;
		}
	}
	for (const auto &file : libraries) {
		if (cfg.library >= 0 &&
		    static_cast<long>(file.index) != cfg.library)
			continue;
		const auto image = load_library(file.host);
		g_sites_image = &image;
		g_fixture_library = file.guest.ends_with("/libaotfixture.so");
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
		/* No entry ran longer than the budget allows (counted one
		 * instruction at a time with UML_AOT_COUNT_RETIRED). */
		require(st.max_retired <= entry_bound);
		/* Both entry refusals are exercised by every library's cases. */
		require(st.refusals[1] > 0);
		require(st.refusals[3] > 0);
		if (file.guest.ends_with("/libaotfixture.so")) {
			/* The same cases under the profile the runtime probes, named
			 * here whatever default the test binary installs (LSE2 among
			 * its features: unaligned atomics inside a granule do not
			 * fault). */
			const auto probed =
				::uml::platform::apple::probed_host_capabilities();
			const ::uml::cpu::HostCapabilitiesScope scope{probed};
			aot::reset_stats();
			const auto p = differential(cfg, image, sample, sites);
			const auto ps = aot::stats();
			std::fprintf(stderr, "probed profile (lse2 %d): %zu cases, "
				"%zu compared, %zu finished, %zu faults; enters %llu\n",
				probed.has(::uml::cpu::ArmFeature::lse2) ? 1 : 0, p.cases,
				p.compared, p.finished, p.fault_events,
				static_cast<unsigned long long>(ps.enters));
			require(p.compared > p.cases / 2);
			require(ps.enters > p.compared / 2);
			require(ps.poisoned == 0);
			loop_through_another_range(image, functions);
			code_change_in_the_dispatch_window(image, functions);
			edges_into_a_page_that_differs(image);
			edges_into_a_page_that_matches(image);
			next_page_changes_after_its_neighbour_was_compared(image);
			edge_into_another_instance(image);
			atomics_are_shared_between_aot_and_engine(image);
			cas_outcomes_and_permissions(image);
			misaligned_cas_follows_the_profile(image);
			more_instances_than_the_binder_holds(image);
			code_change_history_overflows(image, functions);
			a_changed_bit_anywhere_in_a_page_is_refused(image, functions);
			a_shootdown_reaches_a_native_loop(image);
			memory_copy_and_set_match_the_engine(image);
			memory_copy_and_set_started_at_the_epilogue(image);
			structure_accesses_over_a_boundary_match_the_engine(image);
			short_entries_are_left_to_the_engine(image);
			admission_history_survives_changes_as_a_hint_only(image);
			a_stepped_fault_hint_survives_changes_as_a_hint_only(image);
			rewritten_code_stays_within_a_stack_budget(image);
		}
		/* The scenarios are about the mechanism, not about the code:
		 * they run on one library, whatever order the build lists. */
		if (file.index == scenario_index) {
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
			changed_text_falls_back(image, functions);
			refill_keeps_state(image, sample);
			partially_mapped_library(image, functions);
			two_vcpus(image, sample);
		}
	}
	if (libraries.size() > 1 && cfg.library < 0) {
		/* The scenario library and the one listed after it. */
		std::size_t at = 0;
		while (libraries[at].index != scenario_index)
			++at;
		const auto &first = libraries[at];
		const auto &second = libraries[(at + 1) % libraries.size()];
		const auto image = load_library(first.host);
		const auto other = load_library(second.host);
		cross_library_branches(image, load_functions(cfg.functions,
			first.index), other, load_functions(cfg.functions,
			second.index));
	}
	return 0;
}
