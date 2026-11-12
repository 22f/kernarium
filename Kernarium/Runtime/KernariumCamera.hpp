// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "camera/calibration_store.hpp"
#include "camera/camera_device.hpp"
#include "camera/camera_diagnostics.hpp"
#include "camera/camera_layout.hpp"
#include "camera/camera_orientation.hpp"
#include "camera/camera_source.hpp"
#include "camera/camera_tiers.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace kernarium::camera {

/* The cameras an Android guest is given (runtime API 37): what the boot request
 * says about them, the layout composed from the host's cameras, and the two
 * statements the application makes while the guest runs, the camera state it
 * hears and the rotation it gives. darwin/device/camera/README.md owns the
 * device and the layouts. */

/* What a request states before the camera mode and the calibration path were
 * added to it. */
inline constexpr std::size_t bootSizeBeforeMode =
    offsetof(kernarium_runtime_boot, android_camera_mode);
inline constexpr std::size_t bootSizeBeforeCalibrationPath =
    offsetof(kernarium_runtime_boot, android_camera_calibration_path);

/* The camera part of a boot request. */
struct Request {
    uml::camera::CameraMode mode {uml::camera::CameraMode::blocked};
    /* Where the colour data a Bayer still teaches is kept; in memory when
     * empty. */
    std::string calibrationDirectory;
    /* The request stated a mode or a path that is not one: the runtime drops
     * it (a camera never fails a boot) and says so. */
    bool unknownMode {};
    bool invalidCalibrationPath {};
};

/* The camera fields of `boot`, read only as far as its struct_size covers. A
 * request that states nothing runs blocked. */
[[nodiscard]] inline Request readRequest(const kernarium_runtime_boot& boot)
{
    Request request;
    if (boot.struct_size >= bootSizeBeforeMode + sizeof(boot.android_camera_mode)) {
        switch (boot.android_camera_mode) {
        case KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED:
            break;
        case KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE:
            request.mode = uml::camera::CameraMode::live;
            break;
        default:
            request.unknownMode = true;
            break;
        }
    }
    if (boot.struct_size >=
            bootSizeBeforeCalibrationPath + sizeof(boot.android_camera_calibration_path) &&
        boot.android_camera_calibration_path != nullptr) {
        const std::size_t length = ::strnlen(boot.android_camera_calibration_path, PATH_MAX);
        if (length == 0 || length == PATH_MAX)
            request.invalidCalibrationPath = true;
        else
            request.calibrationDirectory.assign(boot.android_camera_calibration_path, length);
    }
    return request;
}

/* The cameras a run presents to its guest, discovered once: the platform
 * camera that serves them, the colour data store, the memory tier and the
 * layout that describes them. Discovery asks the system for no permission and
 * opens no session, so the layout is the same whether the session is live or
 * blocked. */
struct Presentation {
    uml::camera::CameraSourceHandle source;
    std::shared_ptr<uml::camera::CalibrationStore> calibration;
    uml::camera::CameraTier tier {uml::camera::CameraTier::small};
    /* The vendor SKU's value: one of the image's layouts, "none" when the host
     * has no cameras the image can describe. */
    std::string layout {"none"};

    /* Whether the guest gets a camera device at all. */
    [[nodiscard]] bool hasCameras() const noexcept { return layout != "none"; }
};

/* Discovers `source`'s cameras at `tier` and composes the layout. A null
 * source is a host with no camera. */
[[nodiscard]] inline Presentation present(
    uml::camera::CameraSourceHandle source,
    uml::camera::CameraTier tier,
    const std::string& calibrationDirectory)
{
    Presentation presentation;
    presentation.tier = tier;
    presentation.calibration = std::make_shared<uml::camera::CalibrationStore>(
        calibrationDirectory);
    if (source == nullptr)
        return presentation;
    uml::camera::ArrangementOptions arrangement;
    arrangement.tier = tier;
    presentation.layout =
        uml::camera::composeLayout(*source, *presentation.calibration, arrangement);
    presentation.source = std::move(source);
    return presentation;
}

/* Whether the side keys report Android's camera lens cover for a session: a
 * blocked session that has cameras. A host with none reports no cover, so a
 * guest does not show a blocked camera it does not have. */
[[nodiscard]] constexpr bool lensCovered(
    uml::camera::CameraMode mode, bool hasCameras) noexcept
{
    return mode == uml::camera::CameraMode::blocked && hasCameras;
}

/* The runtime's state value of the device's. */
[[nodiscard]] constexpr int stateValue(uml::camera::CameraState state) noexcept
{
    switch (state) {
    case uml::camera::CameraState::idle:
        return KERNARIUM_RUNTIME_CAMERA_STATE_IDLE;
    case uml::camera::CameraState::streaming:
        return KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING;
    case uml::camera::CameraState::interrupted:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED;
    case uml::camera::CameraState::interruptedByMultitasking:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING;
    case uml::camera::CameraState::interruptedBySystemPressure:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE;
    }
    return KERNARIUM_RUNTIME_CAMERA_STATE_IDLE;
}

/* The one receiver of the camera state. The callback runs with the mutex held,
 * which is what makes clearing it wait for a call in progress; it reports a
 * state when it changes, and the state in force to a new callback at once. */
class StateReceiver final {
public:
    void set(kernarium_runtime_camera_state_callback callback, void* context)
    {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        context_ = context;
        if (callback != nullptr)
            callback(context, state_);
    }

    /* The device's state, on its transport thread. */
    void publish(uml::camera::CameraState state)
    {
        publishValue(stateValue(state));
    }

    /* The guest ended or restarts: no camera is open any more. */
    void ended() { publishValue(KERNARIUM_RUNTIME_CAMERA_STATE_IDLE); }

    [[nodiscard]] int current()
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

private:
    void publishValue(int state)
    {
        std::lock_guard lock(mutex_);
        if (state == state_)
            return;
        state_ = state;
        if (callback_ != nullptr)
            callback_(context_, state);
    }

    std::mutex mutex_;
    kernarium_runtime_camera_state_callback callback_ {};
    void* context_ {};
    int state_ {KERNARIUM_RUNTIME_CAMERA_STATE_IDLE};
};

/* The turn of the device body the application states: the clockwise degrees
 * from portrait, as its user sees it, where the host owns the guest's rotation
 * (an iPad window: the guest stays upright in the window while the device,
 * which carries every built-in sensor, turns under it); 0 where the guest owns
 * the rotation, which leaves every camera as the platform states it. A boot
 * reads it once, to derive each camera's SENSOR_ORIENTATION; a change after
 * the boot reaches the cameras as a turn of the picture from that figure
 * (camera_orientation.hpp). */
class Rotation final {
public:
    /* 0, 90, 180 or 270; false for another value, which changes nothing. */
    bool set(std::uint32_t degrees) noexcept
    {
        if (degrees >= 360 || degrees % 90 != 0)
            return false;
        degrees_.store(degrees, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] std::uint32_t degrees() const noexcept
    {
        return degrees_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> degrees_ {0};
};

/* The device's options for a boot of a session of `mode`. The receiver and the
 * rotation outlive every device. The turn the application has stated now is the
 * boot's: the guest's SENSOR_ORIENTATIONs follow it, and the picture turns by
 * the change since. */
[[nodiscard]] inline uml::camera::CameraDeviceOptions deviceOptions(
    const Presentation& presentation,
    uml::camera::CameraMode mode,
    StateReceiver& receiver,
    const Rotation& rotation)
{
    uml::camera::CameraDeviceOptions options;
    options.mode = mode;
    options.tier = presentation.tier;
    options.calibration = presentation.calibration;
    const auto bootTurn = rotation.degrees();
    options.sensorOrientation = [bootTurn](const uml::platform::CameraInfo& info) {
        return uml::camera::turnedSensorOrientation(
            info.position, info.sensorOrientation, bootTurn);
    };
    options.automaticRotation = [&rotation, bootTurn](const uml::platform::CameraInfo& info) {
        return uml::camera::pictureTurn(info.position, bootTurn, rotation.degrees());
    };
    options.onState = [&receiver](uml::camera::CameraState state) {
        receiver.publish(state);
    };
#ifndef UML_APP_STORE_RUNTIME
    /* What reaches the host and what it costs, for a development run to read. */
    options.onRotateAndCrop = uml::camera::printRotateAndCrop;
    options.onCheckpoint = uml::camera::printCheckpoint;
#endif
    return options;
}

#ifndef UML_APP_STORE_RUNTIME
/* Which camera a runtime that is not an App Store build serves, from the
 * environment: the host's (the default), or the synthetic one that tests and
 * the Simulator use, which is no camera of the machine's and never captures
 * one. An App Store runtime reads no such knob and has no synthetic camera. */
inline constexpr const char* sourceVariable = "KERNARIUM_CAMERA_SOURCE";

struct SourceChoice {
    bool synthetic {};
    uml::camera::SyntheticProfile profile {uml::camera::SyntheticProfile::phone};
    /* The value was not one of the names: the host's camera is used. */
    bool invalid {};
};

/* "host" or nothing, "synthetic" (the phone) or "synthetic:" with phone,
 * tablet or laptop. */
[[nodiscard]] inline SourceChoice parseSource(const char* value)
{
    SourceChoice choice;
    if (value == nullptr || *value == '\0' || std::string_view {value} == "host")
        return choice;
    const std::string_view text {value};
    if (text == "synthetic" || text == "synthetic:phone") {
        choice.synthetic = true;
    } else if (text == "synthetic:tablet") {
        choice.synthetic = true;
        choice.profile = uml::camera::SyntheticProfile::tablet;
    } else if (text == "synthetic:laptop") {
        choice.synthetic = true;
        choice.profile = uml::camera::SyntheticProfile::laptop;
    } else {
        choice.invalid = true;
    }
    return choice;
}
#endif

} // namespace kernarium::camera
