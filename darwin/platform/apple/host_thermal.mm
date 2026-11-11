// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_thermal.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>

namespace uml::platform {
namespace {

class AppleHostThermalProvider final : public HostThermalProvider {
public:
	HostThermalState state() noexcept override
	{
		@autoreleasepool {
			NSProcessInfo *info = NSProcessInfo.processInfo;
			HostThermalState result;
			switch (info.thermalState) {
			case NSProcessInfoThermalStateNominal:
				result.level = HostThermalLevel::nominal;
				break;
			case NSProcessInfoThermalStateFair:
				result.level = HostThermalLevel::fair;
				break;
			case NSProcessInfoThermalStateSerious:
				result.level = HostThermalLevel::serious;
				break;
			case NSProcessInfoThermalStateCritical:
				result.level = HostThermalLevel::critical;
				break;
			}
			result.lowPowerMode = info.lowPowerModeEnabled ? true : false;
			result.activeProcessors = static_cast<std::uint8_t>(std::min<NSUInteger>(
				info.activeProcessorCount, 255));
			return result;
		}
	}
};

} // namespace

std::unique_ptr<HostThermalProvider> makeAppleHostThermalProvider()
{
	return std::make_unique<AppleHostThermalProvider>();
}

} // namespace uml::platform
