// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace andarium::motion {
constexpr uint32_t port = 5556;
constexpr size_t record_size = 20;
// Android's sensor type of the device-orientation sensor (SensorType
// DEVICE_ORIENTATION), whose record the host sends once the guest has said it
// has the sensor (control frame "ANC2"). The record carries the orientation in
// the first value: 0 the default portrait, 1 the device turned 90 degrees
// counter-clockwise, 2 turned 180 degrees, 3 turned 90 degrees clockwise, which
// are Surface.ROTATION_* and the numbers of the sensor's own events. The other
// two values are zero.
constexpr uint32_t orientation_type = 27;
constexpr int orientation_count = 4;
// The launch property that makes the guest list the sensor: the host's
// "androidboot.kernarium.orientation=1" kernel argument. Without it the sub-HAL
// lists the accelerometer and the gyroscope alone and sends "ANC1" frames, as
// an image without the sensor does.
constexpr const char* orientation_property = "ro.boot.kernarium.orientation";
struct Sample {
    uint32_t type;
    std::array<float, 3> values;
};
inline std::optional<Sample> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() != record_size || bytes[0] != 'A' || bytes[1] != 'N' ||
        bytes[2] != 'S' || bytes[3] != '1') return std::nullopt;
    const auto word = [&bytes](size_t offset) {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    Sample sample{word(4), {}};
    if (sample.type != 1 && sample.type != 4 && sample.type != orientation_type) return std::nullopt;
    for (size_t i = 0; i < sample.values.size(); ++i) {
        sample.values[i] = std::bit_cast<float>(word(8 + i * 4));
        if (!std::isfinite(sample.values[i])) return std::nullopt;
    }
    if (sample.type == orientation_type &&
        (sample.values[0] < 0 || sample.values[0] >= orientation_count ||
         sample.values[0] != std::floor(sample.values[0]) || sample.values[1] != 0 ||
         sample.values[2] != 0)) {
        return std::nullopt;
    }
    return sample;
}

// Guest-to-host control frame, sent on the same connection when the host
// connects and at every change of what applications use. 16 bytes, little
// endian: "ANC1", a uint32 mask of the active sensors (bit 0 accelerometer,
// bit 1 gyroscope), then the uint32 sampling period in microseconds of the
// accelerometer and of the gyroscope. The host reads a sensor only while it is
// active; a host that has not seen a frame reads both continuously.
//
// An image that has the device-orientation sensor sends "ANC2" instead: the
// same layout, with bit 2 of the mask for that sensor (it has no period). The
// frame also tells the host that the guest accepts orientation records, which
// an image without the sensor would take for a bad record and answer by ending
// the connection, so a host sends one only after an "ANC2" frame.
constexpr size_t control_size = 16;
constexpr int64_t minimum_period_ns = 10'000'000;
constexpr int64_t maximum_period_ns = 1'000'000'000;
// `orientation` is empty for a guest without the sensor, which sends "ANC1",
// and otherwise whether the sensor is active, which sends "ANC2".
inline std::array<uint8_t, control_size> encode_control(
        const std::array<bool, 2>& active, const std::array<int64_t, 2>& period_ns,
        std::optional<bool> orientation = std::nullopt) {
    std::array<uint8_t, control_size> frame{'A', 'N', 'C', uint8_t(orientation ? '2' : '1')};
    const auto put = [&frame](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) frame[offset + i] = uint8_t(value >> (8 * i));
    };
    put(4, (active[0] ? 1u : 0u) | (active[1] ? 2u : 0u) | (orientation.value_or(false) ? 4u : 0u));
    for (size_t i = 0; i < period_ns.size(); ++i) {
        const auto period = std::clamp(period_ns[i], minimum_period_ns, maximum_period_ns);
        put(8 + i * 4, uint32_t(period / 1000));
    }
    return frame;
}
} // namespace andarium::motion
