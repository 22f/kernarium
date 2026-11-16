// SPDX-License-Identifier: GPL-2.0-or-later
/* What StaticGuestMemory reports as the stamp of a code page: the identity,
 * the version, the epoch, and whether the page may be shared -- the kernel's
 * immutable fact for the mapping the page is fetched through, on a record
 * that still counts exactly. The stamp the translation caches keep stays
 * the two words it was. */
#include "memory/static_guest_memory.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <sys/mman.h>

namespace {

using uml::memory::CodePageStamp;
using uml::memory::ExtendedCodePageStamp;
using uml::memory::StaticGuestMemory;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr std::string_view backing_name = "uml-static-guest-stamp-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;

void map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, unsigned long offset, int protection, int code)
{
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(), offset,
		page, protection, code, 1) == 0);
}

[[nodiscard]] bool stamp_of(StaticGuestMemory &memory, unsigned long address,
	ExtendedCodePageStamp &stamp)
{
	return memory.extended_code_page_stamp(address, nullptr, stamp);
}

void test_the_stamp_says_whether_the_page_may_be_shared()
{
	constexpr unsigned long generation = 601;
	constexpr unsigned long base = 0x1000'0000;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, base, backing, 0, rwx, 3);
	map(generation, base + page, backing, 0, rwx, 1);
	map(generation, base + 2 * page, backing, page, PROT_READ | PROT_EXEC, 3);
	/* Immutable, but not a page a translator can hold. */
	map(generation, base + 3 * page, backing, 2 * page,
		PROT_READ | PROT_WRITE, 3);
	require(darwin_uml_static_vm_activate(generation) == 0);

	StaticGuestMemory memory;
	memory.set_generation(generation);
	std::array<ExtendedCodePageStamp, 3> stamps{};
	for (std::size_t index = 0; index < stamps.size(); ++index)
		require(stamp_of(memory, base + index * page, stamps[index]));
	require(stamps[0].shareable && !stamps[1].shareable && stamps[2].shareable);

	/* The two mappings of one frame name the same page at the same version
	 * and the same epoch; the fact is the mapping's. */
	require(stamps[0].stamp == stamps[1].stamp &&
		stamps[0].epoch == stamps[1].epoch);
	require(!(stamps[0].stamp == stamps[2].stamp));

	/* The stamp the translation caches keep is the same two words. */
	CodePageStamp plain{};
	require(memory.code_page_stamp(base, nullptr, plain));
	require(plain == stamps[0].stamp);
	require(sizeof(CodePageStamp) == 2 * sizeof(std::uint64_t));

	ExtendedCodePageStamp unused{};
	require(!stamp_of(memory, base + 3 * page, unused));

	/* A publication moves the version and the epoch; the stamp taken again
	 * through the cached entry still carries the mapping's fact. */
	void *handle = nullptr;
	darwin_uml_static_code_page_stamp raw{};
	require(darwin_uml_static_vm_code_page(generation, base, &handle, &raw) ==
		DARWIN_UML_STATIC_TRANSLATED);
	darwin_uml_static_vm_invalidate_code_page(handle);
	ExtendedCodePageStamp after{};
	require(stamp_of(memory, base, after));
	require(after.stamp.identity == stamps[0].stamp.identity &&
		after.stamp.version == stamps[0].stamp.version + 1 &&
		after.epoch == stamps[0].epoch + 1 && after.shareable);
	ExtendedCodePageStamp alias{};
	require(stamp_of(memory, base + page, alias));
	require(alias.stamp == after.stamp && alias.epoch == after.epoch &&
		!alias.shareable);

	/* Flipping the fact is a mapping change, which the executor consumes
	 * before it asks again. */
	map(generation, base + page, backing, 0, rwx, 3);
	memory.set_generation(generation);
	require(stamp_of(memory, base + page, alias));
	require(alias.shareable);
	map(generation, base, backing, 0, rwx, 1);
	memory.set_generation(generation);
	require(stamp_of(memory, base, after));
	require(!after.shareable);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	test_the_stamp_says_whether_the_page_may_be_shared();
	std::puts("Static guest memory stamp tests passed");
	return 0;
}
