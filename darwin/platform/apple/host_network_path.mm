// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/host_network_path.hpp"
#include "host_network_path_status.hpp"

#import <Network/Network.h>

#include <memory>
#include <mutex>
#include <new>

namespace uml::platform {

bool hostNetworkPathIsOutage(nw_path_status_t status,
			     nw_path_unsatisfied_reason_t reason) noexcept
{
	return status == nw_path_status_unsatisfied &&
		reason != nw_path_unsatisfied_reason_local_network_denied;
}

bool hostNetworkPathIsCarried(nw_path_status_t status) noexcept
{
	return status == nw_path_status_satisfied;
}

namespace {

/* What an update writes into. It outlives the monitor that made it, so that
 * an update in flight when the monitor ends finds a lock and no state, not
 * freed memory. */
struct Sink final {
	std::mutex lock;
	HostNetworkPathState *state{};

	void deliver(bool usable, bool carried) noexcept
	{
		std::lock_guard guard{lock};
		if (state != nullptr)
			state->report(usable, carried);
	}
};

class AppleHostNetworkPathMonitor final : public HostNetworkPathMonitor {
public:
	explicit AppleHostNetworkPathMonitor(HostNetworkPathState &state)
	{
		sink_->state = &state;
		monitor_ = nw_path_monitor_create();
		if (monitor_ == nil)
			return;
		nw_path_monitor_set_queue(monitor_, dispatch_queue_create(
			"uml.network-path",
			dispatch_queue_attr_make_with_qos_class(
				DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)));
		/* On the system's queue: a block that does no more than write the
		 * state. */
		const std::shared_ptr<Sink> sink = sink_;
		nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
			sink->deliver(!hostNetworkPathIsOutage(
					      nw_path_get_status(path),
					      nw_path_get_unsatisfied_reason(path)),
				      hostNetworkPathIsCarried(
					      nw_path_get_status(path)));
		});
		nw_path_monitor_start(monitor_);
	}

	~AppleHostNetworkPathMonitor() override
	{
		{
			std::lock_guard guard{sink_->lock};
			sink_->state = nullptr;
		}
		if (monitor_ != nil)
			nw_path_monitor_cancel(monitor_);
	}

private:
	std::shared_ptr<Sink> sink_{std::make_shared<Sink>()};
	nw_path_monitor_t monitor_{};
};

} // namespace

std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state)
{
	return std::unique_ptr<HostNetworkPathMonitor>{
		new (std::nothrow) AppleHostNetworkPathMonitor(state)};
}

} // namespace uml::platform
