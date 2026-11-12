// SPDX-License-Identifier: Apache-2.0
//
// How a capture request of the camera HAL ends. The HAL collects the output
// buffers of a request one after another against one deadline, and the
// framework learns of the end by the camera HAL's notify contract: a request
// whose outputs all failed is an ERROR_REQUEST (the application gets
// onCaptureFailed and may shoot again), while ERROR_BUFFER, which Camera2 hands
// to the application as onCaptureBufferLost and CameraX ignores, belongs to a
// request that lost only some of its outputs. The tests hold both rules where
// no Android library is needed.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace andarium::camera {

// What the device is given for the buffers of a request. A frame is due at once;
// a request with a still waits for the device to finish its capture (the device
// gives a started still ten seconds) and for the rest of the group's pictures
// (two seconds more), so the HAL's limit is the device's own and some more.
constexpr std::int64_t frame_timeout_ns = 3'000'000'000;
constexpr std::int64_t still_timeout_ns = 15'000'000'000;

// What the framework is told when the HAL has collected a request.
enum class RequestEnd {
    // Every output is filled: the shutter, the results and the buffers.
    complete,
    // Some outputs failed: the shutter, the results and the buffers as above,
    // and ERROR_BUFFER for each failed one. The filled outputs are real frames,
    // such as a recording's that a refused snapshot must not interrupt.
    buffer_errors,
    // No output could be made: ERROR_REQUEST, no shutter and no results, and
    // the buffers go back marked as failed (with no ERROR_BUFFER of their own).
    request_error,
};

[[nodiscard]] constexpr RequestEnd request_end(std::size_t outputs, std::size_t failed) noexcept
{
    if (outputs == 0 || failed == 0) return RequestEnd::complete;
    return failed >= outputs ? RequestEnd::request_error : RequestEnd::buffer_errors;
}

// The one deadline of a request. Every buffer waits for what is left of it, so
// a request of any number of outputs ends within one limit, and a wait that
// starts after the deadline ends at once.
class RequestClock {
public:
    void begin(std::int64_t now_ns, bool with_still) noexcept
    {
        deadline_ns_ = now_ns + (with_still ? still_timeout_ns : frame_timeout_ns);
    }

    [[nodiscard]] std::int64_t remaining(std::int64_t now_ns) const noexcept
    {
        return std::max<std::int64_t>(deadline_ns_ - now_ns, 0);
    }

private:
    std::int64_t deadline_ns_ = 0;
};

} // namespace andarium::camera
