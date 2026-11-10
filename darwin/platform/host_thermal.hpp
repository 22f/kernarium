// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>

namespace uml::platform {

/* The system's thermal state, as NSProcessInfo names it. */
enum class HostThermalLevel : std::uint8_t {
	nominal,
	fair,
	serious,
	critical,
};

struct HostThermalState final {
	HostThermalLevel level{HostThermalLevel::nominal};
	bool lowPowerMode{};
	/* The processor cores the system runs now; fewer than the device has
	 * while it holds some back. 0 where unknown. */
	std::uint8_t activeProcessors{};

	friend bool operator==(const HostThermalState &,
		const HostThermalState &) = default;
};

/* What the host says of its heat and power state. Reading it is cheap and
 * any thread may; nothing is cached, so a reading is the system's now. */
class HostThermalProvider {
public:
	virtual ~HostThermalProvider() = default;

	[[nodiscard]] virtual HostThermalState state() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<HostThermalProvider>
makeAppleHostThermalProvider();

} // namespace uml::platform
