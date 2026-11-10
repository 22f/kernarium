// SPDX-License-Identifier: GPL-2.0-or-later
// The Virtio camera device driven the way the guest driver drives it, over its
// two queues, in front of the synthetic platform camera. The platform camera
// has its own test; every case here is about what the device does with the
// guest's commands, buffers and controls, what it asks of the platform, and what
// it writes back. A blocked guest must never reach the platform's capture
// session at all.

#include "camera_controls.hpp"
#include "camera_device.hpp"
#include "in_process_service.hpp"
#include "jpeg_gap.hpp"
#include "nv12_transform.hpp"
#include "raw_pitch.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"
#include "virtio_video_v4l2_ids.h"
#include "wire_bytes.hpp"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace uml::camera;
using namespace uml::platform;
using namespace uml::virtio;
using uml::testing::append;
using uml::testing::load;
using uml::testing::require;
using uml::video::rawPitch;

namespace {

constexpr std::size_t page = 4096;
constexpr auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
constexpr std::uint32_t videoKind = VIRTIO_VIDEO_CAMERA_STREAM_VIDEO;
constexpr std::uint32_t stillKind = VIRTIO_VIDEO_CAMERA_STREAM_STILL;
constexpr std::uint32_t metaKind = VIRTIO_VIDEO_CAMERA_STREAM_META;
constexpr std::uint32_t errorFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
constexpr std::uint32_t blackFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
constexpr std::uint32_t interruptedFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
constexpr std::uint32_t blockedFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED;
constexpr std::uint32_t resumingFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING;
constexpr std::uint32_t metaSize = sizeof(virtio_video_camera_meta);

struct RigOptions {
    SyntheticCameraProfile profile {SyntheticCameraProfile::tablet};
    CameraMode mode {CameraMode::live};
    CameraTier tier {CameraTier::small};
    bool authorized {true};
    std::shared_ptr<CalibrationStore> store;
};

class Recorder final : public CameraSessionDelegate {
public:
    void cameraFrame(std::size_t, CameraFrame frame) override { frames.push_back(std::move(frame)); }
    void cameraStill(std::size_t, CameraStillResult) override {}
    void cameraEvent(CameraEvent) override {}
    std::vector<CameraFrame> frames;
};

struct Rig final {
    explicit Rig(RigOptions rigOptions = {}) : memory(24u << 20, 0xcc)
    {
        SyntheticCameraOptions synthetic;
        synthetic.profile = rigOptions.profile;
        synthetic.manualClock = true;
        synthetic.authorized = rigOptions.authorized;
        camera = makeSyntheticCamera(synthetic);
        store = rigOptions.store ? rigOptions.store : std::make_shared<CalibrationStore>();
        CameraDeviceOptions options;
        options.mode = rigOptions.mode;
        options.tier = rigOptions.tier;
        options.calibration = store;
        options.onState = [this](CameraState state) { states.push_back(state); };
        options.automaticRotation = [this] { return automaticDegrees ? *automaticDegrees : 0u; };
        device = std::make_unique<VirtioCameraDevice>(camera, std::move(options));
        device->setGuestMapper([this](std::uint64_t offset, std::size_t size) {
            if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t> {};
            return std::span(memory).subspan(offset, size);
        });
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    std::shared_ptr<SyntheticCamera> camera;
    std::shared_ptr<CalibrationStore> store;
    std::unique_ptr<VirtioCameraDevice> device;
    std::vector<CameraState> states;
    std::uint32_t* automaticDegrees {};
    std::vector<std::uint8_t> memory;
    std::size_t bump {16 * page};
    std::uint64_t next {1};
    std::map<std::uint64_t, std::vector<std::uint8_t>> completed;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::pair<std::uint64_t, std::uint32_t>>> segments;

    // --- Commands ----------------------------------------------------------

    template<class T> VhostUserQueueResult send(const T& value, std::size_t capacity = 16384)
    {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device->processQueue(0, next++, request, capacity);
    }
    static std::uint32_t type(const VhostUserQueueResult& result) { return load<virtio_video_cmd_hdr>(result.response).type; }
    template<class T> void ok(const T& value, std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "a command failed", where);
    }
    template<class T> void refused(const T& value, std::uint32_t expected, const char* what,
                                   std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == expected, what, where);
    }

    void create(std::uint32_t stream, std::uint32_t cameraIndex, std::uint32_t kind,
                std::source_location where = std::source_location::current())
    {
        virtio_video_camera_stream_create command {};
        command.hdr = {VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, stream};
        command.camera_index = cameraIndex;
        command.kind = kind;
        ok(command, where);
    }
    void destroy(std::uint32_t stream) { ok(virtio_video_stream_destroy {{VIRTIO_VIDEO_CMD_STREAM_DESTROY, stream}}); }
    virtio_video_params params(std::uint32_t stream, std::source_location where = std::source_location::current())
    {
        const auto result = send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, stream}, output, {}});
        require(type(result) == VIRTIO_VIDEO_RESP_OK_GET_PARAMS, "GET_PARAMS failed", where);
        return load<virtio_video_get_params_resp>(result.response).params;
    }
    VhostUserQueueResult setParams(std::uint32_t stream, std::uint32_t format, std::uint32_t width, std::uint32_t height)
    {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
        set.params.queue_type = output;
        set.params.format = format;
        set.params.frame_width = width;
        set.params.frame_height = height;
        return send(set);
    }
    void setVideo(std::uint32_t stream, std::uint32_t width, std::uint32_t height,
                  std::source_location where = std::source_location::current())
    {
        const auto result = setParams(stream, VIRTIO_VIDEO_FORMAT_NV12, width, height);
        require(type(result) == VIRTIO_VIDEO_RESP_OK_NODATA, "SET_PARAMS failed", where);
    }
    VhostUserQueueResult setInterval(std::uint32_t stream, std::uint32_t numerator, std::uint32_t denominator)
    {
        virtio_video_set_stream_info set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, stream};
        set.info.queue_type = output;
        set.info.frame_interval_numerator = numerator;
        set.info.frame_interval_denominator = denominator;
        return send(set);
    }

    // --- Guest memory ------------------------------------------------------

    /* A buffer of `size` bytes in two pieces with a page between them. */
    void attach(std::uint32_t stream, std::uint32_t resource, std::uint32_t size,
                std::source_location where = std::source_location::current())
    {
        const std::uint32_t first = (size / 2) & ~15u;
        const std::uint32_t second = size - first;
        const auto place = [&](std::uint32_t length) {
            const auto at = bump;
            bump += (length + page - 1) / page * page + page;
            require(bump <= memory.size(), "the guest memory is full", where);
            return at;
        };
        const auto firstAt = place(first), secondAt = place(second);
        segments[{stream, resource}] = {{firstAt, first}, {secondAt, second}};
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, stream, output, resource});
        append(request, std::uint32_t(2));
        append(request, std::uint32_t(0));
        append(request, virtio_video_resource_sg_entry {firstAt, first, 0});
        append(request, virtio_video_resource_sg_entry {secondAt, second, 0});
        const auto result = device->processQueue(0, next++, request, 16384);
        require(result.action == VhostUserQueueAction::complete && type(result) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "RESOURCE_ATTACH failed", where);
    }
    std::vector<std::uint8_t> bytes(std::uint32_t stream, std::uint32_t resource, std::size_t count)
    {
        std::vector<std::uint8_t> out;
        for (const auto& [address, length] : segments.at({stream, resource})) {
            const auto take = std::min<std::size_t>(length, count - out.size());
            out.insert(out.end(), memory.begin() + std::ptrdiff_t(address), memory.begin() + std::ptrdiff_t(address + take));
            if (out.size() == count) break;
        }
        return out;
    }

    // --- Buffers -----------------------------------------------------------

    std::uint64_t queue(std::uint32_t stream, std::uint32_t resource, std::uint64_t timestamp = 0, std::uint32_t flags = 0,
                        std::source_location where = std::source_location::current())
    {
        virtio_video_resource_queue command {};
        command.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        command.stream_id = stream;
        command.queue_type = output;
        command.resource_id = resource;
        command.flags = flags;
        command.timestamp = timestamp;
        std::vector<std::uint8_t> request;
        append(request, command);
        const auto id = next++;
        const auto result = device->processQueue(0, id, request, sizeof(virtio_video_camera_queue_resp));
        require(result.action == VhostUserQueueAction::defer, "RESOURCE_QUEUE did not defer", where);
        collect();
        return id;
    }
    void collect()
    {
        VhostUserQueueCompletion completion;
        while (device->takeQueueCompletion(completion)) completed[completion.requestId] = completion.response;
    }
    void step()
    {
        device->drainEventSource();
        collect();
    }
    bool done(std::uint64_t request) const { return completed.contains(request); }
    virtio_video_camera_queue_resp result(std::uint64_t request, std::source_location where = std::source_location::current()) const
    {
        const auto found = completed.find(request);
        require(found != completed.end(), "a buffer did not complete", where);
        require(load<virtio_video_cmd_hdr>(found->second).type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, "not a queue response", where);
        return load<virtio_video_camera_queue_resp>(found->second);
    }
    /* Waits, with a bound, for the device's own wake-up (a timer or the platform). */
    bool waitFor(const std::function<bool()>& condition, int milliseconds = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        step();
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            pollfd descriptor {device->eventDescriptor(), POLLIN, 0};
            ::poll(&descriptor, 1, 20);
            step();
        }
        return true;
    }

    // --- Controls ----------------------------------------------------------

    struct Item {
        std::uint32_t id {};
        std::int64_t value {};
        std::vector<std::uint8_t> payload;
    };
    VhostUserQueueResult controls(std::uint32_t command, std::uint32_t stream, const std::vector<Item>& items)
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{command, stream}, std::uint32_t(items.size()), {}});
        std::vector<std::uint8_t> area;
        for (const auto& item : items) {
            virtio_video_v4l2_control_value entry {item.id, 0, std::uint64_t(item.value)};
            if (!item.payload.empty()) {
                entry.size = std::uint32_t(item.payload.size());
                entry.value = area.size();
                area.insert(area.end(), item.payload.begin(), item.payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            }
            append(request, entry);
        }
        request.insert(request.end(), area.begin(), area.end());
        return device->processQueue(0, next++, request, 16384);
    }
    std::uint32_t set(std::uint32_t stream, const std::vector<Item>& items)
    {
        const auto result = controls(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, stream, items);
        require(result.action == VhostUserQueueAction::complete, "SET_V4L2_CONTROLS did not complete");
        return type(result);
    }
    void setOk(std::uint32_t stream, const std::vector<Item>& items, std::source_location where = std::source_location::current())
    {
        require(set(stream, items) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "a control set failed", where);
    }
    std::vector<std::int64_t> get(std::uint32_t stream, const std::vector<std::uint32_t>& ids,
                                  std::source_location where = std::source_location::current())
    {
        std::vector<Item> items;
        for (const auto id : ids) items.push_back({id, 0, {}});
        const auto result = controls(VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, stream, items);
        require(type(result) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "a control get failed", where);
        std::vector<std::int64_t> values;
        for (std::size_t index = 0; index < ids.size(); ++index)
            values.push_back(std::int64_t(load<virtio_video_v4l2_control_value>(
                                              result.response, sizeof(virtio_video_v4l2_controls_resp) +
                                                  index * sizeof(virtio_video_v4l2_control_value)).value));
        return values;
    }
};

virtio_video_camera_meta metaOf(Rig& rig, std::uint32_t stream, std::uint32_t resource)
{
    return load<virtio_video_camera_meta>(rig.bytes(stream, resource, metaSize));
}

std::uint32_t nv12Size(std::uint32_t width, std::uint32_t height) { return rawPitch(width) * (height + height / 2); }

/* What the device should have written for a frame the platform produced. */
std::vector<std::uint8_t> expectedNv12(const CameraFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t rotation)
{
    const auto pitch = rawPitch(width);
    std::vector<std::uint8_t> out(nv12Size(width, height));
    const Nv12Target target {out.data(), pitch, out.data() + std::size_t(pitch) * height, pitch, width, height};
    const Nv12Source source {frame.luma.data(), frame.lumaStride, frame.chroma.data(), frame.chromaStride, frame.width, frame.height};
    const bool quarter = rotation == 90 || rotation == 270;
    const auto crop = centredCrop(frame.width, frame.height, width, height, quarter);
    require(transformNv12(source, crop, rotation, target), "the reference transform failed");
    return out;
}

// ---------------------------------------------------------------------------

void testHandshake()
{
    Rig rig;
    const auto& device = *rig.device;
    require(device.queueCount() == 2, "commands and events");
    const std::uint64_t features = device.features();
    require((features >> VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES & 1) && (features >> VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG & 1) &&
                (features >> VIRTIO_VIDEO_F_V4L2_CONTROLS & 1) && !(features >> VIRTIO_VIDEO_F_RESOURCE_SHM_REGION & 1),
            "the features");
    const auto config = load<virtio_video_config>(device.configuration());
    require(config.version == UML_VIRTIO_VIDEO_VERSION && config.max_caps_length == 16384 && config.max_resp_length == 4096,
            "the configuration");
    require(cameraDeviceId == UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID, "the device ID matches the wire");
    require(device.cameras().size() == 2, "a tablet has two cameras");

    const auto query = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}}, 4096);
    require(query.action == VhostUserQueueAction::complete, "the query completes");
    const auto header = load<virtio_video_camera_query_resp>(query.response);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY && header.num_cameras == 2 && header.num_descs == 2 &&
                header.session_flags == 0 && header.next_index == 0,
            "the camera list");
    const auto back = load<virtio_video_camera_desc>(query.response, sizeof(header));
    require(back.index == 0 && back.facing == VIRTIO_VIDEO_CAMERA_FACING_BACK && (back.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS),
            "camera 0 is the back camera");
    // A short buffer pages the list.
    const auto paged = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}},
                                sizeof(header) + sizeof(virtio_video_camera_desc));
    require(load<virtio_video_camera_query_resp>(paged.response).num_descs == 1 &&
                load<virtio_video_camera_query_resp>(paged.response).next_index == 1,
            "a short buffer gets one record and the next index");
    rig.refused(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 9, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a start index past the end");
    rig.refused(virtio_video_get_params {{0x7777, 0}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "an unknown command on no stream");

    // Formats.
    const auto formats = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, videoKind});
    require(load<virtio_video_query_capability_resp>(formats.response).num_descs == 1 &&
                load<virtio_video_format_desc>(formats.response, sizeof(virtio_video_query_capability_resp)).format ==
                    VIRTIO_VIDEO_FORMAT_NV12,
            "video is NV12");
    const auto stills = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, stillKind});
    require(load<virtio_video_query_capability_resp>(stills.response).num_descs == 1, "an uncalibrated camera offers JPEG only");
    rig.refused(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, metaKind},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "formats of a metadata stream");
    rig.refused(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 5, videoKind},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "formats of a camera that is not there");

    // The controls, in pages, are the controls whole.
    std::vector<std::uint32_t> whole, paging;
    const auto walk = [&](std::size_t capacity, std::vector<std::uint32_t>& ids) {
        std::uint32_t start = 0;
        for (int guard = 0; guard < 100; ++guard) {
            const auto answer =
                rig.send(virtio_video_camera_query_controls {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS, 0}, 0, start}, capacity);
            require(Rig::type(answer) == VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, "the controls answer");
            const auto page = load<virtio_video_query_v4l2_controls_resp>(answer.response);
            for (std::uint32_t index = 0; index < page.num_descs; ++index)
                ids.push_back(load<virtio_video_v4l2_control_desc>(answer.response, sizeof(page) +
                                                                       index * sizeof(virtio_video_v4l2_control_desc)).id);
            require(answer.response.size() == sizeof(page) + page.num_descs * sizeof(virtio_video_v4l2_control_desc),
                    "exactly the records");
            if (page.next_index == 0) return;
            start = page.next_index;
        }
        require(false, "the control walk does not end");
    };
    walk(8192, whole);
    walk(sizeof(virtio_video_query_v4l2_controls_resp) + 3 * sizeof(virtio_video_v4l2_control_desc), paging);
    require(whole.size() > 20 && whole == paging, "pages add up to the whole list");
    require(std::set<std::uint32_t>(whole.begin(), whole.end()).size() == whole.size(), "no control twice");
    rig.refused(virtio_video_camera_query_controls {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS, 0}, 7, 0},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "controls of a camera that is not there");
    require(rig.camera->sessionsOpened() == 0, "questions open no capture session");
}

void testStreamsAndParameters()
{
    Rig rig;
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 0}, 0, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "stream 0");
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 9, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a camera that is not there");
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 0, 9, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a stream kind that is not one");
    rig.create(1, 0, videoKind);
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 0, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a stream ID in use");

    // A new video stream starts at 720p, 30 frames a second, rows at 16 bytes.
    auto p = rig.params(1);
    require(p.queue_type == output && p.format == VIRTIO_VIDEO_FORMAT_NV12 && p.frame_width == 1280 && p.frame_height == 720 &&
                p.frame_rate == 30 && p.num_planes == 1 && p.plane_formats[0].stride == 1280 &&
                p.plane_formats[0].plane_size == 1280 * 1080 && p.min_buffers == 1 && p.max_buffers == 4,
            "the default video format");
    rig.setVideo(1, 640, 480);
    p = rig.params(1);
    require(p.frame_width == 640 && p.frame_height == 480 && p.plane_formats[0].plane_size == 640 * 720, "640 x 480");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 3840, 2160)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "4K is above the small tier");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 641, 480)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an odd size");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_H264, 640, 480)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a coded format");
    require(rig.params(1).frame_width == 640, "a refused change changes nothing");

    // The interval, and the colour the frames carry.
    const auto info = rig.send(virtio_video_get_stream_info {{VIRTIO_VIDEO_CMD_GET_STREAM_INFO, 1}, output, {}});
    const auto stream = load<virtio_video_get_stream_info_resp>(info.response).info;
    require(Rig::type(info) == VIRTIO_VIDEO_RESP_OK_STREAM_INFO && stream.frame_interval_numerator == 1 &&
                stream.frame_interval_denominator == 30 && stream.colorspace == 3 && stream.xfer_func == 1 &&
                stream.ycbcr_enc == 2 && stream.quantization == 1,
            "BT.709 full range at 30 frames a second");
    require(Rig::type(rig.setInterval(1, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA && rig.params(1).frame_rate == 15, "15 frames a second");
    require(Rig::type(rig.setInterval(1, 1, 1000)) == VIRTIO_VIDEO_RESP_OK_NODATA && rig.params(1).frame_rate == 120,
            "a rate above the camera's is the highest any format that covers 640 x 480 runs at");
    require(Rig::type(rig.setInterval(1, 0, 30)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a zero interval changes nothing");

    // A buffer smaller than the format is refused; one the size of it is taken.
    std::vector<std::uint8_t> request;
    append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 1, output, 1});
    append(request, std::uint32_t(1));
    append(request, std::uint32_t(0));
    append(request, virtio_video_resource_sg_entry {page * 20, 4096, 0});
    require(Rig::type(rig.device->processQueue(0, 900, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a short buffer");
    rig.attach(1, 2, nv12Size(640, 480));
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 320, 240)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "the format cannot change with buffers allocated");

    // Still and metadata streams.
    rig.create(2, 0, stillKind);
    p = rig.params(2);
    require(p.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG && p.frame_width == 4032 && p.frame_height == 3024 &&
                p.plane_formats[0].plane_size == 4032u * 3024u * 3 / 2 + exifGapLength,
            "a still stream starts at the largest JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a JPEG size");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1000, 700)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a size the camera does not take stills at");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "RAW before the lens is calibrated");
    rig.create(3, 0, metaKind);
    p = rig.params(3);
    require(p.format == VIRTIO_VIDEO_FORMAT_CAMERA_META && p.plane_formats[0].plane_size == metaSize, "a metadata stream");
    // The front camera is a camera of its own.
    rig.create(4, 1, videoKind);
    require(Rig::type(rig.send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 99}, output, {}})) ==
                VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
            "an unknown stream");
    rig.destroy(1);
    rig.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "a destroyed stream");
    require(rig.camera->sessionsOpened() == 0, "no capture without a buffer");
}

void testVideoAndMetadata()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, metaKind);
    for (std::uint32_t resource = 1; resource <= 5; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    for (std::uint32_t resource = 1; resource <= 4; ++resource) rig.attach(2, resource, metaSize);
    std::vector<std::uint64_t> metas, frames;
    for (std::uint32_t resource = 1; resource <= 3; ++resource) metas.push_back(rig.queue(2, resource));
    require(rig.camera->sessionsOpened() == 0, "a metadata buffer alone starts no capture");
    for (std::uint32_t resource = 1; resource <= 3; ++resource) frames.push_back(rig.queue(1, resource));
    require(rig.camera->sessionsOpened() == 1 && rig.camera->videoRunning(0) && rig.camera->videoFrameRate(0) == 30,
            "the first video buffer starts the camera at the stream's rate");
    require(rig.states == std::vector<CameraState> {CameraState::streaming}, "the host is streaming");

    rig.camera->pump(0, 3);
    rig.step();
    const std::uint64_t interval = 1'000'000'000ull / 30;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto frame = rig.result(frames[index]);
        require(frame.base.flags == 0 && frame.base.data_sizes[0] == nv12Size(640, 480), "a frame fills its buffer");
        require(frame.sequence == index + 1, "frames are numbered from 1 in capture order");
        require(frame.base.timestamp == (3 - index) * interval, "the timestamp is the frame's age");
        const auto picture = rig.bytes(1, std::uint32_t(index + 1), nv12Size(640, 480));
        const auto marker = decodeChartMarker(picture.data(), 640, 640, 480);
        require(marker.valid && marker.tag == chartTag(0) && marker.counter == index + 1, "the buffer holds that frame's picture");

        const auto record = rig.result(metas[index]);
        require(record.base.flags == 0 && record.base.data_sizes[0] == metaSize && record.sequence == index + 1 &&
                    record.base.timestamp == frame.base.timestamp,
                "the metadata completes with its frame");
        const auto meta = metaOf(rig, 2, std::uint32_t(index + 1));
        require(meta.sequence == index + 1 && meta.flags == 0 && meta.active_camera == 0 && meta.frame_duration_ns > 0 &&
                    meta.exposure_ns > 0 && meta.iso >= 25 && meta.zoom_ratio_q16 == 65536 && meta.focal_length_um > 0 &&
                    meta.jpeg_gap_offset == 0 && meta.jpeg_gap_length == 0,
                "the record states what the exposure did");
    }

    // A frame with no buffer is dropped and its number is spent; the record is kept for the next metadata buffer.
    rig.camera->pump(0, 1);
    rig.step();
    const auto lateMeta = rig.queue(2, 4);
    require(rig.done(lateMeta) && rig.result(lateMeta).sequence == 4, "the record of the dropped frame is served from the backlog");
    const auto fifth = rig.queue(1, 4);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.result(fifth).sequence == 5, "and the next frame is number 5");

    // Clearing the queue returns what is queued, with an error, and releases the capture.
    const auto queued = rig.queue(1, 5);
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    rig.collect();
    require(rig.done(queued) && (rig.result(queued).base.flags & errorFlag), "a cleared buffer completes with an error");
    require(!rig.camera->videoRunning(0) && rig.camera->sessionsOpen() == 0, "with no stream left the capture is released");
    require(rig.states == std::vector<CameraState> {CameraState::streaming, CameraState::idle}, "the host is idle again");
    rig.destroy(1);
    rig.destroy(2);
}

void testDecimation()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.create(2, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.setVideo(2, 640, 480);
    require(Rig::type(rig.setInterval(2, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA, "15 frames a second");
    for (std::uint32_t resource = 1; resource <= 6; ++resource) {
        rig.attach(1, resource, nv12Size(640, 480));
        rig.attach(2, resource, nv12Size(640, 480));
    }
    std::vector<std::uint64_t> fast, slow;
    for (std::uint32_t resource = 1; resource <= 6; ++resource) fast.push_back(rig.queue(1, resource));
    for (std::uint32_t resource = 1; resource <= 3; ++resource) slow.push_back(rig.queue(2, resource));
    require(rig.camera->videoFrameRate(0) == 30, "the camera runs at the faster stream's rate");
    for (int frame = 0; frame < 6; ++frame) {
        rig.camera->pump(0, 1);
        rig.step();
    }
    for (std::size_t index = 0; index < 6; ++index) require(rig.result(fast[index]).sequence == index + 1, "the fast stream has every frame");
    require(rig.result(slow[0]).sequence == 1 && rig.result(slow[1]).sequence == 3 && rig.result(slow[2]).sequence == 5,
            "the slow stream has every other frame");
}

void testPipeline()
{
    struct Case {
        std::uint32_t width, height, control, degrees;
        const char* what;
    };
    const Case cases[] = {
        {640, 480, 0, 0, "native size"},
        {320, 240, 0, 0, "scaled down"},
        {176, 144, 0, 0, "scaled to a small size"},
        {640, 360, 0, 0, "cropped to 16:9"},
        {640, 480, 2, 180, "turned 180 degrees"},
        {480, 640, 1, 90, "turned 90 degrees, as a portrait"},
        {480, 640, 3, 270, "turned 270 degrees, as a portrait"},
    };
    for (const auto& test : cases) {
        Rig rig;
        Recorder recorder;
        auto reference = rig.camera->openSession(recorder);
        require(reference != nullptr, test.what);
        rig.create(1, 0, videoKind);
        rig.setVideo(1, test.width, test.height);
        if (test.control != 0) rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, std::int64_t(test.control), {}}});
        rig.attach(1, 1, nv12Size(test.width, test.height));
        rig.attach(1, 2, nv12Size(test.width, test.height));
        const auto first = rig.queue(1, 1);
        const auto second = rig.queue(1, 2);
        require(reference->startVideo(0, test.width, test.height, 30), test.what);
        rig.camera->pump(0, 2);
        rig.step();
        require(recorder.frames.size() == 2, test.what);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto answer = rig.result(index == 0 ? first : second);
            require(answer.base.flags == 0 && answer.base.data_sizes[0] == nv12Size(test.width, test.height), test.what);
            const auto wanted = expectedNv12(recorder.frames[index], test.width, test.height, test.degrees);
            require(rig.bytes(1, std::uint32_t(index + 1), wanted.size()) == wanted, test.what);
        }
        reference->stop();
    }

    // The automatic setting follows the application's rotation, picked up at the next frame.
    Rig rig;
    std::uint32_t degrees = 90;
    rig.automaticDegrees = &degrees;
    Recorder recorder;
    auto reference = rig.camera->openSession(recorder);
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 480, 640);
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, VIRTIO_VIDEO_CAMERA_ROTATE_AUTOMATIC, {}}});
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(1, resource, nv12Size(480, 640));
    rig.queue(1, 1);
    rig.queue(1, 2);
    require(reference->startVideo(0, 480, 640, 30), "reference");
    rig.camera->pump(0, 1);
    rig.step();
    degrees = 270;
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.bytes(1, 1, nv12Size(480, 640)) == expectedNv12(recorder.frames[0], 480, 640, 90), "automatic: 90 degrees");
    require(rig.bytes(1, 2, nv12Size(480, 640)) == expectedNv12(recorder.frames[1], 480, 640, 270), "automatic: now 270 degrees");
    reference->stop();
}

void testBlockedNeverOpensASession()
{
    RigOptions options;
    options.mode = CameraMode::blocked;
    Rig rig(options);
    const auto query = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}}, 4096);
    const auto header = load<virtio_video_camera_query_resp>(query.response);
    require(header.num_cameras == 2 && (header.session_flags & VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED),
            "a blocked session still publishes the cameras it found, and says it is blocked");

    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, metaKind);
    rig.create(3, 0, stillKind);
    require(Rig::type(rig.setParams(3, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    for (std::uint32_t resource = 1; resource <= 3; ++resource) {
        rig.attach(1, resource, nv12Size(640, 480));
        rig.attach(2, resource, metaSize);
    }
    rig.attach(3, 1, std::uint32_t(rig.params(3).plane_formats[0].plane_size));

    std::vector<std::uint64_t> metas, frames;
    for (std::uint32_t resource = 1; resource <= 3; ++resource) metas.push_back(rig.queue(2, resource));
    for (std::uint32_t resource = 1; resource <= 3; ++resource) frames.push_back(rig.queue(1, resource));
    require(rig.waitFor([&] { return std::all_of(frames.begin(), frames.end(), [&](auto id) { return rig.done(id); }); }),
            "black frames flow on the device's own timer");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto frame = rig.result(frames[index]);
        require(frame.base.flags == (blackFlag | blockedFlag), "a blocked frame is black and blocked");
        require(frame.base.data_sizes[0] == nv12Size(640, 480) && frame.base.timestamp == 0, "a whole picture, no age");
        const auto picture = rig.bytes(1, std::uint32_t(index + 1), nv12Size(640, 480));
        require(std::all_of(picture.begin(), picture.begin() + 640 * 480, [](auto byte) { return byte == 0; }) &&
                    std::all_of(picture.begin() + 640 * 480, picture.end(), [](auto byte) { return byte == 128; }),
                "the picture is black");
    }
    require(rig.waitFor([&] { return std::all_of(metas.begin(), metas.end(), [&](auto id) { return rig.done(id); }); }),
            "the metadata follows");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto record = rig.result(metas[index]);
        const auto meta = metaOf(rig, 2, std::uint32_t(index + 1));
        require(record.sequence == meta.sequence && meta.sequence >= 1 &&
                    meta.flags == (VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK | VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED),
                "metadata says black and blocked");
    }
    // A still is refused at once.
    const auto still = rig.queue(3, 1, 4321);
    require(rig.done(still), "a still in a blocked session completes at once");
    const auto refusal = rig.result(still);
    require(refusal.base.flags == (errorFlag | blackFlag | blockedFlag) && refusal.base.timestamp == 4321, "with an error and its cookie");

    // Controls are accepted and kept, and reach no platform session.
    rig.setOk(1, {{UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 5'000'000, {}},
                  {UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    require(rig.get(1, {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, UML_V4L2_CID_PRIVACY}) == std::vector<std::int64_t> {5'000'000, 1},
            "the values are kept, and privacy reads as on");
    require(rig.set(1, {{UML_V4L2_CID_PRIVACY, 0, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "privacy is read-only");

    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    rig.destroy(1);
    rig.destroy(2);
    rig.destroy(3);
    rig.device->reset();
    require(rig.camera->sessionsOpened() == 0 && rig.camera->sessionsOpen() == 0,
            "a blocked session never opened a platform capture session");
}

void testInterruption()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 6; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    // The guest keeps event buffers queued.
    std::vector<std::uint64_t> eventBuffers;
    for (int index = 0; index < 8; ++index) {
        const auto id = rig.next++;
        const auto result = rig.device->processQueue(1, id, {}, sizeof(virtio_video_event));
        require(result.action == VhostUserQueueAction::defer, "an event buffer waits");
        eventBuffers.push_back(id);
    }
    const auto eventOf = [&](std::size_t index) {
        require(rig.done(eventBuffers[index]), "an event did not arrive");
        return load<virtio_video_event>(rig.completed.at(eventBuffers[index]));
    };
    const auto a = rig.queue(1, 1), b = rig.queue(1, 2);
    rig.camera->pump(0, 2);
    rig.step();
    require(rig.result(a).base.flags == 0 && rig.result(b).base.flags == 0, "frames flow");

    rig.camera->interrupt();
    rig.step();
    // The platform interrupts every camera, and each is an event, named by its camera index.
    require(eventOf(0).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED && eventOf(0).stream_id == 0 &&
                eventOf(1).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED && eventOf(1).stream_id == 1,
            "the interruption is an event for each camera");
    require(rig.states.back() == CameraState::interrupted, "the application is told");
    const auto c = rig.queue(1, 3), d = rig.queue(1, 4);
    require(rig.waitFor([&] { return rig.done(c) && rig.done(d); }), "black frames bridge the interruption");
    for (const auto id : {c, d}) {
        require(rig.result(id).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
        const auto picture = rig.bytes(1, id == c ? 3u : 4u, 640 * 480);
        require(std::all_of(picture.begin(), picture.end(), [](auto byte) { return byte == 0; }), "black");
    }
    rig.camera->pump(0, 2);
    rig.step();

    rig.camera->resume();
    rig.step();
    require(eventOf(2).event_type == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED && eventOf(2).stream_id == 0 &&
                eventOf(3).event_type == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED && eventOf(3).stream_id == 1,
            "and so is the resumption");
    require(rig.states.back() == CameraState::streaming, "streaming again");
    const auto e = rig.queue(1, 5);
    require(rig.waitFor([&] { return rig.done(e); }), "black frames flag the restart");
    require(rig.result(e).base.flags == (blackFlag | resumingFlag), "black and resuming");
    // The first real frame ends it.
    const auto f = rig.queue(1, 6);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(f) && rig.result(f).base.flags == 0, "a captured frame ends the restart");
    require(rig.result(f).sequence > rig.result(e).sequence, "numbers keep counting");

    // Multitasking has its own state.
    rig.camera->interrupt(CameraEventKind::interruptedByMultitasking);
    rig.step();
    require(rig.states.back() == CameraState::interruptedByMultitasking, "the multitasking interruption");
    require(eventOf(4).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, "is an interruption to the guest");
    rig.camera->resume();
    rig.step();
    require(rig.states.back() == CameraState::streaming, "resumed");
}

void testAccessDenied()
{
    RigOptions options;
    options.authorized = false;
    Rig rig(options);
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, stillKind);
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(2, 1, std::uint32_t(rig.params(2).plane_formats[0].plane_size));
    const auto frame = rig.queue(1, 1);
    require(rig.waitFor([&] { return rig.done(frame); }), "a guest of a host that has not granted access still gets frames");
    require(rig.result(frame).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
    require(rig.states.back() == CameraState::interrupted, "and the application is told");
    const auto still = rig.queue(2, 1, 8);
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag) && (rig.result(still).base.flags & interruptedFlag),
            "a still is refused");
    require(rig.camera->sessionsOpen() == 0, "nothing is left open");
}

/* Reads the little-endian 16-bit samples of a RAW row. */
std::vector<std::uint16_t> rawRow(Rig& rig, std::uint32_t stream, std::uint32_t resource, std::uint32_t width, std::uint32_t row)
{
    const auto stride = rawPitch(2 * width);
    const auto data = rig.bytes(stream, resource, std::size_t(stride) * (row + 1));
    std::vector<std::uint16_t> samples(width);
    std::memcpy(samples.data(), data.data() + std::size_t(stride) * row, std::size_t(width) * 2);
    return samples;
}

void testStillsAndCalibration()
{
    auto store = std::make_shared<CalibrationStore>();
    std::string model;
    CameraInfo info;
    {
        RigOptions options;
        options.store = store;
        Rig rig(options);
        model = rig.camera->hostModel();
        info = rig.camera->cameras()[0];
        require(!rig.device->cameras()[0].raw(), "no RAW until the lens is calibrated");
        rig.create(1, 0, stillKind);
        require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        const auto bufferSize = rig.params(1).plane_formats[0].plane_size;
        require(bufferSize == 1008u * 756u * 3 / 2 + exifGapLength, "the buffer holds a YUV-sized JPEG and the gap");
        rig.create(2, 0, metaKind);
        rig.attach(1, 1, bufferSize);
        rig.attach(2, 1, metaSize);
        const auto meta = rig.queue(2, 1);
        const auto still = rig.queue(1, 1, 555);
        require(!rig.done(still) && rig.camera->sessionsOpened() == 1, "the capture is asked for when the buffer is queued");
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(still), "the still completed");
        const auto answer = rig.result(still);
        require(answer.base.flags == 0 && answer.base.timestamp == 555 && answer.sequence == 1 && answer.base.data_sizes[0] > 4000 &&
                    answer.base.data_sizes[0] < bufferSize,
                "a still completes with the guest's cookie and its size");
        const auto file = rig.bytes(1, 1, answer.base.data_sizes[0]);
        require(file[0] == 0xff && file[1] == 0xd8 && file[2] == 0xff && file[3] == 0xe1 && file[4] == 0xfd && file[5] == 0xea,
                "a JPEG that starts with the reserved Exif segment (length 65002)");
        require(std::memcmp(file.data() + 6, "Exif\0\0", 6) == 0, "which holds an Exif header");
        require(file[file.size() - 2] == 0xff && file[file.size() - 1] == 0xd9, "and ends with the end of image");
        require(rig.done(meta), "the still's metadata");
        const auto record = metaOf(rig, 2, 1);
        require(rig.result(meta).sequence == 1 && record.sequence == 1 && record.flags == VIRTIO_VIDEO_CAMERA_FRAME_F_STILL &&
                    record.jpeg_gap_offset == 6 && record.jpeg_gap_length == exifGapLength && record.active_camera == 0,
                "describes the still and where its gap is");
        require(rig.camera->sessionsOpen() == 0 && rig.states.back() == CameraState::idle, "the capture session is released");

        // The first still of a lens taught the device its colour data.
        const auto learned = store->load(model, CalibrationStore::lensKey(info));
        require(learned && learned->bayer == BayerOrder::rggb && learned->rawWidth == 1008 && learned->rawHeight == 756 &&
                    learned->whiteLevel > learned->blackLevel,
                "the lens is calibrated");
        require(!rig.device->cameras()[0].raw(), "but this guest keeps seeing what it saw at the start");
    }

    // A later guest sees RAW, and takes it with a JPEG from one capture.
    RigOptions options;
    options.store = store;
    Rig rig(options);
    const auto camera = rig.device->cameras()[0];
    require(camera.raw() && (camera.wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) && camera.wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_RGGB &&
                camera.wire.raw_width == 1008,
            "RAW is offered once the lens is known");
    const auto formats = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, stillKind});
    require(load<virtio_video_query_capability_resp>(formats.response).num_descs == 2, "JPEG and RAW");
    rig.create(1, 0, stillKind);
    rig.create(2, 0, stillKind);
    rig.create(3, 0, metaKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "RAW");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG, 1008, 756)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "another pattern is refused");
    const auto raw = rig.params(2);
    require(raw.plane_formats[0].stride == rawPitch(2 * 1008) && raw.plane_formats[0].plane_size == rawPitch(2 * 1008) * 756,
            "RAW rows are padded to 16 bytes");
    rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
    rig.attach(2, 1, raw.plane_formats[0].plane_size);
    rig.attach(2, 2, raw.plane_formats[0].plane_size);
    rig.attach(1, 2, rig.params(1).plane_formats[0].plane_size);
    rig.attach(3, 1, metaSize);
    const auto meta = rig.queue(3, 1);

    const auto rawBuffer = rig.queue(2, 1, 77, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    rig.camera->pump(0, 0);
    rig.step();
    require(!rig.done(rawBuffer), "a group waits for its last buffer");
    const auto jpegBuffer = rig.queue(1, 1, 77);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(rawBuffer) && rig.done(jpegBuffer), "the last buffer starts one capture that fills both");
    const auto rawAnswer = rig.result(rawBuffer), jpegAnswer = rig.result(jpegBuffer);
    require(rawAnswer.base.flags == 0 && jpegAnswer.base.flags == 0 && rawAnswer.base.timestamp == 77 &&
                jpegAnswer.base.timestamp == 77 && rawAnswer.sequence == jpegAnswer.sequence,
            "with the cookie and one exposure");
    require(rawAnswer.base.data_sizes[0] == rawPitch(2 * 1008) * 756, "a whole RAW picture");
    std::uint64_t sum = 0;
    std::uint16_t largest = 0;
    for (const std::uint32_t row : {0u, 1u, 377u, 755u})
        for (const auto sample : rawRow(rig, 2, 1, 1008, row)) {
            sum += sample;
            largest = std::max(largest, sample);
        }
    require(sum > 0 && largest <= camera.calibration->whiteLevel, "RAW samples in range");
    const auto record = metaOf(rig, 3, 1);
    require(rig.done(meta) && record.sequence == rawAnswer.sequence && record.flags == VIRTIO_VIDEO_CAMERA_FRAME_F_STILL &&
                record.jpeg_gap_offset == 6 && record.neutral_point_q16[1] != 0 && record.noise_profile_f32[0] != 0,
            "one record for the capture, with the white point and noise model of a RAW");

    // RAW alone, without a cookie.
    const auto alone = rig.queue(2, 2, 0);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(alone) && rig.result(alone).base.flags == 0, "RAW alone");

    // A group left unfinished is failed after two seconds.
    const auto stuck = rig.queue(2, 1, 99, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    rig.step();
    require(!rig.done(stuck), "waiting");
    rig.camera->advanceClock(1'500'000'000);
    rig.step();
    require(!rig.done(stuck), "not yet");
    rig.camera->advanceClock(600'000'000);
    rig.step();
    require(rig.done(stuck) && (rig.result(stuck).base.flags & errorFlag) && rig.result(stuck).base.timestamp == 99,
            "failed with its cookie");

    // A capture the platform cannot finish fails its buffers.
    const auto doomed = rig.queue(1, 2, 31);
    rig.camera->interrupt();
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(doomed) && (rig.result(doomed).base.flags & errorFlag), "an interrupted capture fails");
    const auto refused = rig.queue(1, 2, 32);
    require(rig.done(refused) && rig.result(refused).base.flags == (errorFlag | interruptedFlag), "and a new one is refused while it lasts");
    rig.camera->resume();
    rig.step();
    require(rig.camera->sessionsOpen() == 0, "no session is held for stills");
}

void testControlsOnStreams()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.queue(1, 1);

    rig.setOk(1, {{UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 5'000'000, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_ISO, 800, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 2 * 65536, {}},
                  {UML_V4L2_CID_FOCUS_AUTO, 0, {}},
                  {UML_V4L2_CID_FOCUS_ABSOLUTE, 2000, {}}});
    auto applied = rig.camera->appliedSettings(0);
    require(applied.exposureMode == CameraExposureMode::manual && applied.exposureNanoseconds == 5'000'000 && applied.iso == 800 &&
                applied.zoomRatio == 2.0f && applied.focusMode == CameraFocusMode::manual && applied.focusDistanceDioptres == 2.0f,
            "the platform camera is told, as soon as the set completes");
    require(rig.get(1, {UML_V4L2_CID_EXPOSURE_ABSOLUTE, UML_V4L2_CID_ISO_SENSITIVITY, UML_V4L2_CID_ZOOM_ABSOLUTE}) ==
                std::vector<std::int64_t> {50, 800, 200},
            "and the aliases read back");

    // A set applies every value or none.
    require(rig.set(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 3 * 65536, {}}, {VIRTIO_VIDEO_CAMERA_CID_ISO, 5, {}}}) ==
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a value out of range fails the set");
    require(rig.get(1, {UML_V4L2_CID_ZOOM_ABSOLUTE})[0] == 200 && rig.camera->appliedSettings(0).zoomRatio == 2.0f,
            "and the valid value before it was not applied");
    const auto bad = rig.controls(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1,
                                  {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 3 * 65536, {}}, {VIRTIO_VIDEO_CAMERA_CID_ISO, 5, {}}});
    const auto failure = load<virtio_video_v4l2_controls_resp>(bad.response);
    require(failure.error_index == 1 && failure.num_controls == 2, "the failing index is reported");
    require(rig.set(1, {{UML_V4L2_CID_PRIVACY, 0, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a read-only control");
    require(rig.set(1, {{UML_V4L2_CID_MPEG_VIDEO_BITRATE, 1, {}}}) == VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, "an encoder control");
    require(rig.set(1, {{UML_V4L2_CID_FOCUS_ABSOLUTE, 99999, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "beyond the nearest focus");

    // Compound controls keep the guest's bytes; the metering region reaches the platform.
    std::vector<std::uint8_t> region(regionPayloadSize);
    const std::uint32_t words[5] = {16384, 16384, 32768, 32768, 7};
    std::memcpy(region.data(), words, sizeof(words));
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region}});
    applied = rig.camera->appliedSettings(0);
    require(applied.aeRegion && applied.aeRegion->weight == 7 && applied.aeRegion->width == 0.5f, "the region");
    const auto read = rig.controls(VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, 1, {{VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, {}}});
    const auto value = load<virtio_video_v4l2_control_value>(read.response, sizeof(virtio_video_v4l2_controls_resp));
    require(value.size == regionPayloadSize && std::vector<std::uint8_t>(read.response.begin() +
                                                                          std::ptrdiff_t(sizeof(virtio_video_v4l2_controls_resp) +
                                                                                         sizeof(value) + value.value),
                                                                      read.response.begin() +
                                                                          std::ptrdiff_t(sizeof(virtio_video_v4l2_controls_resp) +
                                                                                         sizeof(value) + value.value +
                                                                                         regionPayloadSize)) == region,
            "and reads back");

    // A button counts presses; the focus status follows the exposure.
    for (int press = 0; press < 3; ++press) rig.setOk(1, {{UML_V4L2_CID_AUTO_FOCUS_START, 1, {}}});
    require(rig.camera->appliedSettings(0).focusTriggerSerial == 3, "three presses of the focus button");
    rig.setOk(1, {{UML_V4L2_CID_FOCUS_AUTO, 1, {}}});
    rig.camera->pump(0, 1);
    rig.step();
    const auto status = rig.get(1, {UML_V4L2_CID_AUTO_FOCUS_STATUS})[0];
    require(status == UML_V4L2_AUTO_FOCUS_STATUS_REACHED || status == UML_V4L2_AUTO_FOCUS_STATUS_BUSY ||
                status == UML_V4L2_AUTO_FOCUS_STATUS_IDLE,
            "the focus status is the camera's");

    // Controls belong to the camera: a metadata stream of it reads them.
    rig.create(2, 0, metaKind);
    require(rig.get(2, {UML_V4L2_CID_ISO_SENSITIVITY})[0] == 800, "any stream of the camera reaches its controls");
    rig.create(3, 1, videoKind);
    require(rig.get(3, {UML_V4L2_CID_ISO_SENSITIVITY})[0] != 800, "another camera has its own");

    // The test pattern makes the frames black, and ends.
    rig.setOk(1, {{UML_V4L2_CID_TEST_PATTERN, 1, {}}});
    const auto patterned = rig.queue(1, 1);
    require(rig.waitFor([&] { return rig.done(patterned); }), "the pattern is delivered on the timer");
    require(rig.result(patterned).base.flags == blackFlag, "a plain black frame");
    rig.setOk(1, {{UML_V4L2_CID_TEST_PATTERN, 0, {}}});
    const auto after = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(after) && rig.result(after).base.flags == 0, "and real frames return");
}

void testTorchHoldsTheSession()
{
    Rig rig;
    rig.create(1, 0, metaKind);
    require(rig.camera->sessionsOpened() == 0, "a stream with no buffer opens nothing");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    require(rig.camera->sessionsOpen() == 1 && rig.camera->appliedSettings(0).torch, "the torch needs a session");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_NONE, {}}});
    require(rig.camera->sessionsOpen() == 0, "which is released when the torch goes out");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    rig.device->reset();
    require(rig.camera->sessionsOpen() == 0, "and a reset releases it");
}

void testResetAndDestroy()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.setOk(1, {{UML_V4L2_CID_ISO_SENSITIVITY, 400, {}}});
    const auto frame = rig.queue(1, 1);
    require(rig.camera->videoRunning(0) && rig.states.back() == CameraState::streaming, "running");
    rig.device->reset();
    require(rig.camera->sessionsOpen() == 0 && !rig.camera->videoRunning(0), "a reset stops the capture");
    require(rig.states.back() == CameraState::idle, "and tells the application");
    rig.collect();
    require(!rig.done(frame), "a buffer of the old guest is not completed to the new one");
    rig.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "streams are gone");
    rig.create(1, 0, videoKind);
    require(rig.get(1, {UML_V4L2_CID_ISO_SENSITIVITY})[0] != 400, "and so are the controls' values");
    rig.device->reset();
    rig.device->reset();
}

void testService()
{
    SyntheticCameraOptions options;
    options.profile = SyntheticCameraProfile::laptop;
    std::shared_ptr<Camera> camera = makeSyntheticCamera(options);
    CameraDeviceOptions device;
    device.mode = CameraMode::blocked;
    auto service = makeInProcessCameraService(camera, device);
    require(service != nullptr && !service->takeConnectionName().empty(), "the service names its connection");
    service->stop();
    service->stop();
    require(std::dynamic_pointer_cast<SyntheticCamera>(camera)->sessionsOpened() == 0, "a blocked service opens no session");
}

} // namespace

int main()
{
    testHandshake();
    testStreamsAndParameters();
    testVideoAndMetadata();
    testDecimation();
    testPipeline();
    testBlockedNeverOpensASession();
    testInterruption();
    testAccessDenied();
    testStillsAndCalibration();
    testControlsOnStreams();
    testTorchHoldsTheSession();
    testResetAndDestroy();
    testService();
    return 0;
}
