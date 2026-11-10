// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "calibration_store.hpp"
#include "camera.hpp"
#include "camera_description.hpp"
#include "camera_tiers.hpp"
#include "vhost_user_device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace uml::camera {

/* The Virtio camera: the video device type VIRTIO_VIDEO_DEVICE_CAMERA, under
 * UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID. */
inline constexpr std::uint32_t cameraDeviceId = 65533;

/* How a session treats the host's cameras, fixed when the guest starts.
 *
 * Live opens a platform capture session when an application in the guest
 * streams, and closes it when none does.
 *
 * Blocked never opens one, so the host's capture indicator cannot light: the
 * device publishes the cameras that discovery found, and every video buffer
 * completes black and flagged blocked. */
enum class CameraMode : std::uint8_t { live, blocked };

/* What the host is doing for the guest, for the application's notice. */
enum class CameraState : std::uint8_t {
    idle,
    streaming,
    /* The capture cannot run for a reason that ends by itself. */
    interrupted,
    /* The same, because other applications share the screen. */
    interruptedByMultitasking,
};

struct CameraDeviceOptions {
    CameraMode mode {CameraMode::live};
    CameraTier tier {CameraTier::small};
    /* Where learned colour data is kept; null keeps it in memory. */
    std::shared_ptr<CalibrationStore> calibration;
    /* The application's statement of a camera's sensor orientation; the
     * platform's where empty. */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> sensorOrientation;
    /* The clockwise rotation, in degrees, that ROTATE_AND_CROP's automatic
     * setting applies; none where empty. */
    std::function<std::uint32_t()> automaticRotation;
    /* Called on the transport thread when the state changes. */
    std::function<void(CameraState)> onState;
    /* The clock frame ages are measured on; the platform's where empty. */
    std::function<std::uint64_t()> clock;
};

/* The camera device. Every method runs on the transport worker, which is also
 * the only thread that touches guest memory; the platform camera calls the
 * device back on its own threads and the device hands those calls to the worker
 * through its event descriptor. */
class VirtioCameraDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioCameraDevice(std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options);
    ~VirtioCameraDevice() override;
    const char* name() const noexcept override;
    std::uint64_t features() const noexcept override;
    std::size_t queueCount() const noexcept override;
    std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override;
    void setGuestMapper(GuestMapper mapper) override;
    void setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions) override;
    uml::virtio::VhostUserQueueResult processQueue(std::uint32_t index, std::uint64_t requestId,
                                                   std::span<const std::uint8_t> request,
                                                   std::size_t responseCapacity) override;
    bool takeQueueCompletion(uml::virtio::VhostUserQueueCompletion&) override;
    void reset() noexcept override;
    int eventDescriptor() const noexcept override;
    void drainEventSource() override;

    /* The cameras the guest sees, in wire order. Call it on the transport
     * worker, or while the device is not serving a guest. */
    [[nodiscard]] std::vector<CameraDescription> cameras() const;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::camera
