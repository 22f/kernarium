// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace uml::runtime {

/* Formats into a fixed buffer; what does not fit is dropped. */
class FixedText final {
public:
	FixedText(char *data, std::size_t size) noexcept : data_{data}, size_{size} {}

	FixedText &text(std::string_view value) noexcept
	{
		const std::size_t count = std::min(value.size(), size_ - used_);
		std::memcpy(data_ + used_, value.data(), count);
		used_ += count;
		return *this;
	}

	FixedText &number(std::int64_t value) noexcept
	{
		char digits[24];
		const auto result =
			std::to_chars(digits, digits + sizeof(digits), value);
		return text({digits, static_cast<std::size_t>(result.ptr - digits)});
	}

	/* `value` with every byte that is not printable ASCII, or a line feed
	 * where `lines` allows one, replaced; carriage returns are dropped. */
	FixedText &printable(std::string_view value, bool lines) noexcept
	{
		for (const char character : value) {
			if (used_ == size_)
				break;
			if (character == '\r')
				continue;
			const bool keep = (character >= ' ' && character < 0x7f) ||
				(lines && (character == '\n' || character == '\t'));
			data_[used_++] = keep ? character : '?';
		}
		return *this;
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		return used_;
	}

private:
	char *data_;
	std::size_t size_;
	std::size_t used_{};
};

} // namespace uml::runtime
