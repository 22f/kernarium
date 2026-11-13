// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::runtime {

namespace detail {

/* A marker as a Knuth-Morris-Pratt automaton: a byte moves the match to
 * the longest prefix of the marker that ends at it. */
struct Marker final {
	std::string_view text;
	std::array<std::uint8_t, 64> fallback{};

	explicit constexpr Marker(std::string_view marker) : text{marker}
	{
		unsigned matched = 0;
		for (std::size_t index = 1; index < text.size(); ++index) {
			while (matched > 0 && text[matched] != text[index])
				matched = fallback[matched - 1];
			if (text[matched] == text[index])
				++matched;
			fallback[index] = static_cast<std::uint8_t>(matched);
		}
	}

	/* The match after `byte`, from `matched` characters of the marker;
	 * equal to the marker's length when `byte` completes it. */
	[[nodiscard]] constexpr unsigned advance(unsigned matched,
		char byte) const noexcept
	{
		while (matched > 0 && text[matched] != byte)
			matched = fallback[matched - 1];
		if (text[matched] == byte)
			++matched;
		return matched;
	}
};

} // namespace detail

/*
 * Whether the console of an Android guest has said that its boot finished.
 *
 * Android's init logs `processing action (sys.boot_completed=1)` when the
 * system is up, and `starting service 'recovery'` when the recovery image is.
 * Both are the moments Andarium's boot screen ends at, so they are what the
 * host calls a completed boot (ConsoleScanner in AndariumCore reads the same
 * two lines). A Linux desktop guest's console says nothing of the kind.
 *
 * The signal reads the bytes the kernel writes to its console, as they pass
 * through kernel_write (session_record.hpp), so a marker that a write splits
 * is found all the same. It takes no lock and allocates nothing: a signal
 * handler or a second console thread may feed it, and a chunk that races
 * another is scanned again after it, so no chunk is lost. Once the boot has
 * completed it reads nothing further.
 */
class ConsoleBootSignal final {
public:
	/* The text of the two lines, which a line holds anywhere. */
	static constexpr std::string_view boot_completed_marker{
		"processing action (sys.boot_completed=1)"};
	static constexpr std::string_view recovery_marker{
		"init: starting service 'recovery'"};

	/* A new boot begins: nothing has been said. */
	void reset() noexcept { state_.store(0, std::memory_order_release); }

	/* The kernel wrote `length` bytes to a console. */
	void feed(const void *data, std::size_t length) noexcept
	{
		if (data == nullptr || length == 0)
			return;
		const auto *bytes = static_cast<const unsigned char *>(data);
		std::uint32_t state = state_.load(std::memory_order_acquire);
		for (;;) {
			if ((state & completed_bit) != 0)
				return;
			const std::uint32_t next = scan(state, bytes, length);
			if (next == state)
				return;
			if (state_.compare_exchange_weak(state, next,
				    std::memory_order_acq_rel,
				    std::memory_order_acquire))
				return;
		}
	}

	/* Whether either line has been written since the last reset. */
	[[nodiscard]] bool completed() const noexcept
	{
		return (state_.load(std::memory_order_acquire) & completed_bit) != 0;
	}

private:
	static constexpr detail::Marker completed_line{boot_completed_marker};
	static constexpr detail::Marker recovery_line{recovery_marker};
	static_assert(boot_completed_marker.size() < 64 &&
		recovery_marker.size() < 64, "a marker's progress outgrew its byte");

	/* The state: how much of each marker matches at the end of what was read
	 * (a byte each), and whether either completed. */
	static constexpr std::uint32_t completed_bit = 1U << 31;
	static constexpr unsigned recovery_shift = 8;

	[[nodiscard]] static std::uint32_t scan(std::uint32_t state,
		const unsigned char *bytes, std::size_t length) noexcept
	{
		unsigned boot = state & 0xffU;
		unsigned recovery = (state >> recovery_shift) & 0xffU;
		for (std::size_t index = 0; index < length; ++index) {
			const char byte = static_cast<char>(bytes[index]);
			boot = completed_line.advance(boot, byte);
			recovery = recovery_line.advance(recovery, byte);
			if (boot == completed_line.text.size() ||
			    recovery == recovery_line.text.size())
				return completed_bit;
		}
		return boot | (recovery << recovery_shift);
	}

	std::atomic<std::uint32_t> state_{0};
};

} // namespace uml::runtime
