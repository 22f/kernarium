// SPDX-License-Identifier: GPL-2.0-or-later
/* The x86-64 JIT over a frame that more than one address maps, on the real
 * static MMU. A frame is compiled at one address (X) and written through
 * another (W); the rows read what the guest can see and the counters the
 * engine and the MMU keep for what must not happen.
 *
 *   store      a store through W reaches X's code: the unit that compiled X
 *              registered its frame first, W's page publishes though it is
 *              not executable, and the next run of X executes the new bytes.
 *   running    a store through W into the unit that is running leaves the
 *              unit before the store, which then runs as a unit of its own:
 *              the guest sees the new immediate.
 *   decoder    a page the engine compiled registers its frame once while its
 *              decoder range stands, and again after the page's holder was
 *              removed and the page mapped back.
 *   failure    a registration that fails reports the fetch fault the kernel
 *              resolves, retires nothing and caches nothing; the run goes on
 *              once a registration succeeds, and 64 failures in a row end the
 *              space. */
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "cpu/x86_64/segments.hpp"
#include "frame_accounting.hpp"
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <sys/mman.h>
#include <vector>

namespace {

namespace x86_64 = uml::cpu::x86_64;
namespace jit = uml::cpu::x86_64::jit;
using uml::memory::FrameAccounting;
using uml::memory::StaticGuestMemory;
using uml::memory::WriterLocation;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rw = PROT_READ | PROT_WRITE;
constexpr int rx = PROT_READ | PROT_EXEC;

[[nodiscard]] bool generation_live(unsigned long generation) noexcept
{
	return darwin_uml_static_vm_generation_live(generation) != 0;
}

/* One address space and one engine on it, run as the runtime runs one: the
 * execution context entered, the engine handed what changed, then the run. */
class Space final {
public:
	explicit Space(unsigned long generation)
		: generation_{generation},
		  context_{darwin_uml_static_vm_execution_context_create(generation)},
		  engine_(uml::platform::apple::probed_host_capabilities(),
			  jit::FexEngineOptions{.generation_live = &generation_live})
	{
		require(context_ != nullptr);
		memory_.set_generation(generation);
	}

	~Space()
	{
		(void)darwin_uml_static_vm_destroy(generation_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	void map(std::uint64_t address, ScratchBacking &backing,
		unsigned long first_page, int protection, int guest_code)
	{
		require(darwin_uml_static_vm_map_generation(generation_,
			reinterpret_cast<void *>(address), backing.descriptor(),
			first_page * page, page, protection, guest_code, 1) == 0,
			"a mapping was refused");
	}

	void unmap(std::uint64_t address)
	{
		require(darwin_uml_static_vm_unmap_generation(generation_,
			reinterpret_cast<void *>(address), page) == 0);
	}

	/* What the runtime hands the engine before each run. */
	void sync(const darwin_uml_static_sequences &sequences)
	{
		const auto mode = x86_64::ExecutionMode::long64;
		engine_.settle_stuck_participants(generation_, mode);
		jit::AddressSpaceChanges changes{};
		changes.mapping_sequence = sequences.mapping_changes;
		changes.code_sequence = sequences.code_changes;
		std::uint64_t mapping_seen = 0;
		std::uint64_t code_seen = 0;
		if (!engine_.consumed_sequences(generation_, mode, mapping_seen,
			code_seen)) {
			engine_.apply_changes(generation_, mode, changes, false);
			return;
		}
		if (mapping_seen == sequences.mapping_changes &&
			code_seen == sequences.code_changes)
			return;
		std::array<darwin_uml_static_mapping_change, 256> mapped;
		std::array<jit::GuestRange, 256> mapping_ranges;
		if (mapping_seen != sequences.mapping_changes) {
			std::uint64_t current = sequences.mapping_changes;
			const int count = darwin_uml_static_vm_mapping_changes(generation_,
				mapping_seen, mapped.data(), mapped.size(), &current);
			if (count < 0) {
				changes.all_mappings = true;
			} else {
				for (int index = 0; index < count; ++index)
					mapping_ranges[index] = {mapped[index].address,
						mapped[index].length};
				changes.mapping_ranges = {mapping_ranges.data(),
					static_cast<std::size_t>(count)};
				changes.mapping_sequence = current;
			}
		}
		std::array<darwin_uml_static_code_change, 256> coded;
		std::array<jit::GuestRange, 256> code_ranges;
		if (code_seen != sequences.code_changes) {
			std::uint64_t current = sequences.code_changes;
			const int count =
				darwin_uml_static_vm_execution_context_code_change_ranges(
					context_, code_seen, coded.data(), coded.size(), &current);
			if (count < 0) {
				changes.all_code = true;
			} else {
				for (int index = 0; index < count; ++index)
					code_ranges[index] = {coded[index].address,
						coded[index].length};
				changes.code_ranges = {code_ranges.data(),
					static_cast<std::size_t>(count)};
				changes.code_sequence = current;
			}
		}
		engine_.apply_changes(generation_, mode, changes, false);
	}

	[[nodiscard]] jit::JitRunResult run(x86_64::CpuState &cpu)
	{
		require(darwin_uml_static_vm_execution_context_begin(context_,
			cpu.rip) == 0, "the execution context was not entered");
		const auto sequences =
			darwin_uml_static_vm_execution_context_sequences(context_);
		memory_.prepare(generation_, sequences.mapping_changes);
		sync(sequences);
		uml::cpu::InterruptToken interrupt{};
		const auto result = engine_.run(cpu, memory_, {
			.generation = generation_,
			.instruction_budget = 1000,
			.execution_context = context_,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		return result;
	}

	/* A fresh CPU at `address` with RBX and RDX set. */
	[[nodiscard]] static x86_64::CpuState cpu_at(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		x86_64::CpuState cpu{};
		cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
		(void)x86_64::enter_segments(cpu);
		cpu.rip = address;
		cpu.rflags = 0x202;
		cpu.gpr(x86_64::Register::rbx) = rbx;
		cpu.gpr(x86_64::Register::rdx) = rdx;
		return cpu;
	}

	/* RAX at the block's system call, running on after the exits that leave
	 * for a store into the block. */
	[[nodiscard]] std::uint64_t to_system_call(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		auto cpu = cpu_at(address, rbx, rdx);
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			const auto result = run(cpu);
			if (result.execution.reason == uml::cpu::ExitReason::system_call)
				return cpu.gpr(x86_64::Register::rax);
		}
		require(false, "the block never reached its system call");
		return 0;
	}

	[[nodiscard]] jit::FexEngine &engine() noexcept { return engine_; }
	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
	void *context_;
	StaticGuestMemory memory_;
	jit::FexEngine engine_;
};

void put(ScratchBacking &backing, std::size_t offset,
	std::initializer_list<std::uint8_t> bytes)
{
	std::vector<std::byte> data;
	for (const auto byte : bytes)
		data.push_back(static_cast<std::byte>(byte));
	backing.put(offset, data);
}

/* mov eax, imm32 ; syscall */
void load_return(ScratchBacking &backing, std::uint8_t value)
{
	put(backing, 0, {0xb8, value, 0x00, 0x00, 0x00, 0x0f, 0x05});
}

/* mov [rdx], bl ; syscall */
void load_writer(ScratchBacking &backing)
{
	put(backing, 0, {0x88, 0x1a, 0x0f, 0x05});
}

[[nodiscard]] bool held_at(
	const std::shared_ptr<uml::memory::MappedBacking> &backing,
	unsigned long generation, std::uint64_t address)
{
	std::vector<WriterLocation> holders;
	FrameAccounting::holders_for_testing(*backing, 0, holders);
	const auto state = uml::memory::testing::generation_state(generation);
	if (state == nullptr)
		return false;
	for (const auto &holder : holders)
		if (holder.page == address && holder.alias == state->accounting.alias)
			return true;
	return false;
}

void test_store()
{
	constexpr std::uint64_t executable = 0x1000'0000;
	constexpr std::uint64_t alias = 0x2000'0000;
	constexpr std::uint64_t writer = 0x3000'0000;
	ScratchBacking frame{"uml-x86-jit-alias-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-alias-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9201};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	require(space.to_system_call(executable) == 1);
	require(held_at(backing, space.generation(), executable),
		"the unit that compiled X did not register its frame");
	(void)space.to_system_call(writer, 2, alias + 1);
	require(space.to_system_call(executable) == 2,
		"a store through another address did not reach the compiled code");
	require(space.engine().diagnostics().registration_failures == 0);
}

void test_running_block()
{
	constexpr std::uint64_t executable = 0x1100'0000;
	constexpr std::uint64_t alias = 0x2100'0000;
	ScratchBacking frame{"uml-x86-jit-running-XXXXXX", page};
	/* mov [rdx], bl ; mov eax, imm32 ; syscall */
	put(frame, 0, {0x88, 0x1a, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9202};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	require(space.to_system_call(executable, 1, alias + 3) == 1);
	require(space.to_system_call(executable, 2, alias + 3) == 2,
		"the running unit ran its old bytes after a store through W");
}

void test_decoder_range()
{
	constexpr std::uint64_t executable = 0x1200'0000;
	ScratchBacking frame{"uml-x86-jit-range-XXXXXX", page};
	load_return(frame, 3);
	Space space{9203};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(space.to_system_call(executable) == 3);
	const auto first = space.engine().diagnostics().frame_registrations;
	require(first >= 1 && held_at(backing, space.generation(), executable));
	/* Compiled again with nothing changed, it asks no more. */
	require(space.to_system_call(executable) == 3);
	require(space.engine().diagnostics().frame_registrations == first,
		"an unchanged page registered its frame again");
	/* The page goes and comes back: its holder is removed, and the next
	 * compile registers it before it reads a byte. */
	space.unmap(executable);
	require(!held_at(backing, space.generation(), executable),
		"the unmap left the page's holder");
	space.map(executable, frame, 0, rx, 1);
	require(space.to_system_call(executable) == 3);
	require(space.engine().diagnostics().frame_registrations == first + 1,
		"a page mapped back was compiled without registering its frame");
	require(held_at(backing, space.generation(), executable));
}

/* Fails every registration that reaches the holder list, as memory that
 * stays exhausted would. */
void fail_every_holder_list(uml::memory::testing::AllocationSite site) noexcept
{
	if (site == uml::memory::testing::AllocationSite::holders)
		uml::memory::testing::fail_next_allocation(site);
}

void test_registration_failure()
{
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1300'0000;
	ScratchBacking frame{"uml-x86-jit-failure-XXXXXX", page};
	load_return(frame, 5);
	Space space{9204};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	/* One failure: the fetch fault the kernel resolves, nothing retired. */
	auto cpu = Space::cpu_at(executable);
	testing::fail_allocation_at(testing::AllocationSite::holders, 1);
	auto result = space.run(cpu);
	testing::clear_allocation_failure();
	require(result.execution.reason == uml::cpu::ExitReason::memory_fault &&
		result.execution.fault.access == uml::memory::MemoryAccess::execute,
		"a failed registration did not report a fetch fault");
	require(result.execution.retired_instructions == 0 &&
		cpu.rip == executable, "a failed registration retired an instruction");
	/* FEXCore asks for the page again as it reports the fault, and that
	 * query registers it: a holder without code is no harm. */
	require(space.engine().diagnostics().registration_failures == 1);
	/* The kernel resumes the instruction; the registration now succeeds. */
	require(space.to_system_call(executable) == 5);
	require(space.engine().diagnostics().consecutive_registration_failures == 0,
		"a registration that succeeded left the failures counted");
	require(held_at(backing, space.generation(), executable));

	/* Failures in a row end the space. */
	ScratchBacking other{"uml-x86-jit-limit-XXXXXX", page};
	load_return(other, 6);
	constexpr std::uint64_t unlucky = 0x1400'0000;
	space.map(unlucky, other, 0, rx, 1);
	auto again = Space::cpu_at(unlucky);
	bool fatal = false;
	testing::set_allocation_observer(&fail_every_holder_list);
	testing::fail_next_allocation(testing::AllocationSite::holders);
	for (unsigned attempt = 0; attempt < jit::FexEngine::registration_failure_limit + 2;
		++attempt) {
		const auto outcome = space.run(again);
		if (outcome.execution.reason == uml::cpu::ExitReason::fatal) {
			fatal = true;
			break;
		}
		require(outcome.execution.reason == uml::cpu::ExitReason::memory_fault);
	}
	testing::set_allocation_observer(nullptr);
	testing::clear_allocation_failure();
	require(fatal, "the failures in a row did not end the space");
	require(space.engine().diagnostics().failed_spaces == 1);
	require(space.engine().diagnostics().registration_failures ==
		1 + jit::FexEngine::registration_failure_limit);
}

} // namespace

int main()
{
	const auto start = FrameAccounting::diagnostics();
	test_store();
	std::puts("store ok");
	test_running_block();
	std::puts("running ok");
	test_decoder_range();
	std::puts("decoder ok");
	test_registration_failure();
	std::puts("failure ok");
	const auto now = FrameAccounting::diagnostics();
	require(now.holder_leaks == start.holder_leaks, "a holder leaked");
	require(now.unquiesced_holder_removals == start.unquiesced_holder_removals);
	require(now.unmarked_holder_removals == start.unmarked_holder_removals);
	require(now.errors == start.errors, "an accounting error was counted");
	std::puts("x86-64 JIT alias tests passed");
	return EXIT_SUCCESS;
}
