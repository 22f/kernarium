// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_device.hpp"

#include "camera_controls.hpp"
#include "guest_buffers.hpp"
#include "jpeg_gap.hpp"
#include "nv12_transform.hpp"
#include "raw_pitch.hpp"
#include "unique_descriptor.hpp"

#include <sys/event.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace uml::camera {
namespace {
using uml::platform::CameraControlState;
using uml::platform::CameraEvent;
using uml::platform::CameraEventKind;
using uml::platform::CameraExposure;
using uml::platform::CameraFrame;
using uml::platform::CameraSettings;
using uml::platform::CameraStillRequest;
using uml::platform::CameraStillResult;
using uml::video::GuestPlanes;
using uml::video::rawPitch;
using uml::virtio::toBytes;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;

constexpr std::size_t maximumStreams = 32;
constexpr std::size_t maximumHeld = 128;
constexpr std::size_t maximumControls = 256;
constexpr std::size_t metaBacklogLimit = 16;
constexpr std::size_t inboxFrameLimit = 3;
/* A group of still buffers whose last buffer has not arrived this long after
 * its first completes with an error. */
constexpr std::uint64_t groupTimeoutNanoseconds = 2'000'000'000;
/* A capture that has started has this long to deliver before its buffers
 * complete with an error: a long exposure, a flash and a 48 MP capture need
 * more than the assembly of a group does. */
constexpr std::uint64_t captureTimeoutNanoseconds = 10'000'000'000;
/* How long a session that failed to open waits before it is tried again. */
constexpr std::uint64_t sessionRetryNanoseconds = 1'000'000'000;
constexpr std::uint32_t userIdent = 0;
constexpr std::uint32_t groupTimerIdent = 0xffffff00u;
constexpr std::uint32_t reservedStreamIds = 0xffff0000u;
constexpr std::uint32_t defaultVideoWidth = 1280;
constexpr std::uint32_t defaultVideoHeight = 720;

/* The values Android and V4L2 give the states the platform reports. */
constexpr std::uint32_t colorspaceRec709 = 3;
constexpr std::uint32_t xferFunc709 = 1;
constexpr std::uint32_t ycbcrEnc709 = 2;
constexpr std::uint32_t quantizationFullRange = 1;

template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short camera command");
    return uml::virtio::fromBytes<T>(bytes);
}

std::vector<std::uint8_t> reply(std::uint32_t stream, std::uint32_t type = VIRTIO_VIDEO_RESP_OK_NODATA)
{
    return toBytes(virtio_video_cmd_hdr {type, stream});
}

std::uint32_t q16(double value) noexcept
{
    return std::uint32_t(std::int32_t(std::lround(value * 65536.0)));
}

std::uint32_t floatBits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

std::uint32_t aeState(CameraControlState state) noexcept
{
    switch (state) {
    case CameraControlState::searching: return 1;
    case CameraControlState::converged: return 2;
    case CameraControlState::locked: return 3;
    default: return 0;
    }
}

std::uint32_t awbState(CameraControlState state) noexcept { return aeState(state); }

std::uint32_t afState(CameraControlState state, bool continuous) noexcept
{
    switch (state) {
    case CameraControlState::searching: return continuous ? 1 : 3;
    case CameraControlState::converged: return continuous ? 2 : 4;
    case CameraControlState::locked: return 4;
    case CameraControlState::failed: return continuous ? 6 : 5;
    default: return 0;
    }
}

} // namespace

class VirtioCameraDevice::Implementation final : public uml::platform::CameraSessionDelegate {
public:
    struct Resource {
        GuestPlanes planes;
        std::uint64_t capacity {};
        bool queued {};
    };
    /* A buffer the guest queued. */
    struct Held {
        std::uint64_t request {};
        std::uint32_t resource {};
        std::uint64_t cookie {};  // the guest's timestamp
        std::uint32_t flags {};
    };
    struct MetaEntry {
        virtio_video_camera_meta meta {};
        std::uint64_t timestamp {};  // of the exposure, on the device's clock; zero: now
    };
    struct Stream {
        std::uint32_t id {};
        std::uint32_t camera {};
        std::uint32_t kind {};
        std::uint32_t format {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t intervalNumerator {1};
        std::uint32_t intervalDenominator {30};
        std::unordered_map<std::uint32_t, Resource> resources;
        std::deque<Held> queued;
        std::deque<MetaEntry> backlog;  // meta streams
        bool started {};
        bool timerArmed {};
        std::uint64_t lastDelivered {};
        bool delivered {};
        std::vector<std::uint8_t> black;
        std::uint64_t framesOut {};
        std::uint64_t framesDropped {};
    };
    struct Member {
        std::uint32_t stream {};
        Held held;
    };
    struct Group {
        std::uint32_t camera {};
        std::uint64_t cookie {};
        std::vector<Member> members;
        std::uint64_t deadline {};
        std::uint64_t token {};
        bool fired {};
    };
    enum class Interruption : std::uint8_t { none, interrupted, multitasking, pressure };
    struct Runtime {
        Runtime(CameraDescription d, bool blocked, std::uint32_t orientation)
            : description(std::move(d)), controls(description.info, orientation, blocked), values(controls.defaults())
        {
        }
        CameraDescription description;
        CameraControls controls;
        ControlValues values;
        CameraSettings applied;
        bool appliedValid {};
        std::uint64_t sequence {};
        std::optional<CameraExposure> latest;
        Interruption interruption {Interruption::none};
        /* The capture is starting and has not delivered a frame yet. */
        bool resuming {};
        bool videoRunning {};
        bool failed {};
        /* The guest has been told the camera is interrupted (or gone) and not
         * yet that it is back; it is told once each way. */
        bool told {};
        bool toldUnavailable {};
        std::uint64_t videoRetry {};
        std::uint32_t runWidth {};
        std::uint32_t runHeight {};
        std::uint32_t runRate {};
        std::uint32_t calibrationAttempts {};
        /* The ROTATE_AND_CROP setting and the turn last reported to
         * `onRotateAndCrop`. */
        std::uint32_t reportedSetting {~0u};
        std::uint32_t reportedTurn {~0u};
    };
    struct InboxFrame {
        std::size_t camera {};
        CameraFrame frame;
    };
    struct InboxStill {
        std::size_t camera {};
        CameraStillResult result;
    };

    std::shared_ptr<uml::platform::Camera> host;
    CameraDeviceOptions options;
    std::shared_ptr<CalibrationStore> calibration;
    std::string model;
    CameraTierLimits limits;
    std::vector<Runtime> runtimes;
    std::unordered_map<std::size_t, std::uint32_t> wireOfPlatform;
    GuestMapper mapper;
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions;
    std::unordered_map<std::uint32_t, Stream> streams;
    std::deque<virtio_video_event> events;
    std::deque<VhostUserQueueCompletion> completions;
    std::vector<Group> groups;
    std::uint64_t nextToken {1};
    uml::gpu::UniqueDescriptor queue;
    std::vector<std::uint8_t> staging;
    std::unique_ptr<uml::platform::Nv12Transformer> transformer;
    CameraState reported {CameraState::idle};
    std::uint64_t lastRetry {};
    bool sessionFailed {};

    // The platform's side: any thread, under the mutex.
    std::mutex inboxMutex;
    std::deque<InboxFrame> inboxFrames;
    std::deque<InboxStill> inboxStills;
    std::deque<CameraEvent> inboxEvents;
    // Last: it is destroyed first, and no callback outlives it.
    std::unique_ptr<uml::platform::CameraSession> session;

    Implementation(std::shared_ptr<uml::platform::Camera> platform, CameraDeviceOptions opts)
        : host(std::move(platform)), options(std::move(opts)), limits(limitsOf(options.tier)), queue(::kqueue())
    {
        if (!host) throw std::invalid_argument("a platform camera is required");
        if (queue.get() < 0) throw std::runtime_error("Virtio camera event queue failed");
        calibration = options.calibration ? options.calibration : std::make_shared<CalibrationStore>();
        model = host->hostModel();
        transformer = host->makeFrameTransformer();
        if (!transformer) throw std::invalid_argument("the platform camera has no frame transformer");
        ArrangementOptions arrangement;
        arrangement.tier = options.tier;
        arrangement.sensorOrientation = options.sensorOrientation;
        const auto infos = host->cameras();
        const auto sets = host->concurrentSets();
        auto cameras = arrangeCameras(infos, sets, model, *calibration, arrangement);
        const bool blocked = options.mode == CameraMode::blocked;
        runtimes.reserve(cameras.size());
        for (auto& camera : cameras) {
            wireOfPlatform[camera.platformIndex] = camera.index;
            const auto orientation = camera.sensorOrientation;
            runtimes.emplace_back(std::move(camera), blocked, orientation);
        }
        struct kevent change {};
        EV_SET(&change, userIdent, EVFILT_USER, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) < 0) throw std::runtime_error("Virtio camera wake-up failed");
    }

    ~Implementation() override { closeSession(); }

    // ------------------------------------------------------------------------
    // Time and bookkeeping
    // ------------------------------------------------------------------------

    [[nodiscard]] std::uint64_t now() const { return options.clock ? options.clock() : host->now(); }
    [[nodiscard]] bool blocked() const noexcept { return options.mode == CameraMode::blocked; }

    [[nodiscard]] std::vector<std::uint8_t> queued(std::uint32_t stream, std::uint64_t timestamp, std::uint32_t flags,
                                                   std::uint32_t size = 0, std::uint64_t sequence = 0) const
    {
        virtio_video_camera_queue_resp response {};
        response.base.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, stream};
        response.base.timestamp = timestamp;
        response.base.flags = flags;
        response.base.data_sizes[0] = size;
        response.sequence = sequence;
        return toBytes(response);
    }

    void complete(std::uint32_t stream, const Held& held, std::uint64_t timestamp, std::uint32_t flags,
                  std::uint32_t size = 0, std::uint64_t sequence = 0)
    {
        auto found = streams.find(stream);
        if (found != streams.end()) {
            const auto resource = found->second.resources.find(held.resource);
            if (resource != found->second.resources.end()) resource->second.queued = false;
        }
        completions.push_back({held.request, queued(stream, timestamp, flags, size, sequence)});
    }

    void event(std::uint32_t camera, std::uint32_t type)
    {
        /* Called from the wake-up path too, where a throw would leave the
         * transport. A full queue already holds unread events, so the new one
         * adds nothing the guest has not been told. */
        if (events.size() >= 64) return;
        events.push_back({type, camera});
        /* The server runs the event queue when the device's event descriptor
         * fires, and the guest's buffer, if it has posted one, is filled. */
        wake();
    }

    [[nodiscard]] Runtime* runtimeOf(std::uint32_t camera) noexcept
    {
        return camera < runtimes.size() ? &runtimes[camera] : nullptr;
    }

    [[nodiscard]] bool blackMode(const Runtime& runtime) const
    {
        return blocked() || runtime.failed || runtime.interruption != Interruption::none || runtime.resuming ||
            runtime.controls.testPattern(runtime.values);
    }

    [[nodiscard]] static bool down(const Runtime& runtime) noexcept
    {
        return runtime.interruption != Interruption::none || runtime.failed;
    }

    /* The dequeue flags of a black frame. A capture that is starting is
     * resuming; one that cannot run is interrupted, whether or not it is also
     * starting. */
    [[nodiscard]] std::uint32_t blackFlags(const Runtime& runtime) const
    {
        std::uint32_t flags = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
        if (blocked()) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED;
        if (down(runtime)) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
        else if (runtime.resuming) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING;
        return flags;
    }

    /* The same state in the metadata record of the frame. */
    [[nodiscard]] std::uint32_t blackMetaFlags(const Runtime& runtime) const
    {
        std::uint32_t flags = VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK;
        if (blocked()) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED;
        if (down(runtime)) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_INTERRUPTED;
        else if (runtime.resuming) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_RESUMING;
        return flags;
    }

    /* The camera cannot run, for a reason that ends by itself. The guest is
     * told once each way: a second interruption, or a capture that fails to
     * start again and again, says nothing new until the camera has been up.
     * Interrupted and unavailable are two states to the guest, so a camera
     * that goes from one to the other is told. */
    void markDown(Runtime& runtime, std::uint32_t eventType)
    {
        runtime.resuming = false;
        const bool unavailable = eventType == VIRTIO_VIDEO_EVENT_CAMERA_UNAVAILABLE;
        if (runtime.told && runtime.toldUnavailable == unavailable) return;
        runtime.told = true;
        runtime.toldUnavailable = unavailable;
        event(runtime.description.index, eventType);
    }

    /* A camera that was told to be down and is up tells the guest so, once. */
    void settle(Runtime& runtime)
    {
        if (!runtime.told || down(runtime)) return;
        runtime.told = false;
        event(runtime.description.index, runtime.toldUnavailable ? VIRTIO_VIDEO_EVENT_CAMERA_AVAILABLE
                                                                 : VIRTIO_VIDEO_EVENT_CAMERA_RESUMED);
    }

    /* The interruptions belong to the session they happened in: when it ends,
     * so do they. */
    void endInterruptions()
    {
        for (auto& runtime : runtimes) {
            runtime.interruption = Interruption::none;
            runtime.resuming = false;
            // A stream that still waits for frames has no capture now: its
            // camera is down until the capture is started again.
            runtime.failed = wantsVideo(runtime);
            settle(runtime);
            updateBlackTimers(runtime);
        }
    }

    [[nodiscard]] bool wantsVideo(const Runtime& runtime) const
    {
        for (const auto& [id, stream] : streams) {
            (void)id;
            if (stream.camera == runtime.description.index && startedVideo(stream)) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // The platform session
    // ------------------------------------------------------------------------

    bool ensureSession()
    {
        if (blocked()) return false;
        if (session) return true;
        const auto time = now();
        if (sessionFailed && time - lastRetry < sessionRetryNanoseconds) return false;
        lastRetry = time;
        session = host->openSession(*this);
        sessionFailed = session == nullptr;
        if (session) {
            // What the guest may ask, which the platform's formats serve.
            session->setLimits({limits.maximumStillPixels});
            checkpoint(CameraCheckpoint::sessionOpened);
            // A session that opens runs; what interrupted the last one is over.
            // A camera with a stream waiting for frames starts its capture again
            // on its next turn, which is now.
            for (auto& runtime : runtimes) {
                runtime.interruption = Interruption::none;
                runtime.failed = wantsVideo(runtime);
                runtime.appliedValid = false;
                runtime.videoRunning = false;
                runtime.videoRetry = 0;
                settle(runtime);
                updateBlackTimers(runtime);
            }
        }
        return session != nullptr;
    }

    void closeSession() noexcept
    {
        if (!session) return;
        session->stop();
        session.reset();
        for (auto& runtime : runtimes) {
            runtime.videoRunning = false;
            runtime.appliedValid = false;
        }
        {
            std::lock_guard lock(inboxMutex);
            inboxFrames.clear();
            inboxStills.clear();
            inboxEvents.clear();
        }
        checkpoint(CameraCheckpoint::sessionClosed);
    }

    /* A diagnostic reading, never allowed to fail the device (it also runs
     * from the destructor). */
    void checkpoint(CameraCheckpoint where) noexcept
    {
        if (!options.onCheckpoint) return;
        try {
            options.onCheckpoint(where);
        } catch (...) {
        }
    }

    [[nodiscard]] bool sessionNeeded() const
    {
        if (!groups.empty()) return true;
        for (const auto& runtime : runtimes)
            if (runtime.videoRunning || (runtime.appliedValid && runtime.applied.torch)) return true;
        return false;
    }

    void releaseSessionIfIdle()
    {
        if (!session || sessionNeeded()) return;
        closeSession();
        endInterruptions();
    }

    void applySettings(Runtime& runtime, bool force)
    {
        if (!session || blocked()) return;
        const auto settings = runtime.controls.settings(runtime.values);
        if (!force && runtime.appliedValid && settings == runtime.applied) return;
        session->apply(runtime.description.platformIndex, settings);
        runtime.applied = settings;
        runtime.appliedValid = true;
    }

    [[nodiscard]] bool startedVideo(const Stream& stream) const noexcept
    {
        return stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && stream.started;
    }

    /* Brings the platform's video for a camera in line with the streams that
     * are running on it. */
    void reconfigure(Runtime& runtime)
    {
        std::uint32_t width = 0, height = 0, rate = 0;
        for (const auto& [id, stream] : streams) {
            (void)id;
            if (stream.camera != runtime.description.index || !startedVideo(stream)) continue;
            width = std::max(width, stream.width);
            height = std::max(height, stream.height);
            rate = std::max(rate, rateOf(stream));
        }
        if (width == 0) {
            if (runtime.videoRunning && session) session->stopVideo(runtime.description.platformIndex);
            runtime.videoRunning = false;
            runtime.runWidth = runtime.runHeight = runtime.runRate = 0;
            runtime.failed = false;
            runtime.resuming = false;
            settle(runtime);
            return;
        }
        if (blocked()) return;
        if (!ensureSession()) {
            runtime.failed = true;
            markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
            return;
        }
        if (runtime.videoRunning && runtime.runWidth == width && runtime.runHeight == height && runtime.runRate == rate) return;
        const bool wasFailed = runtime.failed;
        const bool ok = session->startVideo(runtime.description.platformIndex, width, height, rate);
        runtime.videoRunning = ok;
        runtime.failed = !ok;
        runtime.runWidth = width;
        runtime.runHeight = height;
        runtime.runRate = rate;
        runtime.videoRetry = now();
        if (ok) {
            // The capture has to deliver before the stream has a picture of its own.
            runtime.resuming = true;
            applySettings(runtime, true);
            // A capture that failed is not told to be back until it delivers
            // (pump): a start that fails again and again would otherwise tell
            // the guest at every try.
            if (!wasFailed) settle(runtime);
        } else {
            markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
        }
    }

    [[nodiscard]] static std::uint32_t rateOf(const Stream& stream) noexcept
    {
        return std::max<std::uint32_t>(1, (stream.intervalDenominator + stream.intervalNumerator / 2) / stream.intervalNumerator);
    }

    void reportState()
    {
        bool active = !groups.empty();
        bool interrupted = false, multitasking = false, pressure = false;
        for (const auto& runtime : runtimes) {
            active |= runtime.videoRunning;
            if (runtime.interruption == Interruption::multitasking) multitasking = true;
            if (runtime.interruption == Interruption::pressure) pressure = true;
            if (runtime.interruption == Interruption::interrupted || runtime.failed) interrupted = true;
        }
        for (const auto& [id, stream] : streams) {
            (void)id;
            active |= startedVideo(stream);
        }
        // A blocked session never captures, so nothing is "streaming" however many
        // black frames the guest's streams get.
        CameraState state = blocked() || !active ? CameraState::idle : multitasking ? CameraState::interruptedByMultitasking
            : pressure ? CameraState::interruptedBySystemPressure
            : interrupted ? CameraState::interrupted : CameraState::streaming;
        if (state == reported) return;
        reported = state;
        if (options.onState) options.onState(state);
    }

    // ------------------------------------------------------------------------
    // The platform's callbacks
    // ------------------------------------------------------------------------

    void wake()
    {
        struct kevent change {};
        EV_SET(&change, userIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
    }

    void cameraFrame(std::size_t camera, CameraFrame frame) override
    {
        {
            std::lock_guard lock(inboxMutex);
            // Frames beyond a few are old by the time the worker takes them,
            // and each holds the capture's buffer: keep the newest.
            std::size_t held = 0;
            for (const auto& entry : inboxFrames) held += entry.camera == camera;
            if (held >= inboxFrameLimit) {
                const auto oldest = std::find_if(inboxFrames.begin(), inboxFrames.end(),
                                                 [camera](const auto& entry) { return entry.camera == camera; });
                inboxFrames.erase(oldest);
            }
            inboxFrames.push_back({camera, std::move(frame)});
        }
        wake();
    }
    void cameraStill(std::size_t camera, CameraStillResult result) override
    {
        {
            std::lock_guard lock(inboxMutex);
            inboxStills.push_back({camera, std::move(result)});
        }
        wake();
    }
    void cameraEvent(CameraEvent platformEvent) override
    {
        {
            std::lock_guard lock(inboxMutex);
            inboxEvents.push_back(platformEvent);
        }
        wake();
    }

    // ------------------------------------------------------------------------
    // Exposures and their metadata
    // ------------------------------------------------------------------------

    [[nodiscard]] virtio_video_camera_meta metaOf(const Runtime& runtime, std::uint64_t sequence, const CameraExposure& exposure,
                                                  std::uint32_t flags) const
    {
        virtio_video_camera_meta meta {};
        meta.sequence = sequence;
        meta.exposure_ns = exposure.exposureNanoseconds;
        meta.frame_duration_ns = exposure.frameDurationNanoseconds;
        meta.iso = exposure.iso;
        meta.flags = flags;
        const bool continuous = runtime.controls.settings(runtime.values).focusMode == uml::platform::CameraFocusMode::continuousAuto;
        meta.ae_state = aeState(exposure.ae);
        meta.af_state = afState(exposure.af, continuous);
        meta.awb_state = awbState(exposure.awb);
        meta.lens_position_q16 = q16(exposure.lensPositionDioptres);
        for (std::size_t index = 0; index < 3; ++index) meta.wb_gain_q16[index] = q16(exposure.whiteBalanceGains[index]);
        meta.color_temperature_k = exposure.colorTemperature;
        meta.tint_q16 = q16(exposure.tint);
        meta.zoom_ratio_q16 = q16(exposure.zoomRatio);
        const auto active = wireOfPlatform.find(exposure.activeCamera);
        meta.active_camera = active != wireOfPlatform.end() ? active->second : runtime.description.index;
        meta.aperture_x100 = std::uint32_t(std::lround(exposure.aperture * 100.0f));
        meta.focal_length_um = exposure.focalLengthMillimetres > 0
            ? std::uint32_t(std::lround(exposure.focalLengthMillimetres * 1000.0f)) : runtime.description.wire.focal_length_um;
        // Android's FLASH_STATE: unavailable, ready, fired.
        meta.flash_state = exposure.flashFired ? 3 : runtime.description.info.flash ? 2 : 0;
        for (std::size_t index = 0; index < 3; ++index) meta.neutral_point_q16[index] = q16(exposure.neutralPoint[index]);
        for (std::size_t index = 0; index < 8; ++index) meta.noise_profile_f32[index] = floatBits(exposure.noiseProfile[index]);
        return meta;
    }

    void pushMeta(Runtime& runtime, const virtio_video_camera_meta& meta, std::uint64_t timestamp)
    {
        for (auto& [id, stream] : streams) {
            if (stream.camera != runtime.description.index || stream.kind != VIRTIO_VIDEO_CAMERA_STREAM_META || !stream.started)
                continue;
            stream.backlog.push_back({meta, timestamp});
            while (stream.backlog.size() > metaBacklogLimit) stream.backlog.pop_front();
            serveMeta(id, stream);
        }
    }

    void serveMeta(std::uint32_t id, Stream& stream)
    {
        while (!stream.queued.empty() && !stream.backlog.empty()) {
            const auto held = stream.queued.front();
            stream.queued.pop_front();
            const auto entry = stream.backlog.front();
            stream.backlog.pop_front();
            auto& resource = stream.resources.at(held.resource);
            auto bytes = toBytes(entry.meta);
            const bool ok = resource.capacity >= bytes.size() && !resource.planes.empty() &&
                transferSegments(mapper, regions.get(), resource.planes[0], bytes, true);
            const auto time = now();
            const auto age = entry.timestamp != 0 && time > entry.timestamp ? time - entry.timestamp : 0;
            complete(id, held, age, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, ok ? std::uint32_t(bytes.size()) : 0,
                     entry.meta.sequence);
        }
    }

    // ------------------------------------------------------------------------
    // Video
    // ------------------------------------------------------------------------

    [[nodiscard]] static std::uint32_t pitchOf(const Stream& stream) noexcept { return rawPitch(stream.width); }
    [[nodiscard]] static std::uint32_t videoPlaneSize(const Stream& stream) noexcept
    {
        return pitchOf(stream) * (stream.height + stream.height / 2);
    }

    /* A picture of the camera's size and orientation goes into the guest's
     * rows as it is, with no copy of its own in between. */
    bool copyRows(const Resource& resource, const Stream& stream, const CameraFrame& frame, uml::platform::Nv12Rect crop)
    {
        const auto pitch = pitchOf(stream);
        uml::video::GuestPlaneWriter writer(mapper, regions.get(), resource.planes[0]);
        for (std::uint32_t row = 0; row < crop.height; ++row) {
            const std::span<const std::uint8_t> data(frame.luma.data() + std::size_t(crop.y + row) * frame.lumaStride + crop.x, crop.width);
            if (!writer.write(std::uint64_t(row) * pitch, data)) return false;
        }
        const std::uint64_t chromaStart = std::uint64_t(pitch) * stream.height;
        for (std::uint32_t row = 0; row < crop.height / 2; ++row) {
            const std::span<const std::uint8_t> data(frame.chroma.data() + std::size_t(crop.y / 2 + row) * frame.chromaStride + crop.x,
                                                     crop.width);
            if (!writer.write(chromaStart + std::uint64_t(row) * pitch, data)) return false;
        }
        return true;
    }

    bool writeVideo(const Stream& stream, Resource& resource, const CameraFrame* frame, std::uint32_t rotation)
    {
        const auto pitch = pitchOf(stream);
        const auto size = videoPlaneSize(stream);
        if (resource.planes.empty() || resource.capacity < size) return false;
        staging.resize(size);
        uml::platform::Nv12Target target {staging.data(), pitch, staging.data() + std::size_t(pitch) * stream.height, pitch,
                                          stream.width, stream.height};
        if (frame == nullptr) {
            uml::platform::fillNv12Black(target);
            return transferSegments(mapper, regions.get(), resource.planes[0], staging, true);
        }
        if (frame->luma.size() < frame->lumaStride * frame->height || frame->chroma.size() < frame->chromaStride * (frame->height / 2))
            return false;
        const uml::platform::Nv12Source source {frame->luma.data(), frame->lumaStride, frame->chroma.data(), frame->chromaStride,
                                                frame->width, frame->height, frame->native};
        const bool quarter = rotation == 90 || rotation == 270;
        const auto crop = uml::platform::centredCrop(frame->width, frame->height, stream.width, stream.height, quarter);
        if (crop.width == 0) return false;
        const bool copy = rotation == 0 && crop.width == stream.width && crop.height == stream.height;
        if (copy) {
            // The picture is the buffer's: the rows go straight in.
            if (!uml::platform::validNv12Transform(source, crop, 0, target)) return false;
            return copyRows(resource, stream, *frame, crop);
        }
        if (!transformer->transform(source, crop, rotation, target)) return false;
        return transferSegments(mapper, regions.get(), resource.planes[0], staging, true);
    }

    [[nodiscard]] std::uint32_t rotationOf(Runtime& runtime) const
    {
        const auto setting = runtime.controls.rotateAndCrop(runtime.values);
        std::uint32_t degrees = 0;
        if (setting >= 1 && setting <= 3)
            degrees = setting * 90;
        else if (setting == 4 && options.automaticRotation)
            degrees = options.automaticRotation(runtime.description.info) % 360 / 90 * 90;
        if (options.onRotateAndCrop && (setting != runtime.reportedSetting || degrees != runtime.reportedTurn)) {
            runtime.reportedSetting = setting;
            runtime.reportedTurn = degrees;
            options.onRotateAndCrop(runtime.description.index, setting, degrees);
        }
        return degrees;
    }

    void deliverFrame(Runtime& runtime, const CameraFrame& frame)
    {
        const auto sequence = ++runtime.sequence;
        runtime.latest = frame.exposure;
        const auto time = now();
        const auto age = time > frame.timestampNanoseconds ? time - frame.timestampNanoseconds : 0;
        const auto rotation = rotationOf(runtime);
        for (auto& [id, stream] : streams) {
            if (stream.camera != runtime.description.index || !startedVideo(stream)) continue;
            // A stream slower than the camera takes the frames that fall on its interval.
            const std::uint64_t interval = 1'000'000'000ull * stream.intervalNumerator / stream.intervalDenominator;
            if (stream.delivered && frame.timestampNanoseconds + interval / 4 < stream.lastDelivered + interval) continue;
            if (stream.queued.empty()) {
                ++stream.framesDropped;
                continue;
            }
            const auto held = stream.queued.front();
            stream.queued.pop_front();
            auto& resource = stream.resources.at(held.resource);
            const bool ok = writeVideo(stream, resource, &frame, rotation);
            stream.delivered = true;
            stream.lastDelivered = frame.timestampNanoseconds;
            ++stream.framesOut;
            complete(id, held, age, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
                     ok ? videoPlaneSize(stream) : 0, sequence);
        }
        pushMeta(runtime, metaOf(runtime, sequence, frame.exposure, 0), frame.timestampNanoseconds);
    }

    void blackTick(std::uint32_t id)
    {
        const auto found = streams.find(id);
        if (found == streams.end()) return;
        auto& stream = found->second;
        auto* runtime = runtimeOf(stream.camera);
        if (!runtime || !startedVideo(stream)) return;
        if (!blackMode(*runtime)) {
            disarm(stream);
            return;
        }
        // A capture that could not start is tried again, once a second, whether
        // the session could not open or the camera could not join it.
        if (!blocked() && runtime->failed && now() - runtime->videoRetry >= sessionRetryNanoseconds) reconfigure(*runtime);
        if (!blackMode(*runtime)) {
            disarm(stream);
            return;
        }
        if (stream.queued.empty()) return;
        const auto held = stream.queued.front();
        stream.queued.pop_front();
        auto& resource = stream.resources.at(held.resource);
        const auto size = videoPlaneSize(stream);
        bool ok = false;
        if (!resource.planes.empty() && resource.capacity >= size) {
            if (stream.black.size() != size) {
                stream.black.assign(size, 0);
                uml::platform::Nv12Target target {stream.black.data(), pitchOf(stream),
                                                  stream.black.data() + std::size_t(pitchOf(stream)) * stream.height, pitchOf(stream),
                                                  stream.width, stream.height};
                uml::platform::fillNv12Black(target);
            }
            ok = transferSegments(mapper, regions.get(), resource.planes[0], stream.black, true);
        }
        const auto sequence = ++runtime->sequence;
        const std::uint32_t metaFlags = blackMetaFlags(*runtime);
        ++stream.framesOut;
        complete(id, held, 0, (ok ? blackFlags(*runtime) : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR), ok ? size : 0, sequence);
        CameraExposure black;
        black.activeCamera = std::uint32_t(runtime->description.platformIndex);
        pushMeta(*runtime, metaOf(*runtime, sequence, black, metaFlags), now());
    }

    void arm(Stream& stream)
    {
        const std::uint64_t microseconds = std::max<std::uint64_t>(1000, 1'000'000ull * stream.intervalNumerator / stream.intervalDenominator);
        struct kevent change {};
        EV_SET(&change, stream.id, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS, static_cast<intptr_t>(microseconds), nullptr);
        if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) == 0) stream.timerArmed = true;
    }

    void disarm(Stream& stream) noexcept
    {
        if (!stream.timerArmed) return;
        struct kevent change {};
        EV_SET(&change, stream.id, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
        stream.timerArmed = false;
    }

    /* Starts the black-frame timer of each started video stream of a camera
     * whose frames are black, and stops it where they are not. */
    void updateBlackTimers(Runtime& runtime)
    {
        const bool black = blackMode(runtime);
        for (auto& [id, stream] : streams) {
            (void)id;
            if (stream.camera != runtime.description.index || stream.kind != VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) continue;
            if (black && stream.started && !stream.timerArmed) arm(stream);
            else if ((!black || !stream.started) && stream.timerArmed) disarm(stream);
        }
    }

    // ------------------------------------------------------------------------
    // Stills
    // ------------------------------------------------------------------------

    [[nodiscard]] Group* groupOfToken(std::uint64_t token)
    {
        const auto found = std::find_if(groups.begin(), groups.end(), [token](const auto& group) { return group.fired && group.token == token; });
        return found == groups.end() ? nullptr : &*found;
    }

    void failGroup(Group& group, std::uint32_t extra)
    {
        for (const auto& member : group.members) complete(member.stream, member.held, member.held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR | extra);
    }

    void eraseGroup(std::uint64_t token)
    {
        std::erase_if(groups, [token](const auto& group) { return group.fired && group.token == token; });
    }

    /* One timer, set for the earliest deadline a group has. */
    void armGroupTimer()
    {
        struct kevent change {};
        if (groups.empty()) {
            EV_SET(&change, groupTimerIdent, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
            ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
            return;
        }
        std::uint64_t earliest = groups.front().deadline;
        for (const auto& group : groups) earliest = std::min(earliest, group.deadline);
        const auto time = now();
        const std::uint64_t wait = earliest > time ? earliest - time : 0;
        EV_SET(&change, groupTimerIdent, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_USECONDS,
               static_cast<intptr_t>(std::max<std::uint64_t>(1000, wait / 1000)), nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
    }

    /* A still buffer arrived. A buffer without GROUP_MORE completes a group of
     * its cookie and starts the capture. */
    void queueStill(Stream& stream, Runtime& runtime, const Held& held)
    {
        stream.started = true;
        std::uint32_t refusal = 0;
        if (blocked()) refusal = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED | VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
        else if (runtime.interruption != Interruption::none) refusal = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
        if (refusal != 0) {
            complete(stream.id, held, held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR | refusal);
            return;
        }
        const bool more = (held.flags & VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE) != 0 && held.cookie != 0;
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
            return !group.fired && group.camera == runtime.description.index && group.cookie == held.cookie && held.cookie != 0;
        });
        if (found == groups.end()) {
            Group group;
            group.camera = runtime.description.index;
            group.cookie = held.cookie;
            group.deadline = now() + groupTimeoutNanoseconds;
            groups.push_back(std::move(group));
            found = groups.end() - 1;
            armGroupTimer();
        }
        found->members.push_back({stream.id, held});
        if (more) return;
        fire(*found, runtime);
    }

    /* The captures a camera has with the platform. */
    [[nodiscard]] std::size_t capturesInFlight(std::uint32_t camera) const
    {
        return std::size_t(std::count_if(groups.begin(), groups.end(),
                                         [camera](const auto& group) { return group.fired && group.camera == camera; }));
    }

    void fire(Group& group, Runtime& runtime)
    {
        CameraStillRequest request;
        std::uint32_t refusal = 0;
        bool jpeg = false, raw = false, calibrationRaw = false;
        for (const auto& member : group.members) {
            const auto& stream = streams.at(member.stream);
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                if (jpeg) refusal = 1;
                jpeg = true;
                request.jpegWidth = stream.width;
                request.jpegHeight = stream.height;
            } else {
                if (raw) refusal = 1;
                raw = true;
                request.raw = true;
            }
        }
        request.jpegQuality = runtime.controls.jpegQuality(runtime.values);
        request.flash = runtime.controls.stillFlash(runtime.values);
        // The first still of an uncalibrated lens also teaches its colour data.
        const auto& info = runtime.description.info;
        if (!raw && info.rawStills && !runtime.description.calibration && runtime.calibrationAttempts < 3 &&
            std::fabs(runtime.controls.settings(runtime.values).zoomRatio - 1.0f) < 1e-3f) {
            request.raw = true;
            request.speedPrioritized = true;
            calibrationRaw = true;
            ++runtime.calibrationAttempts;
        }
        if (raw) request.speedPrioritized = true;
        // A guest holds at most a pipeline's worth of captures with the platform.
        if (capturesInFlight(runtime.description.index) >= limits.buffers) refusal = 1;
        if (refusal == 0 && !ensureSession()) refusal = 1;
        group.token = request.token = nextToken++;
        group.fired = true;
        if (refusal == 0) {
            applySettings(runtime, false);
            bool taken = session->takeStill(runtime.description.platformIndex, request);
            if (!taken && calibrationRaw) {
                // The colour data is a courtesy: the person's photo does not
                // wait for it, so it is asked for again without it.
                request.raw = request.speedPrioritized = false;
                taken = session->takeStill(runtime.description.platformIndex, request);
            }
            if (!taken) refusal = 1;
        }
        if (refusal != 0) {
            failGroup(group, sessionFailed ? std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED) : 0u);
            eraseGroup(group.token);
            releaseSessionIfIdle();
        } else {
            // The group is whole; the capture has its own time.
            group.deadline = now() + captureTimeoutNanoseconds;
            armGroupTimer();
        }
        reportState();
    }

    void deliverStill(std::size_t platformCamera, CameraStillResult& result)
    {
        auto* group = groupOfToken(result.token);
        const auto wire = wireOfPlatform.find(platformCamera);
        if (!group || wire == wireOfPlatform.end()) return;
        auto& runtime = runtimes[wire->second];
        if (!result.ok) {
            failGroup(*group, runtime.interruption != Interruption::none ? std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED) : 0u);
            eraseGroup(result.token);
            checkpoint(CameraCheckpoint::stillDelivered);
            releaseSessionIfIdle();
            reportState();
            return;
        }
        // A Bayer still teaches the lens's colour data, which reaches the guest
        // at its next start.
        if (result.color.bayer != uml::platform::BayerOrder::none && !runtime.description.calibration) {
            runtime.description.calibration = result.color;
            calibration->save(model, CalibrationStore::lensKey(runtime.description.info), result.color);
        }
        const auto sequence = ++runtime.sequence;
        auto meta = metaOf(runtime, sequence, result.exposure, VIRTIO_VIDEO_CAMERA_FRAME_F_STILL);
        runtime.latest = result.exposure;
        for (const auto& member : group->members) {
            auto& stream = streams.at(member.stream);
            auto& resource = stream.resources.at(member.held.resource);
            std::uint32_t size = 0;
            bool ok = false;
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                auto plan = reserveExifGap(result.jpeg);
                // The guest asked for this size and gets this size or an error: a
                // platform picture of another size is not the one it configured.
                if (plan && (plan->frameWidth != stream.width || plan->frameHeight != stream.height)) {
                    std::fprintf(stderr, "Virtio camera stream %u: the still is %ux%u, not the %ux%u that was asked for\n", stream.id,
                                 unsigned(plan->frameWidth), unsigned(plan->frameHeight), stream.width, stream.height);
                    plan.reset();
                }
                if (plan && plan->size() <= resource.capacity && !resource.planes.empty()) {
                    // The scan is the end of the platform's own bytes, written from where they lie.
                    const std::span<std::uint8_t> tail(result.jpeg.data() + (plan->tail.data() - result.jpeg.data()), plan->tail.size());
                    ok = transferSegments(mapper, regions.get(), resource.planes[0], plan->head, true) &&
                        transferSegments(mapper, regions.get(), resource.planes[0], tail, true, plan->head.size());
                    size = std::uint32_t(plan->size());
                    meta.jpeg_gap_offset = plan->gapOffset;
                    meta.jpeg_gap_length = plan->gapLength;
                }
            } else if (!result.raw.empty() && result.color.rawWidth != 0) {
                ok = writeRaw(stream, resource, result, size);
            }
            complete(stream.id, member.held, member.held.cookie, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, ok ? size : 0, sequence);
        }
        pushMeta(runtime, meta, 0);
        eraseGroup(result.token);
        checkpoint(CameraCheckpoint::stillDelivered);
        releaseSessionIfIdle();
        reportState();
    }

    bool writeRaw(const Stream& stream, Resource& resource, CameraStillResult& result, std::uint32_t& size)
    {
        const std::uint32_t width = result.color.rawWidth;
        const std::uint32_t height = result.color.rawHeight;
        if (width != stream.width || height != stream.height || result.raw.size() != std::size_t(width) * height) return false;
        const std::size_t rowBytes = std::size_t(width) * 2;
        const auto stride = rawPitch(std::uint32_t(rowBytes));
        const std::uint64_t total = std::uint64_t(stride) * height;
        if (resource.capacity < total || resource.planes.empty()) return false;
        auto* bytes = reinterpret_cast<std::uint8_t*>(result.raw.data());
        if (stride == rowBytes) {
            // Tight rows are the buffer: one pass over the segments.
            if (!transferSegments(mapper, regions.get(), resource.planes[0], std::span<std::uint8_t>(bytes, rowBytes * height), true))
                return false;
        } else {
            uml::video::GuestPlaneWriter writer(mapper, regions.get(), resource.planes[0]);
            for (std::uint32_t row = 0; row < height; ++row)
                if (!writer.write(std::uint64_t(row) * stride, std::span<const std::uint8_t>(bytes + std::size_t(row) * rowBytes, rowBytes)))
                    return false;
        }
        size = std::uint32_t(total);
        return true;
    }

    void expireGroups()
    {
        const auto time = now();
        for (std::size_t index = 0; index < groups.size();) {
            auto& group = groups[index];
            if (group.deadline > time) {
                ++index;
                continue;
            }
            failGroup(group, 0);
            groups.erase(groups.begin() + std::ptrdiff_t(index));
        }
        armGroupTimer();
        releaseSessionIfIdle();
        reportState();
    }

    // ------------------------------------------------------------------------
    // Platform events
    // ------------------------------------------------------------------------

    void handlePlatformEvent(const CameraEvent& platformEvent)
    {
        const auto each = [&](auto&& body) {
            if (platformEvent.camera == uml::platform::allCameras) {
                for (auto& runtime : runtimes) body(runtime);
            } else if (const auto wire = wireOfPlatform.find(platformEvent.camera); wire != wireOfPlatform.end()) {
                body(runtimes[wire->second]);
            }
        };
        switch (platformEvent.kind) {
        case CameraEventKind::interrupted:
        case CameraEventKind::interruptedByMultitasking:
        case CameraEventKind::interruptedBySystemPressure:
        case CameraEventKind::unavailable:
            each([&](Runtime& runtime) {
                runtime.interruption = platformEvent.kind == CameraEventKind::interruptedByMultitasking ? Interruption::multitasking
                    : platformEvent.kind == CameraEventKind::interruptedBySystemPressure ? Interruption::pressure
                                                                                          : Interruption::interrupted;
                markDown(runtime, platformEvent.kind == CameraEventKind::unavailable ? VIRTIO_VIDEO_EVENT_CAMERA_UNAVAILABLE
                                                                                      : VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
                updateBlackTimers(runtime);
            });
            break;
        case CameraEventKind::resumed:
        case CameraEventKind::available:
            each([&](Runtime& runtime) {
                if (runtime.interruption == Interruption::none && !runtime.failed) return;
                runtime.interruption = Interruption::none;
                runtime.failed = false;
                // A capture that went away is started again, and black frames
                // bridge the gap until it delivers.
                reconfigure(runtime);
                runtime.resuming = runtime.videoRunning;
                settle(runtime);
                updateBlackTimers(runtime);
            });
            break;
        case CameraEventKind::failed:
            // The session is gone; it is opened again when a stream needs it.
            // The cameras that have a stream are down until it runs again, and
            // are told so before the session's end could tell them they are
            // back.
            each([&](Runtime& runtime) {
                runtime.videoRunning = false;
                if (!wantsVideo(runtime)) return;
                runtime.failed = true;
                markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
            });
            closeSession();
            sessionFailed = true;
            lastRetry = now();
            each([&](Runtime& runtime) { updateBlackTimers(runtime); });
            break;
        }
        reportState();
    }

    /* Takes what the platform produced since the last pass. */
    void pump()
    {
        std::deque<InboxFrame> frames;
        std::deque<InboxStill> stills;
        std::deque<CameraEvent> platformEvents;
        {
            std::lock_guard lock(inboxMutex);
            frames.swap(inboxFrames);
            stills.swap(inboxStills);
            platformEvents.swap(inboxEvents);
        }
        for (const auto& platformEvent : platformEvents) handlePlatformEvent(platformEvent);
        for (auto& still : stills) deliverStill(still.camera, still.result);
        for (auto& entry : frames) {
            const auto wire = wireOfPlatform.find(entry.camera);
            if (wire == wireOfPlatform.end()) continue;
            auto& runtime = runtimes[wire->second];
            if (runtime.resuming && runtime.interruption == Interruption::none) {
                runtime.resuming = false;
                settle(runtime);
                updateBlackTimers(runtime);
            }
            if (blackMode(runtime)) continue;
            deliverFrame(runtime, entry.frame);
        }
    }

    // ------------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------------

    VhostUserQueueResult done(std::vector<std::uint8_t> data) const { return {VhostUserQueueAction::complete, std::move(data)}; }

    [[nodiscard]] virtio_video_params paramsOf(const Stream& stream) const
    {
        virtio_video_params p {};
        p.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
        p.format = stream.format;
        p.frame_width = stream.width;
        p.frame_height = stream.height;
        p.min_buffers = 1;
        p.max_buffers = limits.buffers;
        p.crop = {0, 0, stream.width, stream.height};
        p.frame_rate = rateOf(stream);
        p.num_planes = 1;
        switch (stream.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
            p.plane_formats[0] = {videoPlaneSize(stream), pitchOf(stream)};
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG)
                p.plane_formats[0] = {std::uint32_t(std::uint64_t(stream.width) * stream.height * 3 / 2 + exifGapLength), 0};
            else
                p.plane_formats[0] = {rawPitch(2 * stream.width) * stream.height, rawPitch(2 * stream.width)};
            break;
        default:
            p.plane_formats[0] = {std::uint32_t(sizeof(virtio_video_camera_meta)), 0};
            break;
        }
        return p;
    }

    VhostUserQueueResult queryControls(std::uint32_t id, const Runtime& runtime, std::uint32_t start, std::size_t capacity)
    {
        const auto& descriptors = runtime.controls.descriptors();
        if (start > descriptors.size()) throw std::invalid_argument("invalid control index");
        const auto room = capacity > sizeof(virtio_video_query_v4l2_controls_resp)
            ? (capacity - sizeof(virtio_video_query_v4l2_controls_resp)) / sizeof(virtio_video_v4l2_control_desc) : 0;
        const auto count = std::min<std::size_t>(descriptors.size() - start, room);
        if (count == 0 && start < descriptors.size()) throw std::invalid_argument("control response is too small");
        virtio_video_query_v4l2_controls_resp response {};
        response.hdr = {VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, id};
        response.num_descs = std::uint32_t(count);
        response.next_index = start + count < descriptors.size() ? std::uint32_t(start + count) : 0;
        auto result = toBytes(response);
        for (std::size_t index = 0; index < count; ++index) {
            const auto bytes = toBytes(descriptors[start + index]);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        return done(std::move(result));
    }

    VhostUserQueueResult setControls(std::uint32_t id, Runtime& runtime, std::span<const std::uint8_t> request)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        const auto fail = [&](std::uint32_t index, std::uint32_t type) {
            return done(toBytes(virtio_video_v4l2_controls_resp {{type, id}, index, count}));
        };
        const auto area = request.subspan(sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value));
        // Every value is checked first, and applies in order to a copy, so a
        // refusal changes nothing.
        ControlValues next = runtime.values;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = runtime.controls.find(entry.id);
            if (!descriptor) return fail(index, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL);
            if (descriptor->flags & VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            std::vector<std::uint8_t> payload;
            if (descriptor->elem_size == 0 && entry.size != 0) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            if (descriptor->elem_size != 0) {
                if (entry.size != descriptor->elem_size || entry.value % 8 != 0 || entry.value > area.size() ||
                    area.size() - entry.value < entry.size)
                    return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
                payload.assign(area.begin() + std::ptrdiff_t(entry.value), area.begin() + std::ptrdiff_t(entry.value + entry.size));
            }
            const auto value = descriptor->elem_size != 0 ? std::int64_t(0) : static_cast<std::int64_t>(entry.value);
            if (runtime.controls.check(entry.id, value, payload.size()) != ControlCheck::ok)
                return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            runtime.controls.store(next, entry.id, value, std::move(payload));
        }
        const bool wasBlack = blackMode(runtime);
        runtime.values = std::move(next);
        // Applied as soon as the host can.
        const auto settings = runtime.controls.settings(runtime.values);
        if (!blocked()) {
            if (settings.torch && !session) ensureSession();
            applySettings(runtime, false);
            if (!settings.torch) releaseSessionIfIdle();
        }
        if (wasBlack != blackMode(runtime)) updateBlackTimers(runtime);
        reportState();
        return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count}));
    }

    VhostUserQueueResult getControls(std::uint32_t id, const Runtime& runtime, std::span<const std::uint8_t> request,
                                     std::size_t capacity)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        std::vector<virtio_video_v4l2_control_value> values;
        std::vector<std::uint8_t> area;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = runtime.controls.find(entry.id);
            if (!descriptor)
                return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, id}, index, count}));
            if (descriptor->elem_size != 0) {
                const auto stored = runtime.values.payloads.find(entry.id);
                std::vector<std::uint8_t> payload = stored == runtime.values.payloads.end()
                    ? std::vector<std::uint8_t>(descriptor->elem_size) : stored->second;
                payload.resize(descriptor->elem_size);
                entry.size = descriptor->elem_size;
                entry.value = area.size();
                area.insert(area.end(), payload.begin(), payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            } else {
                entry.size = 0;
                entry.value = static_cast<std::uint64_t>(
                    runtime.controls.read(runtime.values, entry.id, runtime.latest ? &*runtime.latest : nullptr));
            }
            values.push_back(entry);
        }
        auto result = toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count});
        for (const auto& value : values) {
            const auto bytes = toBytes(value);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        result.insert(result.end(), area.begin(), area.end());
        if (result.size() > capacity) throw std::invalid_argument("control response is too small");
        return done(std::move(result));
    }

    /* A stream's first state. */
    Stream makeStream(std::uint32_t id, const virtio_video_camera_stream_create& create)
    {
        const auto* runtime = runtimeOf(create.camera_index);
        if (!runtime) throw std::invalid_argument("no such camera");
        const auto& description = runtime->description;
        Stream stream;
        stream.id = id;
        stream.camera = create.camera_index;
        stream.kind = create.kind;
        switch (create.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO: {
            if (description.video.empty()) throw std::invalid_argument("the camera has no video");
            stream.format = VIRTIO_VIDEO_FORMAT_NV12;
            // The size nearest 1280 x 720 without exceeding it, else the smallest.
            const VideoMode* best = nullptr;
            for (const auto& mode : description.video)
                if (mode.width <= defaultVideoWidth && mode.height <= defaultVideoHeight &&
                    (!best || std::uint64_t(mode.width) * mode.height > std::uint64_t(best->width) * best->height))
                    best = &mode;
            if (!best) best = &description.video.back();
            stream.width = best->width;
            stream.height = best->height;
            stream.intervalDenominator = supportedFrameRate(description, stream.width, stream.height, 30);
            if (stream.intervalDenominator == 0) throw std::invalid_argument("the camera has no frame rate");
            break;
        }
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (!description.jpegSizes.empty()) {
                stream.format = VIRTIO_VIDEO_FORMAT_CAMERA_JPEG;
                stream.width = description.jpegSizes.front().width;
                stream.height = description.jpegSizes.front().height;
            } else if (description.raw()) {
                stream.format = rawWireFormat(description.calibration->bayer);
                stream.width = description.rawSize.width;
                stream.height = description.rawSize.height;
            } else {
                throw std::invalid_argument("the camera takes no stills");
            }
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_META:
            stream.format = VIRTIO_VIDEO_FORMAT_CAMERA_META;
            break;
        default: throw std::invalid_argument("invalid camera stream kind");
        }
        return stream;
    }

    VhostUserQueueResult setParams(Stream& stream, const virtio_video_params& p)
    {
        const auto id = stream.id;
        if (p.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("a camera stream has only the output queue");
        const auto& description = runtimes[stream.camera].description;
        std::uint32_t format = p.format, width = p.frame_width, height = p.frame_height;
        switch (stream.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
            if (format != VIRTIO_VIDEO_FORMAT_NV12 || !videoSizeAllowed(description, width, height))
                throw std::invalid_argument("unsupported camera video format");
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                if (std::find(description.jpegSizes.begin(), description.jpegSizes.end(), uml::platform::CameraStillSize {width, height}) ==
                    description.jpegSizes.end())
                    throw std::invalid_argument("unsupported camera still size");
            } else if (!description.raw() || format != rawWireFormat(description.calibration->bayer) ||
                       width != description.rawSize.width || height != description.rawSize.height) {
                throw std::invalid_argument("unsupported camera still format");
            }
            break;
        default:
            if (format != VIRTIO_VIDEO_FORMAT_CAMERA_META) throw std::invalid_argument("unsupported camera metadata format");
            width = height = 0;
            break;
        }
        const bool same = format == stream.format && width == stream.width && height == stream.height;
        if (same) return done(reply(id));
        if (stream.started) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_BUSY));
        if (!stream.resources.empty()) throw std::invalid_argument("camera format changed with allocated buffers");
        stream.format = format;
        stream.width = width;
        stream.height = height;
        if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
            const auto rate = supportedFrameRate(description, width, height, rateOf(stream));
            stream.intervalNumerator = 1;
            stream.intervalDenominator = rate != 0 ? rate : 1;
        }
        return done(reply(id));
    }

    /* Everything a stream has queued but not filled goes back as an error. */
    void cancelQueued(Stream& stream)
    {
        for (const auto& held : stream.queued) complete(stream.id, held, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR);
        stream.queued.clear();
        stream.backlog.clear();
        for (auto& group : groups)
            for (auto member = group.members.begin(); member != group.members.end();) {
                if (member->stream == stream.id) {
                    complete(stream.id, member->held, member->held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR);
                    member = group.members.erase(member);
                } else {
                    ++member;
                }
            }
        std::erase_if(groups, [](const auto& group) { return group.members.empty() && !group.fired; });
    }

    void stopStream(Stream& stream)
    {
        const bool wasVideo = startedVideo(stream);
        cancelQueued(stream);
        stream.started = false;
        stream.delivered = false;
        disarm(stream);
        if (wasVideo) {
            if (auto* runtime = runtimeOf(stream.camera)) {
                reconfigure(*runtime);
                updateBlackTimers(*runtime);
            }
        }
        releaseSessionIfIdle();
        reportState();
    }

    VhostUserQueueResult command(std::uint64_t requestId, std::span<const std::uint8_t> request, std::size_t capacity)
    {
        const auto header = load<virtio_video_cmd_hdr>(request);
        const auto id = header.stream_id;
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY: {
            const auto q = load<virtio_video_camera_query>(request);
            std::vector<CameraDescription> descriptions;
            descriptions.reserve(runtimes.size());
            for (const auto& runtime : runtimes) descriptions.push_back(runtime.description);
            if (q.start_index > descriptions.size()) throw std::invalid_argument("invalid camera index");
            return done(cameraQueryResponse(descriptions, q.start_index, capacity,
                                            blocked() ? VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED : 0));
        }
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS: {
            const auto q = load<virtio_video_camera_query_formats>(request);
            const auto* runtime = runtimeOf(q.camera_index);
            if (!runtime) throw std::invalid_argument("no such camera");
            if (q.kind != VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && q.kind != VIRTIO_VIDEO_CAMERA_STREAM_STILL)
                throw std::invalid_argument("invalid camera stream kind");
            return done(cameraFormatsResponse(runtime->description, q.kind));
        }
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS: {
            const auto q = load<virtio_video_camera_query_controls>(request);
            const auto* runtime = runtimeOf(q.camera_index);
            if (!runtime) throw std::invalid_argument("no such camera");
            return queryControls(id, *runtime, q.start_index, capacity);
        }
        case VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE: {
            const auto q = load<virtio_video_camera_stream_create>(request);
            if (!id || id >= reservedStreamIds || streams.contains(id)) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            if (streams.size() >= maximumStreams) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY));
            streams.emplace(id, makeStream(id, q));
            return done(reply(id));
        }
        default: break;
        }
        const auto found = streams.find(id);
        if (found == streams.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID));
        auto& stream = found->second;
        auto& runtime = runtimes[stream.camera];
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            stopStream(stream);
            streams.erase(found);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_GET_PARAMS: {
            const auto q = load<virtio_video_get_params>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid parameter queue");
            return done(toBytes(virtio_video_get_params_resp {{VIRTIO_VIDEO_RESP_OK_GET_PARAMS, id}, paramsOf(stream)}));
        }
        case VIRTIO_VIDEO_CMD_SET_PARAMS: return setParams(stream, load<virtio_video_set_params>(request).params);
        case VIRTIO_VIDEO_CMD_SET_STREAM_INFO: {
            const auto q = load<virtio_video_set_stream_info>(request).info;
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid stream information queue");
            if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && q.frame_interval_numerator != 0 && q.frame_interval_denominator != 0) {
                // The interval is adjusted to a rate the camera runs the size at.
                const auto wanted = std::max<std::uint32_t>(1, (q.frame_interval_denominator + q.frame_interval_numerator / 2) / q.frame_interval_numerator);
                const auto rate = supportedFrameRate(runtime.description, stream.width, stream.height, wanted);
                if (rate == 0) throw std::invalid_argument("unsupported camera frame rate");
                const bool changed = rate != rateOf(stream);
                stream.intervalNumerator = 1;
                stream.intervalDenominator = rate;
                if (changed && stream.started) {
                    reconfigure(runtime);
                    updateBlackTimers(runtime);
                    if (stream.timerArmed) arm(stream);
                }
            }
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_GET_STREAM_INFO: {
            const auto q = load<virtio_video_get_stream_info>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid stream information queue");
            virtio_video_stream_info info {};
            info.queue_type = q.queue_type;
            info.frame_interval_numerator = stream.intervalNumerator;
            info.frame_interval_denominator = stream.intervalDenominator;
            if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
                info.colorspace = colorspaceRec709;
                info.xfer_func = xferFunc709;
                info.ycbcr_enc = ycbcrEnc709;
                info.quantization = quantizationFullRange;
            }
            return done(toBytes(virtio_video_get_stream_info_resp {{VIRTIO_VIDEO_RESP_OK_STREAM_INFO, id}, info}));
        }
        case VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS: {
            const auto q = load<virtio_video_query_v4l2_controls>(request);
            return queryControls(id, runtime, q.start_index, capacity);
        }
        case VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS: return setControls(id, runtime, request);
        case VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS: return getControls(id, runtime, request, capacity);
        case VIRTIO_VIDEO_CMD_RESOURCE_ATTACH: {
            const auto q = load<virtio_video_resource_attach>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT || !q.resource_id) throw std::invalid_argument("invalid camera resource");
            const auto previous = stream.resources.find(q.resource_id);
            if (previous != stream.resources.end() && previous->second.queued)
                throw std::invalid_argument("camera resource is already queued");
            if (previous == stream.resources.end() && stream.resources.size() >= 128) throw std::invalid_argument("invalid camera resource");
            Resource resource;
            resource.planes = uml::video::parseScatterLists(request.subspan(sizeof(q)), 1, mapper, regions.get());
            for (const auto& segment : resource.planes[0]) resource.capacity += segment.length;
            if (resource.capacity < paramsOf(stream).plane_formats[0].plane_size)
                throw std::invalid_argument("camera resource is smaller than the buffer");
            if (previous != stream.resources.end()) previous->second = std::move(resource);
            else stream.resources.emplace(q.resource_id, std::move(resource));
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE: {
            const auto q = load<virtio_video_resource_queue>(request);
            const auto resource = stream.resources.find(q.resource_id);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT || resource == stream.resources.end())
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID));
            if (resource->second.queued) throw std::invalid_argument("camera resource is already queued");
            if (stream.queued.size() >= maximumHeld) throw std::invalid_argument("camera queue is full");
            const Held held {requestId, q.resource_id, q.timestamp, q.flags};
            resource->second.queued = true;
            switch (stream.kind) {
            case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
                stream.queued.push_back(held);
                if (!stream.started) {
                    stream.started = true;
                    reconfigure(runtime);
                    updateBlackTimers(runtime);
                    reportState();
                }
                break;
            case VIRTIO_VIDEO_CAMERA_STREAM_STILL: queueStill(stream, runtime, held); break;
            default:
                stream.queued.push_back(held);
                stream.started = true;
                serveMeta(id, stream);
                break;
            }
            return {VhostUserQueueAction::defer, {}};
        }
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        case VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES: {
            const auto q = load<virtio_video_queue_clear>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid clear queue");
            stopStream(stream);
            if (header.type == VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES) stream.resources.clear();
            return done(reply(id));
        }
        default: return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION));
        }
    }

    void drainEvents()
    {
        std::array<struct kevent, 8> ready {};
        const timespec none {};
        bool user = false, groupTimer = false;
        std::vector<std::uint32_t> ticks;
        for (;;) {
            int count;
            do {
                count = ::kevent(queue.get(), nullptr, 0, ready.data(), int(ready.size()), &none);
            } while (count < 0 && errno == EINTR);
            if (count < 0) throw std::runtime_error("draining the camera events failed");
            for (int index = 0; index < count; ++index) {
                const auto& entry = ready[std::size_t(index)];
                if (entry.filter == EVFILT_USER) user = true;
                else if (entry.filter == EVFILT_TIMER && entry.ident == groupTimerIdent) groupTimer = true;
                else if (entry.filter == EVFILT_TIMER) ticks.push_back(std::uint32_t(entry.ident));
            }
            if (count < int(ready.size())) break;
        }
        (void)user;
        pump();
        for (const auto id : ticks) blackTick(id);
        if (groupTimer || !groups.empty()) expireGroups();
    }

    void shutdown() noexcept
    {
        for (auto& [id, stream] : streams) {
            (void)id;
            disarm(stream);
        }
        closeSession();
        streams.clear();
        groups.clear();
        events.clear();
        completions.clear();
        {
            std::lock_guard lock(inboxMutex);
            inboxFrames.clear();
            inboxStills.clear();
            inboxEvents.clear();
        }
        for (auto& runtime : runtimes) {
            runtime.interruption = Interruption::none;
            runtime.resuming = false;
            runtime.failed = false;
            runtime.told = false;
            runtime.toldUnavailable = false;
            runtime.videoRetry = 0;
            runtime.calibrationAttempts = 0;
            runtime.values = runtime.controls.defaults();
            runtime.latest.reset();
            runtime.sequence = 0;
            runtime.videoRunning = false;
            runtime.appliedValid = false;
        }
        if (reported != CameraState::idle) {
            reported = CameraState::idle;
            try {
                if (options.onState) options.onState(CameraState::idle);
            } catch (...) {
                // The application's notice must not stop a reset.
            }
        }
    }
};

VirtioCameraDevice::VirtioCameraDevice(std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options)
    : implementation_(std::make_unique<Implementation>(std::move(host), std::move(options)))
{
}
VirtioCameraDevice::~VirtioCameraDevice() = default;
const char* VirtioCameraDevice::name() const noexcept { return "Virtio Video camera"; }
std::uint64_t VirtioCameraDevice::features() const noexcept
{
    const std::uint64_t base = (1u << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES) | (1u << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG) |
        (1u << VIRTIO_VIDEO_F_V4L2_CONTROLS);
    return implementation_->regions ? base | (1u << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION) : base;
}
std::size_t VirtioCameraDevice::queueCount() const noexcept { return 2; }
std::vector<std::uint8_t> VirtioCameraDevice::configuration() const
{
    /* The longest answers: a format query, and a page of camera or control
     * descriptors, which the device splits to fit the buffer the guest gave. */
    return toBytes(virtio_video_config {UML_VIRTIO_VIDEO_VERSION, 16384, 4096});
}
void VirtioCameraDevice::writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) {}
void VirtioCameraDevice::setGuestMapper(GuestMapper mapper) { implementation_->mapper = std::move(mapper); }
void VirtioCameraDevice::setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions)
{
    implementation_->regions = std::move(regions);
}
VhostUserQueueResult VirtioCameraDevice::processQueue(std::uint32_t index, std::uint64_t id,
                                                      std::span<const std::uint8_t> request, std::size_t capacity)
{
    auto& impl = *implementation_;
    if (index == 1) {
        if (!request.empty() || capacity < sizeof(virtio_video_event)) return {VhostUserQueueAction::complete, {}};
        if (!impl.events.empty()) {
            auto result = toBytes(impl.events.front());
            impl.events.pop_front();
            return {VhostUserQueueAction::complete, std::move(result)};
        }
        /* No event is waiting: the buffer stays in the guest's ring, as an
         * input device's does, and the server offers it again when an event
         * wakes the device. */
        return {VhostUserQueueAction::retry, {}};
    }
    if (index != 0 || request.size() < sizeof(virtio_video_cmd_hdr) || capacity < sizeof(virtio_video_cmd_hdr))
        return {VhostUserQueueAction::complete, {}};
    const auto header = load<virtio_video_cmd_hdr>(request);
    try {
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE && capacity < sizeof(virtio_video_camera_queue_resp))
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        auto result = impl.command(id, request, capacity);
        if (result.response.size() > capacity)
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        return result;
    } catch (const std::invalid_argument&) {
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtio camera stream %u: %s\n", header.stream_id, error.what());
        const auto found = impl.streams.find(header.stream_id);
        if (found != impl.streams.end()) impl.stopStream(found->second);
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE)
            return {VhostUserQueueAction::complete, impl.queued(header.stream_id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)};
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)};
    }
}
bool VirtioCameraDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    auto& pending = implementation_->completions;
    if (pending.empty()) return false;
    completion = std::move(pending.front());
    pending.pop_front();
    return true;
}
void VirtioCameraDevice::reset() noexcept { implementation_->shutdown(); }
int VirtioCameraDevice::eventDescriptor() const noexcept { return implementation_->queue.get(); }
void VirtioCameraDevice::drainEventSource() { implementation_->drainEvents(); }
std::vector<CameraDescription> VirtioCameraDevice::cameras() const
{
    std::vector<CameraDescription> result;
    result.reserve(implementation_->runtimes.size());
    for (const auto& runtime : implementation_->runtimes) result.push_back(runtime.description);
    return result;
}

} // namespace uml::camera
