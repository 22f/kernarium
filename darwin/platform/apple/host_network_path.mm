// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/host_network_path.hpp"
#include "host_network_path_status.hpp"

#import <Network/Network.h>

#include <pthread.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace uml::platform {

bool hostNetworkPathIsOutage(nw_path_status_t status,
			     nw_path_unsatisfied_reason_t reason) noexcept
{
	return status == nw_path_status_unsatisfied &&
		reason != nw_path_unsatisfied_reason_local_network_denied;
}

namespace {

/* What the monitor's thread and its updates share. It outlives the monitor
 * that made it, so that an update in flight when the monitor ends, and the
 * thread on its way out, find a lock and no state, not freed memory. */
struct Shared final {
	std::mutex lock;
	std::condition_variable wake;
	HostNetworkPathState *state{};
	bool ended{};

	void deliver(bool usable) noexcept
	{
		std::lock_guard guard{lock};
		if (state != nullptr)
			state->report(usable);
	}
};

/*
 * The first Network.framework monitor of a process starts the framework up,
 * and the thread that makes it keeps a Mach port of its own (a send and
 * receive right) until that thread ends. A guest's restart ends the thread
 * that booted it, so a monitor made on that thread leaves the process one port
 * short of its first boot's, which the restart audit (guest-restart) reports.
 * The monitor is therefore made on a thread of its own, which lives as long as
 * the monitor does and waits without waking for anything.
 */
void run(std::shared_ptr<Shared> shared) noexcept
{
	(void)::pthread_setname_np("uml-network-path");
	nw_path_monitor_t monitor = nw_path_monitor_create();
	if (monitor != nil) {
		nw_path_monitor_set_queue(monitor, dispatch_queue_create(
			"uml.network-path",
			dispatch_queue_attr_make_with_qos_class(
				DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)));
		/* On the system's queue: a block that does no more than write the
		 * state. */
		nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
			shared->deliver(!hostNetworkPathIsOutage(
				nw_path_get_status(path),
				nw_path_get_unsatisfied_reason(path)));
		});
		nw_path_monitor_start(monitor);
	}
	{
		std::unique_lock guard{shared->lock};
		shared->wake.wait(guard, [&] { return shared->ended; });
	}
	if (monitor != nil)
		nw_path_monitor_cancel(monitor);
}

class AppleHostNetworkPathMonitor final : public HostNetworkPathMonitor {
public:
	explicit AppleHostNetworkPathMonitor(HostNetworkPathState &state)
	{
		shared_->state = &state;
		/* The thread holds the shared part alone: it may outlive this
		 * object by the time it takes to cancel the monitor. */
		const std::shared_ptr<Shared> shared = shared_;
		std::thread{[shared] { run(shared); }}.detach();
	}

	~AppleHostNetworkPathMonitor() override
	{
		{
			std::lock_guard guard{shared_->lock};
			shared_->state = nullptr;
			shared_->ended = true;
		}
		shared_->wake.notify_all();
	}

private:
	std::shared_ptr<Shared> shared_{std::make_shared<Shared>()};
};

} // namespace

std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state)
{
	return std::unique_ptr<HostNetworkPathMonitor>{
		new (std::nothrow) AppleHostNetworkPathMonitor(state)};
}

} // namespace uml::platform
