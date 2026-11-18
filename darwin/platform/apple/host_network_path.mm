// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/host_network_path.hpp"

#import <Network/Network.h>

#include <memory>
#include <mutex>
#include <new>

namespace uml::platform {
namespace {

/* A path with nothing to use. One that needs a connection first is not one:
 * the attempt itself brings it up. A denial of the local network alone leaves
 * every other address reachable. */
bool outage(nw_path_t path) noexcept
{
	return nw_path_get_status(path) == nw_path_status_unsatisfied &&
		nw_path_get_unsatisfied_reason(path) !=
			nw_path_unsatisfied_reason_local_network_denied;
}

/* What an update writes into. It outlives the monitor that made it, so that
 * an update in flight when the monitor ends finds a lock and no state, not
 * freed memory. */
struct Sink final {
	std::mutex lock;
	HostNetworkPathState *state{};

	void deliver(bool usable) noexcept
	{
		std::lock_guard guard{lock};
		if (state != nullptr)
			state->report(usable);
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
			sink->deliver(!outage(path));
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
