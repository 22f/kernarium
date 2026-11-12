// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_network.h"
#include "address_source.hpp"
#include "dns_resolver.hpp"
#include "host_addresses.hpp"
#include "network_counters.hpp"
#include "platform/descriptor.hpp"

#include <libslirp.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef UML_SLIRP_TESTING
#define UML_SLIRP_TESTING 0
#endif

namespace uml::network {

inline constexpr std::size_t frame_size = DARWIN_UML_NET_FRAME_SIZE;
inline constexpr std::size_t ring_size = 256;
/* How long a guest transmit waits for the event loop to free a ring slot
 * before the frame is refused (see SlirpProvider::write). The loop frees one
 * within its next pass; the bound only stops a wedged loop holding a vCPU, so
 * it must stay well above any scheduling delay a loaded host imposes. */
inline constexpr std::chrono::milliseconds transmit_space_wait{500};

/* The user-mode network the guest sees. `guest_address` is the single DHCP
 * lease, so it is also the default target of a host forward. */
inline constexpr const char *network_address = "10.0.2.0";
inline constexpr const char *network_mask = "255.255.255.0";
inline constexpr const char *host_alias_address = "10.0.2.2";
inline constexpr const char *guest_address = "10.0.2.15";
inline constexpr const char *nameserver_address = "10.0.2.3";
inline constexpr dns::Ipv4Address nameserver_ipv4{10, 0, 2, 3};
inline constexpr dns::Ipv4Address host_alias_ipv4{10, 0, 2, 2};
/* The addresses SLiRP answers for itself; none of them is the host. */
inline constexpr dns::Subnet virtual_network_ipv4{
	{10, 0, 2, 0}, {255, 255, 255, 0}};
/* While nothing reports a change of the host's addresses, the provider reads
 * them again after this long. */
inline constexpr std::chrono::seconds address_poll_interval{2};

/* One host listening socket forwarded into the guest. Registrations are
 * accepted while the provider is not open; the provider owns replaying them
 * onto each SLiRP instance it creates, so a stop/reopen keeps the same
 * forwards. */
struct HostForward final {
	bool udp{};
	std::string host_address{"127.0.0.1"};
	std::uint16_t host_port{};
	std::string guest_address{uml::network::guest_address};
	std::uint16_t guest_port{};
};

/* Parse "[PROTO:][HOSTADDR:]HOSTPORT-[GUESTADDR:]GUESTPORT". PROTO is tcp or
 * udp and defaults to tcp, HOSTADDR defaults to loopback so a forward is not
 * reachable off the machine unless it says so, and GUESTADDR defaults to the
 * DHCP lease. Returns false on any malformed field. */
[[nodiscard]] bool parse_host_forward(
	std::string_view specification, HostForward &forward) noexcept;

struct Frame final {
	std::size_t length{};
	std::array<std::byte, frame_size> data{};
};

struct Timer final {
	SlirpTimerId id{};
	void *callback_opaque{};
	std::int64_t expires_ms{-1};
};

/* The descriptors one event-loop pass polls: the wake pipe first, then one
 * per socket SLiRP asks for, however many flows the guest has open. The
 * vector is kept across passes, so a steady flow count allocates nothing. */
struct PollState final {
	std::vector<pollfd> descriptors;
};

struct SlirpDeleter final {
	void operator()(Slirp *slirp) const noexcept;
};

class SlirpProvider final {
public:
	SlirpProvider() = default;

	SlirpProvider(const SlirpProvider &) = delete;
	SlirpProvider &operator=(const SlirpProvider &) = delete;
	~SlirpProvider();

    int open(std::span<unsigned char, 6> mac, unsigned int &mtu);
	/* Register a host forward. It is recorded and installed on each SLiRP
	 * instance open() creates, before that instance's worker starts: libslirp
	 * takes no lock, so nothing may join an instance its worker is walking.
	 * Returns 0, -EINVAL for a zero port, or -EBUSY while the provider is
	 * open. A host port that cannot be claimed is reported at open(). */
	int add_forward(const HostForward &forward);
	/* The active-session IRQ descriptor is borrowed from the provider and keeps
	 * the same numeric identity across stop/reopen. It closes only when the
	 * provider itself is destroyed, after users have released registrations. */
    [[nodiscard]] int irq_fd() const;
    int read(std::span<std::byte> buffer);
    int write(std::span<const std::byte> frame);
	void stop() noexcept;
	/* What the provider has counted since it was made, and across the
	 * links it brings up; any thread, and it takes no lock. */
	[[nodiscard]] NetworkCounts counters() const noexcept
	{
		return counters_.read();
	}

#if UML_SLIRP_TESTING
	/* A guest forward for a test: a connection the guest opens to
	 * `guest_address:guest_port` goes to the Unix socket `path`, where the
	 * test listens. It stands in for the host listener the guest can no
	 * longer reach through the gateway, which the product has no way to
	 * forward to. Applies to the next open(). The environment variable
	 * UML_TEST_SLIRP_GUEST_FORWARD ("ADDRESS:PORT=PATH", more than one
	 * separated by commas) adds the same forwards, for a test launcher. */
	struct GuestForward final {
		std::string guest_address{host_alias_address};
		std::uint16_t guest_port{};
		std::string path;
	};
	void add_guest_forward_for_testing(GuestForward forward)
	{
		guest_forwards_for_testing_.push_back(std::move(forward));
	}
	/* The host's addresses come from `reader` instead of the system. */
	void set_host_address_reader_for_testing(
		std::function<std::optional<HostAddresses>()> reader)
	{
		address_reader_for_testing_ = std::move(reader);
	}
	/* Acts as the routing socket's report of a change. */
	void notify_host_addresses_changed_for_testing() noexcept;
	/* Times the provider has read the host's addresses. */
	[[nodiscard]] std::uint64_t host_address_reads_for_testing() const noexcept
	{
		return host_address_reads_for_testing_.load(
			std::memory_order_acquire);
	}
	/* The next open() leaves the routing socket closed, as a system that
	 * refuses it does, and reads the addresses every `interval`. */
	void use_address_timer_for_testing(
		std::chrono::milliseconds interval) noexcept
	{
		address_timer_for_testing_ = interval;
	}
	/* Whether a routing socket reports the host's address changes. */
	[[nodiscard]] bool address_source_active_for_testing() const noexcept
	{
		return address_source_.active();
	}
	void pause_transmit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_transmit_pause_for_testing() noexcept;
	void resume_transmit_for_testing() noexcept;
	[[nodiscard]] std::size_t transmit_space_waiters_for_testing() noexcept;
	void pause_next_write_before_commit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_write_pause_for_testing() noexcept;
	void release_write_for_testing() noexcept;
	void pause_next_read_before_notification_for_testing() noexcept;
	[[nodiscard]] bool wait_for_read_pause_for_testing() noexcept;
	void release_read_for_testing() noexcept;
	[[nodiscard]] bool wait_for_receive_cleanup_for_testing() noexcept;
	[[nodiscard]] slirp_ssize_t enqueue_received_for_testing(
		std::span<const std::byte> frame) noexcept;
	[[nodiscard]] bool readiness_channels_drained_for_testing() const noexcept;
	[[nodiscard]] std::uint64_t processed_transmits_for_testing() const noexcept
	{
		return processed_transmits_for_testing_.load(
			std::memory_order_acquire);
	}
	/* Passes the event loop has made: a loop that never waits on its
	 * sockets goes through them without pause. */
	[[nodiscard]] std::uint64_t event_loop_passes_for_testing() const noexcept
	{
		return event_loop_passes_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Forwards installed while a worker already ran on the instance. */
	[[nodiscard]] std::size_t
	forwards_installed_while_running_for_testing() const noexcept
	{
		return forwards_installed_while_running_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Internet-family host sockets SLiRP has made: a guest's DNS must
	 * never add one. */
	[[nodiscard]] std::size_t internet_sockets_for_testing() const noexcept
	{
		return internet_sockets_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Applies to the next open(). */
	void set_dns_interface_for_testing(std::uint32_t index) noexcept
	{
		dns_interface_for_testing_ = index;
	}
	/* The resolver's wiring checks (dns_resolver.hpp). */
	void inject_dns_submit_error_for_testing(std::int32_t error) noexcept
	{
		resolver_.inject_submit_error_for_testing(error);
	}
	void inject_dns_reply_error_for_testing(std::int32_t error) noexcept
	{
		resolver_.inject_reply_error_for_testing(error);
	}
	void set_dns_deadline_for_testing(
		std::chrono::milliseconds deadline) noexcept
	{
		resolver_.set_deadline_for_testing(deadline);
	}
	void hold_dns_answers_for_testing(bool held) noexcept
	{
		resolver_.hold_answers_for_testing(held);
	}
	[[nodiscard]] std::uint64_t dns_answers_dropped_for_testing() const noexcept
	{
		return resolver_.answers_dropped_for_testing();
	}
	void set_dns_pending_limit_for_testing(std::size_t limit) noexcept
	{
		resolver_.set_pending_limit_for_testing(limit);
	}
	/* The next open() finds no socket path for DNS over TCP. */
	void set_dns_streams_unavailable_for_testing(bool unavailable) noexcept
	{
		dns_streams_unavailable_for_testing_ = unavailable;
	}
	[[nodiscard]] HostResolver &resolver_for_testing() noexcept
	{
		return resolver_;
	}
#endif

private:
	static const SlirpCb &callbacks() noexcept;
	static SlirpProvider *from_opaque(void *opaque) noexcept;
	static slirp_ssize_t send_packet_callback(
		const void *data, std::size_t length, void *opaque) noexcept;
	static void guest_error_callback(const char *message,
					 void *opaque) noexcept;
	static std::int64_t clock_callback(void *opaque) noexcept;
	static void *timer_new_callback(SlirpTimerId id,
					void *callback_opaque,
					void *opaque) noexcept;
	static void timer_free_callback(void *timer, void *opaque) noexcept;
	static void timer_modify_callback(void *timer, std::int64_t expires_ms,
					  void *opaque) noexcept;
	static void notify_callback(void *opaque) noexcept;
	static void register_poll_socket(slirp_os_socket descriptor,
					 void *opaque) noexcept;
	static void unregister_poll_socket(slirp_os_socket descriptor,
					   void *opaque) noexcept;
	static int add_poll_descriptor(slirp_os_socket descriptor, int events,
				       void *opaque) noexcept;
	static int get_poll_events(int index, void *opaque) noexcept;
	/* Called with start_mutex_ held and slirp_ valid. */
	int install_forward(const HostForward &forward) noexcept;
	/* Reads the host's addresses into host_addresses_; false, with the table
	 * as it was, when the system cannot list them. */
	bool refresh_host_addresses() noexcept;
	/* The event loop's part: reads the addresses again when the routing
	 * socket (`readable`) or, without one, the timer says they may have
	 * changed. */
	void service_host_addresses(bool readable) noexcept;
	[[nodiscard]] std::uint32_t address_timeout(
		std::uint32_t timeout) const noexcept;

    slirp_ssize_t enqueue_received(std::span<const std::byte> frame);
	/* Answers a guest frame meant for the name server; false passes it to
	 * SLiRP. */
	bool answer_locally(std::span<const std::byte> frame) noexcept;
	void process_transmit_queue() noexcept;
	[[nodiscard]] bool transmit_permitted() noexcept;
	void wake() noexcept;
#if UML_SLIRP_TESTING
	void pause_write_before_commit_for_testing() noexcept;
	void pause_read_before_notification_for_testing() noexcept;
	void notify_receive_cleanup_for_testing() noexcept;
	void install_guest_forwards_for_testing() noexcept;
#endif
	void event_loop(std::stop_token stop_token) noexcept;
	[[nodiscard]] std::uint32_t timer_timeout(
		std::uint32_t timeout) const noexcept;
	void handle_expired_timers() noexcept;

	mutable std::mutex start_mutex_;
	std::mutex receive_mutex_;
	std::mutex transmit_mutex_;
	/* Signalled when the event loop frees a transmit slot while a writer
	 * waits; guarded by transmit_mutex_. */
	std::condition_variable transmit_space_;
	std::size_t transmit_space_waiters_{};
#if UML_SLIRP_TESTING
	std::mutex transmit_pause_mutex_;
	std::condition_variable transmit_pause_condition_;
	std::mutex io_pause_mutex_;
	std::condition_variable io_pause_condition_;
#endif
	std::array<Frame, ring_size> receive_ring_{};
	std::array<Frame, ring_size> transmit_ring_{};
	std::size_t receive_head_{};
	std::size_t receive_tail_{};
	std::size_t transmit_head_{};
	std::size_t transmit_tail_{};
	std::list<std::unique_ptr<Timer>> timers_;
	std::vector<HostForward> forwards_;
	std::unique_ptr<Slirp, SlirpDeleter> slirp_;
	std::array<uml::platform::UniqueDescriptor, 2> wake_pipe_;
	std::array<uml::platform::UniqueDescriptor, 2> receive_pipe_;
	/* Declared before what counts into them. */
	NetworkCounters counters_;
	TrafficObserver observer_{counters_};
	HostResolver resolver_;
	std::uint16_t next_identification_{1};
	/* Where SLiRP's guest forward carries DNS over TCP to the resolver;
	 * set by open() before the worker exists. */
	bool name_server_stream_{};
	bool gateway_stream_{};
	/* What the guest may not address: open() fills the table before the
	 * worker exists, and the worker alone reads and refreshes it after. */
	HostAddresses host_addresses_;
	AddressChangeSource address_source_;
	/* Without a routing socket, or after a read failed, the time of the next
	 * read. */
	bool address_read_pending_{};
	std::chrono::steady_clock::time_point next_address_read_{};
	std::jthread worker_;
	std::atomic_bool started_{false};
	std::atomic_bool stopping_{true};
#if UML_SLIRP_TESTING
	std::atomic_bool transmit_pause_requested_for_testing_{};
	bool transmit_pause_reached_for_testing_{};
	std::atomic_uint64_t processed_transmits_for_testing_{};
	std::atomic_uint64_t event_loop_passes_for_testing_{};
	std::atomic_size_t forwards_installed_while_running_for_testing_{};
	std::atomic_size_t internet_sockets_for_testing_{};
	std::uint32_t dns_interface_for_testing_{};
	bool dns_streams_unavailable_for_testing_{};
	bool write_pause_requested_for_testing_{};
	bool write_pause_reached_for_testing_{};
	bool release_write_for_testing_{};
	bool read_pause_requested_for_testing_{};
	bool read_pause_reached_for_testing_{};
	bool release_read_for_testing_{};
	bool receive_cleanup_reached_for_testing_{};
	std::vector<GuestForward> guest_forwards_for_testing_;
	std::function<std::optional<HostAddresses>()> address_reader_for_testing_;
	std::atomic_bool address_change_for_testing_{};
	std::atomic_uint64_t host_address_reads_for_testing_{};
	std::chrono::milliseconds address_timer_for_testing_{};
#endif
};

} // namespace uml::network
