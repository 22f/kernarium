// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::memory {

enum class MemoryAccess {
	read,
	write,
	execute,
};

struct MemoryFault final {
	std::uint64_t address{};
	MemoryAccess access{MemoryAccess::read};
};

/* A stable name for the bytes of a code page: the same page at the same
 * version. The translation caches rely on it, and it is part of the layout of
 * what they keep, so it stays two words. */
struct CodePageStamp final {
	std::uint64_t identity{};
	std::uint64_t version{};

	friend bool operator==(const CodePageStamp &, const CodePageStamp &) =
		default;
};

/* What a code page says about itself, for a consumer that keeps verdicts
 * about the page's bytes across address spaces. `epoch` moves at a
 * publication and when the last executable mapping of the page departs, and
 * nowhere else. `shareable` is true when the mapping the stamp was taken
 * through carries the kernel's immutable fact and the page's record counts its
 * executable mappings exactly. A verdict is kept only for a shareable page,
 * under (identity, epoch). */
struct ExtendedCodePageStamp final {
	CodePageStamp stamp{};
	std::uint64_t epoch{};
	bool shareable{};

	friend bool operator==(const ExtendedCodePageStamp &,
			       const ExtendedCodePageStamp &) = default;
};

class GuestAddressSpace {
public:
	virtual ~GuestAddressSpace() = default;
	[[nodiscard]] virtual bool read(std::uint64_t address,
					std::span<std::byte> destination) = 0;
	[[nodiscard]] virtual bool write(std::uint64_t address,
					 std::span<const std::byte> source) = 0;
	/* Validate an access without changing guest memory. Emulators with
	 * protection-aware mappings override this so instructions that validate
	 * a whole transfer before writing preserve architectural fault
	 * priority. */
	[[nodiscard]] virtual bool validate_access(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return true;
	}
	[[nodiscard]] virtual bool fetch(std::uint64_t address,
					 std::span<std::byte> destination)
	{
		return read(address, destination);
	}
	[[nodiscard]] virtual std::byte *atomic_pointer(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return nullptr;
	}
	[[nodiscard]] virtual std::byte *reservation_pointer(
		std::uint64_t address, std::size_t size, MemoryAccess access)
	{
		return atomic_pointer(address, size, access);
	}
	[[nodiscard]] virtual bool is_executable(std::uint64_t) noexcept
	{
		return false;
	}
	/* A stable stamp lets a translated-code cache reject unrelated global code
	 * publications without rereading its source bytes. Implementations which
	 * cannot retain page identity return false and keep exact byte validation.
	 */
	[[nodiscard]] virtual bool code_page_stamp(
		std::uint64_t, void *, CodePageStamp &) noexcept
	{
		return false;
	}
	/* The same page's stamp with its epoch and whether it may be shared. */
	[[nodiscard]] virtual bool extended_code_page_stamp(
		std::uint64_t, void *, ExtendedCodePageStamp &) noexcept
	{
		return false;
	}
	virtual void invalidate_code(std::uint64_t) {}
	/* A translated executor may carry an already-entered memory execution
	 * context. Static memory uses it to publish directly to the owning
	 * generation; other memory implementations retain the ordinary hook.
	 */
	virtual void invalidate_code(std::uint64_t address, void *)
	{
		invalidate_code(address);
	}
	[[nodiscard]] virtual MemoryFault last_fault() const noexcept
	{
		return {};
	}
	[[nodiscard]] virtual bool has_fault() const noexcept
	{
		return false;
	}
};

} // namespace uml::memory
