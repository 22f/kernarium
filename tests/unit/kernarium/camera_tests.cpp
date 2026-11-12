// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumCamera.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace kernarium::camera;
using uml::camera::CameraMode;
using uml::camera::CameraState;
using uml::camera::CameraTier;
using uml::testing::require;

kernarium_runtime_boot fullBoot()
{
    kernarium_runtime_boot boot {};
    boot.struct_size = sizeof(boot);
    return boot;
}

void aRequestThatStatesNothingIsBlocked()
{
    auto boot = fullBoot();
    auto request = readRequest(boot);
    require(request.mode == CameraMode::blocked);
    require(request.calibrationDirectory.empty());
    require(!request.unknownMode && !request.invalidCalibrationPath);

    // An application built before the fields states nothing, whatever follows
    // its struct in memory.
    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE;
    static const char directory[] = "/calibration";
    boot.android_camera_calibration_path = directory;
    boot.struct_size = bootSizeBeforeMode;
    request = readRequest(boot);
    require(request.mode == CameraMode::blocked, "the mode is read only if the struct covers it");
    require(request.calibrationDirectory.empty(), "and so is the path");
    boot.struct_size = bootSizeBeforeCalibrationPath;
    request = readRequest(boot);
    require(request.mode == CameraMode::live, "the mode alone");
    require(request.calibrationDirectory.empty());
    boot.struct_size = bootSizeBeforeMode + sizeof(boot.android_camera_mode) - 1;
    require(readRequest(boot).mode == CameraMode::blocked, "half a mode is none");
}

void theRequestStatesTheModeAndThePath()
{
    auto boot = fullBoot();
    static const char directory[] = "/calibration";
    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE;
    boot.android_camera_calibration_path = directory;
    auto request = readRequest(boot);
    require(request.mode == CameraMode::live);
    require(request.calibrationDirectory == directory);
    require(!request.unknownMode && !request.invalidCalibrationPath);

    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED;
    require(readRequest(boot).mode == CameraMode::blocked);
}

void aCameraNeverFailsABoot()
{
    auto boot = fullBoot();
    boot.android_camera_mode = 7;
    static const char empty[] = "";
    boot.android_camera_calibration_path = empty;
    const auto request = readRequest(boot);
    require(request.mode == CameraMode::blocked, "an unknown mode is blocked, the safe one");
    require(request.unknownMode);
    require(request.invalidCalibrationPath && request.calibrationDirectory.empty(),
        "an empty path keeps the colour data in memory");

    const std::string tooLong(PATH_MAX + 1, 'a');
    boot.android_camera_calibration_path = tooLong.c_str();
    require(readRequest(boot).invalidCalibrationPath);
}

void theEnumsAreTheApplicationsWords()
{
    require(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED == 0 && KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE == 1);
    require(KERNARIUM_RUNTIME_CAMERA_STATE_IDLE == 0);
    // Every device state has a distinct runtime value, and the states the
    // application tells apart are not merged.
    std::vector<int> seen;
    for (const auto state :
         {CameraState::idle, CameraState::streaming, CameraState::interrupted,
          CameraState::interruptedByMultitasking, CameraState::interruptedBySystemPressure}) {
        const int value = stateValue(state);
        for (const int earlier : seen) require(earlier != value, "distinct");
        seen.push_back(value);
    }
    require(stateValue(CameraState::streaming) == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING);
    require(stateValue(CameraState::interruptedByMultitasking) ==
        KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING);
    require(stateValue(CameraState::interruptedBySystemPressure) ==
        KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE);
}

void aHostWithoutCamerasPresentsNone()
{
    const auto none = present(nullptr, CameraTier::large, "");
    require(none.layout == "none" && !none.hasCameras() && none.source == nullptr);
    require(none.calibration != nullptr);
}

void theLayoutFollowsTheCamerasAndTheTier()
{
    const auto phone = [](CameraTier tier) {
        return present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), tier, "");
    };
    const auto large = phone(CameraTier::large);
    require(large.layout == "bf-large-flash-manual-concurrent-front2160", large.layout);
    require(large.hasCameras() && large.source != nullptr && large.tier == CameraTier::large);
    // The layout is discovery alone: no session opened.
    require(phone(CameraTier::small).layout == "bf-small-flash-manual");

    const auto laptop = present(
        uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::laptop), CameraTier::large, "");
    require(laptop.layout == "front", laptop.layout);
    require(laptop.hasCameras());
    // The vendor SKU names a layout of the image's closed set.
    const auto names = uml::camera::allLayoutNames();
    for (const auto* layout : {&large.layout, &laptop.layout})
        require(std::find(names.begin(), names.end(), *layout) != names.end(), *layout);
}

void theLensCoverFollowsTheSession()
{
    require(lensCovered(CameraMode::blocked, true), "a blocked session with cameras");
    require(!lensCovered(CameraMode::live, true), "a live one");
    require(!lensCovered(CameraMode::blocked, false), "a host without cameras shows no blocked camera");
    require(!lensCovered(CameraMode::live, false));
}

struct Reports {
    std::vector<int> states;
    static void callback(void* context, int state) { static_cast<Reports*>(context)->states.push_back(state); }
};

void theStateReceiverReportsChanges()
{
    StateReceiver receiver;
    Reports reports;
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_IDLE);
    receiver.publish(CameraState::streaming);
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING, "kept without a receiver");

    receiver.set(Reports::callback, &reports);
    require(reports.states == std::vector<int> {KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING},
        "a new receiver hears the state in force at once");
    receiver.publish(CameraState::streaming);
    require(reports.states.size() == 1, "the same state again is not a change");
    receiver.publish(CameraState::interruptedByMultitasking);
    receiver.publish(CameraState::streaming);
    receiver.publish(CameraState::idle);
    require((reports.states == std::vector<int> {
                KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING,
                KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING,
                KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING,
                KERNARIUM_RUNTIME_CAMERA_STATE_IDLE}),
        "in the order they happened");

    receiver.publish(CameraState::interrupted);
    receiver.ended();
    require(reports.states.back() == KERNARIUM_RUNTIME_CAMERA_STATE_IDLE && receiver.current() == 0,
        "a guest that ends leaves no camera open");
    const auto count = reports.states.size();
    receiver.ended();
    require(reports.states.size() == count, "and ending twice says it once");

    receiver.set(nullptr, nullptr);
    receiver.publish(CameraState::streaming);
    require(reports.states.size() == count, "a cleared receiver is not called");
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING);
}

void theRotationTakesQuarterTurns()
{
    Rotation rotation;
    require(rotation.degrees() == 0);
    for (const std::uint32_t degrees : {90u, 180u, 270u, 0u}) {
        require(rotation.set(degrees));
        require(rotation.degrees() == degrees);
    }
    require(rotation.set(90));
    for (const std::uint32_t bad : {1u, 45u, 91u, 360u, 450u, 0xFFFFFFFFu})
        require(!rotation.set(bad) && rotation.degrees() == 90, "another value changes nothing");
}

void theDeviceOptionsCarryTheSessionsStatements()
{
    const auto presentation =
        present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), CameraTier::large, "");
    StateReceiver receiver;
    Rotation rotation;
    Reports reports;
    receiver.set(Reports::callback, &reports);

    const auto options = deviceOptions(presentation, CameraMode::blocked, receiver, rotation);
    require(options.mode == CameraMode::blocked && options.tier == CameraTier::large);
    require(options.calibration == presentation.calibration, "the one store of the run");
    require(options.automaticRotation && options.onState);
    require(options.automaticRotation() == 0);
    require(rotation.set(270) && options.automaticRotation() == 270, "the rotation is read when it is asked");
    options.onState(CameraState::interruptedByMultitasking);
    require(reports.states.back() == KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING);
    require(deviceOptions(presentation, CameraMode::live, receiver, rotation).mode == CameraMode::live);
}

#ifndef UML_APP_STORE_RUNTIME
void theEnvironmentNamesTheSyntheticCamera()
{
    require(std::string {sourceVariable} == "KERNARIUM_CAMERA_SOURCE");
    for (const char* host : {static_cast<const char*>(nullptr), "", "host"}) {
        const auto choice = parseSource(host);
        require(!choice.synthetic && !choice.invalid);
    }
    using uml::camera::SyntheticProfile;
    struct Case {
        const char* value;
        SyntheticProfile profile;
    };
    for (const Case& named : {Case {"synthetic", SyntheticProfile::phone}, Case {"synthetic:phone", SyntheticProfile::phone},
                              Case {"synthetic:tablet", SyntheticProfile::tablet},
                              Case {"synthetic:laptop", SyntheticProfile::laptop}}) {
        const auto choice = parseSource(named.value);
        require(choice.synthetic && !choice.invalid && choice.profile == named.profile, named.value);
    }
    for (const char* bad : {"synthetic:watch", "webcam", "Synthetic", "synthetic:"}) {
        const auto choice = parseSource(bad);
        require(!choice.synthetic && choice.invalid, bad);
    }
}
#endif

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"a-request-that-states-nothing-is-blocked", aRequestThatStatesNothingIsBlocked},
        {"the-request-states-the-mode-and-the-path", theRequestStatesTheModeAndThePath},
        {"a-camera-never-fails-a-boot", aCameraNeverFailsABoot},
        {"the-enums-are-the-applications-words", theEnumsAreTheApplicationsWords},
        {"a-host-without-cameras-presents-none", aHostWithoutCamerasPresentsNone},
        {"the-layout-follows-the-cameras-and-the-tier", theLayoutFollowsTheCamerasAndTheTier},
        {"the-lens-cover-follows-the-session", theLensCoverFollowsTheSession},
        {"the-state-receiver-reports-changes", theStateReceiverReportsChanges},
        {"the-rotation-takes-quarter-turns", theRotationTakesQuarterTurns},
        {"the-device-options-carry-the-sessions-statements", theDeviceOptionsCarryTheSessionsStatements},
#ifndef UML_APP_STORE_RUNTIME
        {"the-environment-names-the-synthetic-camera", theEnvironmentNamesTheSyntheticCamera},
#endif
    };
    return uml::testing::runNamedTests(tests);
}
