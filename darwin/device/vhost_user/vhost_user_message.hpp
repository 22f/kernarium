// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_protocol.hpp"
#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace uml::gpu {

inline constexpr std::size_t maximumVhostUserMessagePayload =
    1024U * 1024U;
// Linux vhost-user memory tables may contain up to 64 regions. Keep the wire
// receiver's descriptor capacity aligned with that protocol limit so a valid
// table cannot be rejected solely by the ancillary-data buffer size.
inline constexpr std::size_t maximumVhostUserDescriptors = 64;

class ReceivedDescriptors final {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] UniqueDescriptor& operator[](std::size_t index) noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] const UniqueDescriptor& operator[](
        std::size_t index) const noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] bool adopt(UniqueDescriptor descriptor) noexcept
    {
        if (count_ == descriptors_.size())
            return false;
        descriptors_[count_++] = std::move(descriptor);
        return true;
    }

private:
    std::array<UniqueDescriptor, maximumVhostUserDescriptors> descriptors_;
    std::size_t count_ {};
};

struct VhostUserMessage final {
    protocol::VhostHeader header {};
    std::vector<std::uint8_t> payload;
    ReceivedDescriptors descriptors;
};

// This process's open-descriptor ceiling, as the kernel applies it.
struct DescriptorCeiling final {
    std::uint64_t soft {};
    std::uint64_t hard {};
    /* The kernel's own per-process cap. However high the soft limit is set, no
     * descriptor is ever allocated past this; zero when the host will not say.
     */
    std::uint64_t perProcess {};

    // The number that actually decides how many descriptors this process gets.
    [[nodiscard]] std::uint64_t effective() const noexcept
    {
        if (perProcess != 0 && perProcess < soft)
            return perProcess;
        return soft;
    }
};

[[nodiscard]] DescriptorCeiling descriptorCeiling() noexcept;

// One line naming that ceiling, for a startup note or a failure.
[[nodiscard]] std::string describeDescriptorCeiling();

/* Raise the soft open-descriptor ceiling as far as the host permits.
 *
 * A vhost-user backend that publishes a shared-memory region hands the guest
 * one descriptor for every host-visible blob it maps, and the guest holds each
 * one for as long as the mapping lives. The transport is an in-process
 * socketpair, so those descriptors and the backend's own share a single table.
 * Darwin refuses to install an arriving SCM_RIGHTS once that table is full and
 * reports the refusal as EMSGSIZE on recvmsg, which ends the connection and
 * takes the guest's GPU with it. Nothing else in this process raises the
 * ceiling, and an iOS application starts with a small one.
 *
 * Returns the soft ceiling in force afterwards.
 */
std::uint64_t raiseDescriptorCeiling() noexcept;

/* Whether `error`, from a write to a socket of a vhost-user connection, shows
 * that the front-end closed its end: EPIPE, ECONNRESET when it left data
 * unread, or ENOTCONN when the write lands between the kernel's disconnect of
 * the socket and its shutdown for sending. */
[[nodiscard]] bool writeFailureMeansPeerClosed(int error) noexcept;

/* A wait that stayed unanswered for longer than the time it was given:
 * thrown by receiveVhostUserMessage() for a frame that stayed incomplete, and
 * by the server for a backend request its front-end left unacknowledged. */
class VhostUserTimeout final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/* How long the rest of a frame may take once its first byte has arrived. The
 * time starts at that byte, so a peer with nothing to say is not timed; it is
 * read from `now`, so that one clock serves a caller and the receive it makes
 * (a null `now` reads std::chrono::steady_clock). */
struct FrameBound final {
    std::chrono::steady_clock::duration allowance {};
    std::chrono::steady_clock::time_point (*now)() noexcept {};
};

/* Returns false only for an orderly peer close or a truncated stream. A frame
 * that stays incomplete beyond `bound` throws VhostUserTimeout; without a
 * bound the receive waits for the rest of a frame as long as the peer leaves
 * the socket open and nobody asks it to stop. */
[[nodiscard]] bool receiveVhostUserMessage(
    int socket,
    VhostUserMessage& message,
    const std::atomic_bool* stopRequested = nullptr,
    const FrameBound* bound = nullptr);

} // namespace uml::gpu
