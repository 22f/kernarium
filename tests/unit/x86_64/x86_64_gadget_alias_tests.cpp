// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine over a frame that more than one address maps.
 *
 * A frame is translated at one address (X) and written through another (W),
 * or at an address of another address space. The rows run the engine over
 * the real static MMU and read what the guest can see: the value a block
 * returns after the store, the tag a write entry carries, and the counters
 * the engine and the MMU keep for what must not happen. Each row names the
 * behaviour it holds; the counters that must stay 0 are read at the end of
 * the run, whatever a row did.
 *
 *   store      a store through W reaches X's translation: the write entry is
 *              untagged while the frame has no holder, retired by X's
 *              registration, and refilled tagged; a frame with no holder
 *              stays untagged.
 *   running    a store through W into the block that is running leaves the
 *              block before it runs the old bytes, for the ordinary store, a
 *              push and the baked entries alike.
 *   serial     an address space's flush leaves its bodies' stamps untouched
 *              yet makes the body compare its source once and register its
 *              frame again; another address space's body on the same frame
 *              keeps its stamp hit.
 *   transient  a registration that fails (allocation) runs the instruction
 *              as a one-instruction transient program and registers on the
 *              next dispatch; no translation becomes runnable unregistered.
 */
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/software_tlb.hpp"
#include "frame_accounting.hpp"
#include "memory/static_guest_memory.hpp"
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
#include <utility>
#include <vector>

namespace {

using namespace uml::cpu::x86_64;
using uml::cpu::EngineEvent;
using uml::memory::FrameAccounting;
using uml::memory::StaticGuestMemory;
using uml::memory::WriterLocation;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rw = PROT_READ | PROT_WRITE;
constexpr int rx = PROT_READ | PROT_EXEC;
constexpr std::uint64_t write_tag_mask = 0xfff;
constexpr std::uint64_t write_code_tag = UML_X86_64_TLB_WRITE_CODE;

[[nodiscard]] gadgets::ProgramCacheSequences cache_sequences(
	void *context) noexcept
{
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	return {sequences.mapping_changes, sequences.code_changes,
		sequences.execution_interrupt != 0};
}

[[nodiscard]] int cache_code_changes(void *context, std::uint64_t sequence,
	gadgets::ProgramCodeChange *changes, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, changes, capacity, current_sequence);
}

[[nodiscard]] int cache_mapping_changes(void *, std::uint64_t generation,
	std::uint64_t sequence, gadgets::ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_mapping_changes(generation, sequence,
		changes, capacity, current_sequence);
}

struct Run final {
	EngineEvent event;
	std::size_t retired;
	std::uint64_t rax;
	std::uint64_t rip;
};

/* One address space and one executor on it, run as the runtime runs it: the
 * execution context entered, the executor prepared from its sequences, then
 * a block selected and executed. */
class Space final {
public:
	explicit Space(unsigned long generation)
		: generation_{generation},
		  context_{darwin_uml_static_vm_execution_context_create(generation)}
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

	/* One dispatch from `cpu`: the block at its RIP, run to its exit. */
	[[nodiscard]] Run step(CpuState &cpu)
	{
		require(darwin_uml_static_vm_execution_context_begin(context_,
			cpu.rip) == 0, "the execution context was not entered");
		const auto sequences =
			darwin_uml_static_vm_execution_context_sequences(context_);
		memory_.prepare(generation_, sequences.mapping_changes);
		executor_.prepare(memory_, generation_, sequences.mapping_changes,
			sequences.code_changes, context_, cache_sequences,
			cache_code_changes, cache_mapping_changes);
		const auto selected = executor_.select_program(
			memory_, cpu.rip, ExecutionMode::long64, 64);
		require(selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr, "a block did not build");
		const auto result = gadgets::execute(
			*selected.program, cpu, memory_, executor_, 64);
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		return {result.event, result.retired_instructions,
			cpu.gpr(Register::rax), cpu.rip};
	}

	[[nodiscard]] Run run(std::uint64_t address, std::uint64_t rbx = 0,
		std::uint64_t rdx = 0)
	{
		CpuState cpu{};
		cpu.rip = address;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rdx) = rdx;
		return step(cpu);
	}

	/* Dispatches until the block makes its system call, as the runtime does
	 * across a block that leaves early. */
	[[nodiscard]] Run run_to_system_call(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		CpuState cpu{};
		cpu.rip = address;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rdx) = rdx;
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			const auto result = step(cpu);
			if (result.event == EngineEvent::system_call)
				return result;
		}
		require(false, "the block never reached its system call");
		return {};
	}

	/* The write entry of the page at `address`: its tags, or ~0 when the
	 * entry holds another page. */
	[[nodiscard]] std::uint64_t write_tags(std::uint64_t address)
	{
		const auto &entry =
			executor_.tlb_entries()[native_tlb_index(address)];
		const auto held = entry.write_page;
		if ((held & ~write_tag_mask) != (address & ~write_tag_mask))
			return ~std::uint64_t{0};
		return held & write_tag_mask;
	}

	[[nodiscard]] const gadgets::ProgramCacheDiagnostics &diagnostics() const
		noexcept
	{
		return executor_.cache_diagnostics();
	}

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
	void *context_;
	StaticGuestMemory memory_;
	gadgets::Executor executor_;
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

std::vector<WriterLocation> holders_of(
	const std::shared_ptr<uml::memory::MappedBacking> &backing,
	unsigned long frame)
{
	std::vector<WriterLocation> holders;
	FrameAccounting::holders_for_testing(*backing, frame * page, holders);
	return holders;
}

/* Whether the generation holds the frame at `address`: the holder names the
 * generation by its state's alias id. */
[[nodiscard]] bool held_at(const std::vector<WriterLocation> &holders,
	unsigned long generation, std::uint64_t address)
{
	const auto state = uml::memory::testing::generation_state(generation);
	if (state == nullptr)
		return false;
	for (const auto &holder : holders)
		if (holder.page == address && holder.alias == state->accounting.alias)
			return true;
	return false;
}

/* A store through W reaches the translation at X, and the tag of W's write
 * entry follows the frame's holders. */
void test_store()
{
	constexpr std::uint64_t executable = 0x1000'0000;
	constexpr std::uint64_t alias = 0x2000'0000;
	constexpr std::uint64_t unheld = 0x2000'4000;
	constexpr std::uint64_t writer = 0x3000'0000;
	ScratchBacking frame{"uml-x86-alias-store-XXXXXX", page};
	ScratchBacking other{"uml-x86-alias-other-XXXXXX", page};
	ScratchBacking code{"uml-x86-alias-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9101};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	space.map(unheld, other, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	/* The frame has no holder: a store through W fills an untagged entry. */
	require(space.run(writer, 1, alias + 1).event == EngineEvent::system_call);
	require(space.write_tags(alias) == 0,
		"a write entry for a frame with no holder was tagged");

	/* X translates, which registers its frame and retires the untagged
	 * entry W's store left. */
	const auto first = space.run(executable);
	require(first.event == EngineEvent::system_call && first.rax == 1);
	require(held_at(holders_of(backing, 0), space.generation(), executable),
		"X's translation did not register its frame");
	require(space.write_tags(alias) == ~std::uint64_t{0},
		"the registration did not retire W's untagged entry");

	/* The store through W refills the entry tagged, and X sees the byte. */
	require(space.run(writer, 2, alias + 1).event == EngineEvent::system_call);
	require((space.write_tags(alias) & write_code_tag) != 0,
		"a write entry for a frame with a holder was not tagged");
	require(space.run(executable).rax == 2,
		"a store through another address did not reach the translation");

	/* A frame no executor translated stays untagged. */
	require(space.run(writer, 3, unheld).event == EngineEvent::system_call);
	require(space.write_tags(unheld) == 0,
		"a write entry for an unheld frame was tagged");
	require(space.diagnostics().unheld_runnable == 0);
}

/* A block that stores through W into its own following bytes leaves before it
 * runs them: the guest sees the new immediate, not the old. */
void test_running_block()
{
	constexpr std::uint64_t executable = 0x1100'0000;
	constexpr std::uint64_t alias = 0x2100'0000;
	ScratchBacking frame{"uml-x86-alias-running-XXXXXX", page};
	/* mov [rdx], bl ; mov eax, imm32 ; syscall: the store names the
	 * immediate's low byte. */
	put(frame, 0, {0x88, 0x1a, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9102};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	require(space.run_to_system_call(executable, 1, alias + 3).rax == 1);
	require(space.run_to_system_call(executable, 2, alias + 3).rax == 2,
		"the running block ran its old bytes after a store through W");
	require(space.diagnostics().unheld_runnable == 0);
}

/* The same through a push: the stack is W, and the push writes eight bytes
 * over the block that runs it. */
void test_running_push()
{
	constexpr std::uint64_t executable = 0x1400'0000;
	constexpr std::uint64_t alias = 0x2400'0000;
	ScratchBacking frame{"uml-x86-alias-push-XXXXXX", page};
	/* push rbx ; mov eax, imm32 ; syscall */
	put(frame, 0, {0x53, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9106};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	/* RBX holds the bytes of the same block with the immediate changed. */
	const auto same = std::uint64_t{0x050f'0000'0001'b853};
	const auto changed = std::uint64_t{0x050f'0000'0002'b853};
	for (const auto [rbx, expected] : {std::pair{same, 1U}, {changed, 2U}}) {
		CpuState cpu{};
		cpu.rip = executable;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rsp) = alias + 8;
		Run result{};
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			result = space.step(cpu);
			if (result.event == EngineEvent::system_call)
				break;
		}
		require(result.event == EngineEvent::system_call);
		require(result.rax == expected,
			"the running block ran its old bytes after a push through W");
	}
	require(space.diagnostics().unheld_runnable == 0);
}

/* An address space's flush moves its flush serial and no stamp: its bodies
 * compare their source once and register again, while the body of another
 * address space on the same frame keeps its stamp hit. */
void test_flush_serial()
{
	constexpr std::uint64_t executable = 0x1200'0000;
	ScratchBacking frame{"uml-x86-alias-serial-XXXXXX", page};
	load_return(frame, 7);
	Space first{9103};
	Space second{9104};
	first.map(executable, frame, 0, rx, 1);
	second.map(executable, frame, 0, rx, 1);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	require(held_at(holders_of(backing, 0), first.generation(), executable) &&
		held_at(holders_of(backing, 0), second.generation(), executable));

	const auto serials = FrameAccounting::diagnostics().serial_bumps;
	const auto first_before = first.diagnostics();
	const auto second_before = second.diagnostics();
	require(darwin_uml_static_vm_flush_generation(first.generation()) == 0);
	require(FrameAccounting::diagnostics().serial_bumps == serials + 1,
		"the flush did not move the serial once");
	require(!held_at(holders_of(backing, 0), first.generation(), executable),
		"the flush left a holder of the generation");
	require(held_at(holders_of(backing, 0), second.generation(), executable));
	/* The page-table walk the kernel makes next republishes the mapping. */
	first.map(executable, frame, 0, rx, 1);

	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	const auto &first_after = first.diagnostics();
	const auto &second_after = second.diagnostics();
	require(first_after.flush_serial_misses ==
		first_before.flush_serial_misses + 1,
		"the flushed generation's body did not miss on the serial");
	require(first_after.validation_stamp_hits ==
		first_before.validation_stamp_hits,
		"the flushed generation's body hit its stamp");
	require(first_after.holder_registrations ==
		first_before.holder_registrations + 1,
		"the flushed generation's body did not register again");
	require(held_at(holders_of(backing, 0), first.generation(), executable));
	require(second_after.flush_serial_misses ==
		second_before.flush_serial_misses,
		"another generation's body missed on the serial");
	require(second_after.holder_registrations ==
			second_before.holder_registrations &&
		second_after.translations == second_before.translations &&
		second_after.validation_mismatches ==
			second_before.validation_mismatches,
		"another generation's body registered or translated again");
	require(first_after.validation_hits == first_before.validation_hits + 1,
		"the flushed generation's body did not match its source");
	require(first_after.unheld_runnable == 0 && second_after.unheld_runnable == 0);
}

/* A registration that fails runs the instruction as a transient program of
 * one, and registers on the next dispatch. */
void test_registration_failure()
{
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1300'0000;
	ScratchBacking frame{"uml-x86-alias-failure-XXXXXX", page};
	load_return(frame, 5);
	Space space{9105};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	const auto before = space.diagnostics();
	testing::fail_allocation_at(testing::AllocationSite::records, 1);
	const auto failed = space.run(executable);
	testing::clear_allocation_failure();
	require(failed.event == EngineEvent::continued && failed.retired == 1,
		"a failed registration ran more than its one instruction");
	require(space.diagnostics().unregistered_transients ==
		before.unregistered_transients + 1,
		"a failed registration was not counted");
	require(!held_at(holders_of(backing, 0), space.generation(), executable),
		"a failed registration left a holder");
	/* The next dispatch registers and runs the whole block. */
	const auto settled = space.run_to_system_call(executable);
	require(settled.rax == 5);
	require(held_at(holders_of(backing, 0), space.generation(), executable));
	require(space.diagnostics().unheld_runnable == 0);
	require(space.diagnostics().walked_refusals == 0);
}

} // namespace

int main()
{
	const auto start = FrameAccounting::diagnostics();
	test_store();
	std::puts("store ok");
	test_running_block();
	std::puts("running ok");
	test_running_push();
	std::puts("push ok");
	test_flush_serial();
	std::puts("serial ok");
	test_registration_failure();
	std::puts("transient ok");
	const auto now = FrameAccounting::diagnostics();
	require(now.holder_leaks == start.holder_leaks, "a holder leaked");
	require(now.unquiesced_holder_removals == start.unquiesced_holder_removals);
	require(now.unmarked_holder_removals == start.unmarked_holder_removals);
	require(now.walked_refusals == start.walked_refusals);
	require(now.errors == start.errors, "an accounting error was counted");
	std::puts("x86-64 gadget alias tests passed");
	return EXIT_SUCCESS;
}
