// SPDX-License-Identifier: GPL-2.0-or-later
// How the host's cameras become the wire's: their order, which run together,
// what the tier and the calibration allow, and the pages of the answers.

#include "calibration_store.hpp"
#include "camera_description.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace uml::camera;
using namespace uml::platform;
using uml::testing::load;
using uml::testing::require;

namespace {

struct Host {
    std::unique_ptr<SyntheticCamera> camera;
    std::vector<CameraInfo> infos;
    std::vector<std::vector<std::size_t>> sets;
    std::string model;
    explicit Host(SyntheticCameraProfile profile)
    {
        SyntheticCameraOptions options;
        options.profile = profile;
        camera = makeSyntheticCamera(options);
        infos = camera->cameras();
        sets = camera->concurrentSets();
        model = camera->hostModel();
    }
    std::vector<CameraDescription> arrange(CameraTier tier, const CalibrationStore& store = CalibrationStore(),
                                           std::function<std::uint32_t(const CameraInfo&)> orientation = {}) const
    {
        ArrangementOptions options;
        options.tier = tier;
        options.sensorOrientation = std::move(orientation);
        return arrangeCameras(infos, sets, model, store, options);
    }
};

bool bit(std::uint32_t mask, std::uint32_t index) { return (mask >> index & 1u) != 0; }

CameraColorData colorOf(std::uint32_t width, std::uint32_t height)
{
    CameraColorData data;
    data.bayer = BayerOrder::grbg;
    data.blackLevel = 64;
    data.whiteLevel = 1023;
    data.rawWidth = width;
    data.rawHeight = height;
    data.illuminant1 = 17;
    data.illuminant2 = 21;
    data.colorMatrix1 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    data.focalLengthMillimetres = 6.9;
    data.intrinsics = {3000, 3000, 1008, 756};
    return data;
}

void testPhoneOrder()
{
    const Host host(SyntheticCameraProfile::phone);
    const auto cameras = host.arrange(CameraTier::small);
    require(cameras.size() == 5, "five cameras");
    // The zoom camera, the front camera, then the lenses.
    require(cameras[0].info.lens == CameraLensKind::zoom && cameras[0].wire.lens == VIRTIO_VIDEO_CAMERA_LENS_ZOOM &&
                cameras[0].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_BACK,
            "camera 0 is the back zoom camera");
    require(cameras[1].info.position == CameraPosition::front && cameras[1].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_FRONT,
            "camera 1 is the front camera");
    require(cameras[2].info.lens == CameraLensKind::wide && cameras[3].info.lens == CameraLensKind::ultraWide &&
                cameras[4].info.lens == CameraLensKind::telephoto,
            "then the wide, ultra wide and telephoto lenses");
    for (std::uint32_t index = 0; index < cameras.size(); ++index)
        require(cameras[index].index == index && cameras[index].wire.index == index, "the index is the position");
    require(cameras[0].platformIndex == 0 && cameras[1].platformIndex == 4 && cameras[2].platformIndex == 1,
            "the platform's index is kept");

    // The zoom camera conflicts with the lenses it is made of, and they with it.
    require(cameras[0].wire.constituent_mask == (1u << 2 | 1u << 3 | 1u << 4), "the zoom camera's lenses");
    for (const auto lens : {2u, 3u, 4u}) {
        require(bit(cameras[0].wire.conflict_mask, lens) && bit(cameras[lens].wire.conflict_mask, 0), "a conflict is mutual");
        require(!bit(cameras[0].wire.concurrent_mask, lens), "and not also concurrent");
        require(cameras[lens].wire.constituent_mask == 0, "a lens has no constituents");
    }
    // The front camera runs with each back camera; the lenses do not run together.
    for (const auto other : {0u, 2u, 3u, 4u})
        require(bit(cameras[1].wire.concurrent_mask, other) && bit(cameras[other].wire.concurrent_mask, 1),
                "the front camera runs with a back camera");
    require(bit(cameras[2].wire.conflict_mask, 3) && bit(cameras[3].wire.conflict_mask, 2), "two back lenses conflict");
    for (const auto& camera : cameras)
        require((camera.wire.conflict_mask & camera.wire.concurrent_mask) == 0 && !bit(camera.wire.conflict_mask, camera.index) &&
                    !bit(camera.wire.concurrent_mask, camera.index),
                "a camera is not its own neighbour, and a pair is one or the other");

    // The flags follow the camera.
    require((cameras[0].wire.flags & VIRTIO_VIDEO_CAMERA_F_ZOOM) && !(cameras[0].wire.flags & VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE),
            "the zoom camera zooms and has no manual exposure");
    require(cameras[2].wire.flags & VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE, "a lens has it");
    require(!(cameras[1].wire.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS) && (cameras[2].wire.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS),
            "the front camera does not focus");
    require(cameras[0].wire.num_switch_overs == 2 && cameras[0].wire.switch_over_q16[0] == 65536 &&
                cameras[0].wire.switch_over_q16[1] == 3 * 65536,
            "the switch-over ratios");
    require(cameras[0].wire.zoom_min_q16 == 65536 / 2 && cameras[0].wire.zoom_max_q16 == 15 * 65536, "the zoom range");
    require(cameras[0].wire.sensor_orientation == 90 && cameras[1].wire.sensor_orientation == 270, "the sensor orientation");
    require(cameras[2].wire.focal_length_um > 0 && cameras[2].wire.sensor_width_um > 0, "the optics are stated");
    // A wider field of view gives a shorter focal length on the same sensor size.
    require(cameras[3].wire.focal_length_um < cameras[2].wire.focal_length_um &&
                cameras[4].wire.focal_length_um > cameras[2].wire.focal_length_um,
            "ultra wide is shorter, telephoto longer");
}

void testOtherHosts()
{
    const Host tablet(SyntheticCameraProfile::tablet);
    const auto cameras = tablet.arrange(CameraTier::small);
    require(cameras.size() == 2 && cameras[0].info.position == CameraPosition::back &&
                cameras[0].wire.lens == VIRTIO_VIDEO_CAMERA_LENS_WIDE && cameras[1].info.position == CameraPosition::front,
            "a tablet's back wide camera is camera 0, the front camera 1");
    require(bit(cameras[0].wire.concurrent_mask, 1) && bit(cameras[1].wire.concurrent_mask, 0), "they run together");
    require(cameras[0].wire.constituent_mask == 0 && cameras[0].wire.conflict_mask == 0, "and nothing conflicts");

    const Host laptop(SyntheticCameraProfile::laptop);
    const auto only = laptop.arrange(CameraTier::large);
    require(only.size() == 1 && only[0].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_FRONT, "a laptop has one front camera");
    require(only[0].wire.concurrent_mask == 0 && only[0].wire.conflict_mask == 0, "alone");

    // The application states the sensor orientation when it knows better.
    const auto stated = tablet.arrange(CameraTier::small, CalibrationStore(), [](const CameraInfo& info) {
        return info.position == CameraPosition::front ? 180u : 0u;
    });
    require(stated[0].sensorOrientation == 0 && stated[0].wire.sensor_orientation == 0 && stated[1].wire.sensor_orientation == 180,
            "the application's orientation");

    // No cameras at all is a valid, empty device.
    require(arrangeCameras({}, {}, "none", CalibrationStore(), {}).empty(), "no cameras");

    // More than the wire holds are cut off, back cameras first.
    std::vector<CameraInfo> many;
    CameraInfo wide = tablet.infos[0];
    many.push_back(wide);
    for (int index = 0; index < 12; ++index) {
        CameraInfo external = tablet.infos[1];
        external.identifier = "external-" + std::to_string(index);
        external.position = CameraPosition::external;
        external.lens = CameraLensKind::external;
        many.push_back(external);
    }
    const auto capped = arrangeCameras(many, {}, "many", CalibrationStore(), {});
    require(capped.size() == VIRTIO_VIDEO_CAMERA_MAX_CAMERAS && capped[0].info.position == CameraPosition::back &&
                capped[1].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL,
            "at most eight cameras, the back one first");
}

void testTiers()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto small = phone.arrange(CameraTier::small);
    const auto large = phone.arrange(CameraTier::large);
    // The wide lens takes 48 MP stills only in the large tier.
    require(small[2].jpegSizes.front() == CameraStillSize {4032, 3024}, "12 MP at most in the small tier");
    require(large[2].jpegSizes.front() == CameraStillSize {8064, 6048}, "48 MP in the large tier");
    const auto largest = [](const CameraDescription& camera) {
        std::uint32_t width = 0, height = 0;
        for (const auto& mode : camera.video) {
            width = std::max(width, mode.width);
            height = std::max(height, mode.height);
        }
        return std::make_pair(width, height);
    };
    require(largest(small[2]) == std::make_pair(1920u, 1080u), "1080p video at most in the small tier");
    require(largest(large[2]) == std::make_pair(3840u, 2160u), "4K video in the large tier");
    // Every host is given the small tier for now, whatever its GPU budget: the cap is one constant, and the
    // tier the budget would earn is still the large one at 1 GiB.
    require(largestOfferedTier == CameraTier::small, "the cap");
    for (const std::uint64_t budget : {std::uint64_t {0}, (1ull << 30) - 1, 1ull << 30, 4ull << 30, ~std::uint64_t {0}})
        require(cameraTierForBudget(budget) == CameraTier::small, "every host composes the small tier");
    require(limitsOf(CameraTier::small).buffers == 4 && limitsOf(CameraTier::large).buffers == 6, "buffers");

    // The sizes include the usual ones the device scales to, never above the camera's own.
    const auto& modes = small[2].video;
    for (const auto size : {std::make_pair(1280u, 720u), std::make_pair(640u, 480u), std::make_pair(320u, 240u),
                            std::make_pair(176u, 144u)})
        require(std::any_of(modes.begin(), modes.end(), [&](const auto& mode) { return mode.width == size.first && mode.height == size.second; }),
                "a standard size is offered");
    for (const auto& mode : modes)
        require(mode.width % 2 == 0 && mode.height % 2 == 0 && mode.minimumFrameRate >= 1 && mode.maximumFrameRate >= mode.minimumFrameRate,
                "sizes are even and have a rate");
    for (std::size_t index = 1; index < modes.size(); ++index)
        require(std::uint64_t(modes[index - 1].width) * modes[index - 1].height >= std::uint64_t(modes[index].width) * modes[index].height,
                "largest first");
}

// An iPad mini's back camera as the platform reports it: the photo sizes belong to the format a camera runs in.
CameraInfo tabletCamera()
{
    CameraInfo info;
    info.identifier = "tablet-back";
    info.position = CameraPosition::back;
    info.lens = CameraLensKind::wide;
    info.captureFormats = {
        {1440, 1080, {{1, 30}}, {{1440, 1080}, {2016, 1512}}, true, true},
        {1920, 1440, {{1, 30}}, {{1920, 1440}, {4032, 3024}}, true, true},
        {4032, 3024, {{1, 30}}, {{4032, 3024}, {4224, 2376}}, true, false},
        {1920, 1080, {{1, 60}}, {{1920, 1080}, {2016, 1134}}, true, true},
    };
    for (const auto& format : info.captureFormats)
        info.videoFormats.push_back({format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), format.multiCamera});
    // Every size some format makes, as the platform lists them.
    info.stillSizes = {{4032, 3024}, {2016, 1512}, {1920, 1440}, {1920, 1080}, {1440, 1080}, {2016, 1134}};
    return info;
}

std::uint32_t fastestRate(const CameraDescription& camera)
{
    std::uint32_t rate = 0;
    for (const auto& mode : camera.video) rate = std::max(rate, mode.maximumFrameRate);
    return rate;
}

// The photos a camera is offered are those every stream it permits can make: no size that only another format makes,
// and the streams that could not make them are not permitted (the photos win over a 60 fps stream).
void testOfferedStillsFollowTheFormats()
{
    const std::vector<CameraInfo> infos {tabletCamera()};
    const auto cameras = arrangeCameras(infos, {}, "tablet", CalibrationStore(), {});
    require(cameras.size() == 1);
    const auto& camera = cameras[0];
    require(camera.jpegSizes == std::vector<CameraStillSize>({{4032, 3024}, {1920, 1440}}),
            "the sizes the 1920x1440 format makes: not 4224x2376, which only the 12 MP video format lists");
    require(camera.info.stillSizes == camera.jpegSizes, "the camera's own list is the offer");
    require(fastestRate(camera) == 30, "no stream above 30 fps: the 1080p60 format makes no 12 MP");
    require(std::none_of(camera.info.videoFormats.begin(), camera.info.videoFormats.end(),
                         [](const CameraVideoFormat& format) { return format.maximumFrameRate > 30; }),
            "the rates the guest is told are cut with the modes");
    // The offer holds for what remains: arranging the arranged camera changes nothing.
    const std::vector<CameraInfo> again {camera.info};
    const auto second = arrangeCameras(again, {}, "tablet", CalibrationStore(), {});
    require(second[0].jpegSizes == camera.jpegSizes && fastestRate(second[0]) == 30, "stable");
    // The guest's cap bounds the offer: the small tier's 12 MP, the large tier's 48 MP.
    ArrangementOptions large;
    large.tier = CameraTier::large;
    require(arrangeCameras(infos, {}, "tablet", CalibrationStore(), large)[0].jpegSizes.front() == CameraStillSize({4032, 3024}),
            "a camera that lists no 48 MP offers none");
}

// A camera that runs with others streams in a multi-camera session's formats then, and a still taken beside another
// camera is made in one: both are what the offer holds for.
void testConcurrentCamerasOfferWhatAMultiCameraSessionMakes()
{
    auto info = tabletCamera();
    info.captureFormats[1].multiCamera = false;  // the 12 MP-making format cannot join a multi-camera session
    for (auto& format : info.videoFormats)
        if (format.width == 1920 && format.height == 1440) format.multiCamera = false;
    CameraInfo other = tabletCamera();
    other.identifier = "tablet-front";
    other.position = CameraPosition::front;
    const std::vector<CameraInfo> infos {info, other};
    const std::vector<std::vector<std::size_t>> together {{0, 1}};
    const auto alone = arrangeCameras(infos, {}, "tablet", CalibrationStore(), {});
    require(alone[0].jpegSizes.front() == CameraStillSize({4032, 3024}), "alone, in the 12 MP format");
    const auto joined = arrangeCameras(infos, together, "tablet", CalibrationStore(), {});
    require(joined[0].jpegSizes.empty(), "beside another camera the streams run in the binned format, which makes no 12 MP");
}

// A host with one format per camera states its sizes and nothing else: they are all offered up to the cap.
void testHostsWithoutCaptureFormatsOfferTheirSizesUpToTheCap()
{
    auto info = tabletCamera();
    info.captureFormats.clear();
    info.stillSizes = {{4032, 3024}, {8064, 6048}, {640, 480}};
    const auto cameras = arrangeCameras(std::vector<CameraInfo> {info}, {}, "plain", CalibrationStore(), {});
    require(cameras[0].jpegSizes == std::vector<CameraStillSize>({{4032, 3024}, {640, 480}}), "the sizes under the cap");
    require(fastestRate(cameras[0]) == 60, "and every rate");
}

void testRawNeedsCalibration()
{
    const Host phone(SyntheticCameraProfile::phone);
    CalibrationStore store;
    auto cameras = phone.arrange(CameraTier::small, store);
    for (const auto& camera : cameras)
        require(!camera.raw() && !(camera.wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) && camera.wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_NONE,
                "no RAW without calibration");

    const auto key = CalibrationStore::lensKey(phone.infos[1]);
    require(store.save(phone.model, key, colorOf(2016, 1512)), "calibrate the wide lens");
    cameras = phone.arrange(CameraTier::small, store);
    require(cameras[2].raw() && (cameras[2].wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) &&
                cameras[2].wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_GRBG && cameras[2].wire.raw_width == 2016 &&
                cameras[2].wire.raw_height == 1512 && cameras[2].wire.black_level == 64 && cameras[2].wire.white_level == 1023,
            "a calibrated lens has RAW");
    require(cameras[2].wire.color_matrix1_q16[0] == 65536 && cameras[2].wire.reference_illuminant1 == 17, "with its colour data");
    require(cameras[2].wire.focal_length_um == 6900, "and the focal length the still revealed");
    require(!cameras[3].raw() && !cameras[4].raw(), "another lens has none");

    // Data that cannot describe a sensor is not enough.
    auto bad = colorOf(2016, 1512);
    bad.whiteLevel = bad.blackLevel;
    require(store.save(phone.model, key, bad), "save bad data");
    require(!phone.arrange(CameraTier::small, store)[2].raw(), "a white level at the black level is not calibration");
    bad = colorOf(2016, 1512);
    bad.bayer = BayerOrder::none;
    require(store.save(phone.model, key, bad), "save data with no pattern");
    require(!phone.arrange(CameraTier::small, store)[2].raw(), "nor is a missing pattern");
    // A calibration beyond the tier's still size is not advertised either.
    require(store.save(phone.model, key, colorOf(8064, 6048)), "save a 48 MP calibration");
    require(!phone.arrange(CameraTier::small, store)[2].raw() && phone.arrange(CameraTier::large, store)[2].raw(),
            "48 MP RAW needs the large tier");
}

// What a live session holds on the host outside the guest's RAM, which the GPU budget gives up (the pool is B less
// the reserve): derived from the tier's limits, so a larger tier needs more.
void testHostMemoryReserve()
{
    const auto small = cameraHostMemory(CameraTier::small);
    require(small.capturePools == 4ull * 1920 * 1080 * 3 / 2, "four 1080p NV12 frames");
    require(small.staging == 1920ull * 1080 * 3 / 2, "one frame");
    require(small.rawStill == 12'700'000ull * 2 * 3, "the RAW pixels three times: the platform's, the copy, the DNG");
    require(small.jpegStill == 12'700'000ull / 2 * 3, "three copies of the JPEG");
    require(small.total() == 12'441'600ull + 3'110'400ull + 76'200'000ull + 19'050'000ull, "the parts add up");
    require(cameraHostMemoryReserveMiB(CameraTier::small) == 106, "105.7 MiB, rounded up");
    // The tier every host gets is the small one, whatever its budget, and so is the reserve it holds.
    require(cameraHostMemoryReserveMiB(cameraTierForBudget(534ull << 20)) == 106);
    require(cameraHostMemoryReserveMiB(cameraTierForBudget(1249ull << 20)) == 106);
    const auto large = cameraHostMemory(CameraTier::large);
    require(large.capturePools == 6ull * 3840 * 2160 * 3 / 2 && large.staging == 3840ull * 2160 * 3 / 2);
    require(cameraHostMemoryReserveMiB(CameraTier::large) == 441 && large.total() > small.total(),
            "lifting the cap raises it with the tier");
}

void testWireFormats()
{
    for (const auto order : {BayerOrder::rggb, BayerOrder::grbg, BayerOrder::gbrg, BayerOrder::bggr})
        require(bayerOrderOf(rawWireFormat(order)) == order, "a Bayer order round-trips through its format");
    require(rawWireFormat(BayerOrder::none) == 0 && bayerOrderOf(VIRTIO_VIDEO_FORMAT_NV12) == BayerOrder::none, "none is none");
}

void testQueryPages()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto cameras = phone.arrange(CameraTier::small);
    const auto whole = cameraQueryResponse(cameras, 0, 4096, 0);
    const auto header = load<virtio_video_camera_query_resp>(whole);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY && header.num_cameras == 5 && header.num_descs == 5 &&
                header.next_index == 0 && header.session_flags == 0 && header.desc_size == sizeof(virtio_video_camera_desc),
            "the whole answer fits");
    require(whole.size() == sizeof(header) + 5 * sizeof(virtio_video_camera_desc), "five records");
    const auto third = load<virtio_video_camera_desc>(whole, sizeof(header) + 2 * sizeof(virtio_video_camera_desc));
    require(third.index == 2 && third.lens == VIRTIO_VIDEO_CAMERA_LENS_WIDE, "records are in order");

    // The answer pages when the guest's buffer is short, and says it is blocked.
    const std::size_t room = sizeof(header) + 2 * sizeof(virtio_video_camera_desc);
    auto page = cameraQueryResponse(cameras, 0, room, VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED);
    auto first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 2 && first.next_index == 2 && first.num_cameras == 5 &&
                (first.session_flags & VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED) && page.size() == room,
            "the first page");
    page = cameraQueryResponse(cameras, 2, room, 0);
    first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 2 && first.next_index == 4, "the second page");
    page = cameraQueryResponse(cameras, 4, room, 0);
    first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 1 && first.next_index == 0, "the last page ends the walk");
    page = cameraQueryResponse(cameras, 5, room, 0);
    require(load<virtio_video_camera_query_resp>(page).num_descs == 0, "past the end is empty");
    // A buffer with no room for a record is refused: "complete" would end the walk.
    bool refused = false;
    try {
        (void)cameraQueryResponse(cameras, 1, sizeof(header) + sizeof(virtio_video_camera_desc) - 1, 0);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    require(refused, "no room for a record is refused");
}

void testFormatAnswers()
{
    const Host phone(SyntheticCameraProfile::phone);
    CalibrationStore store;
    store.save(phone.model, CalibrationStore::lensKey(phone.infos[1]), colorOf(2016, 1512));
    const auto cameras = phone.arrange(CameraTier::small, store);

    const auto video = cameraFormatsResponse(cameras[2], VIRTIO_VIDEO_CAMERA_STREAM_VIDEO);
    auto header = load<virtio_video_query_capability_resp>(video);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY && header.num_descs == 1, "video is one format");
    std::size_t at = sizeof(header);
    const auto desc = load<virtio_video_format_desc>(video, at);
    require(desc.format == VIRTIO_VIDEO_FORMAT_NV12 && (desc.planes_layout & VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER) &&
                desc.plane_align == 16 && desc.num_frames > 3,
            "NV12 in one buffer, rows at 16 bytes");
    at += sizeof(desc);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    for (std::uint32_t frame = 0; frame < desc.num_frames; ++frame) {
        const auto record = load<virtio_video_format_frame>(video, at);
        require(record.width.min == record.width.max && record.height.min == record.height.max && record.num_rates >= 1,
                "a frame record is one size with its rates");
        sizes.emplace_back(record.width.min, record.height.min);
        at += sizeof(record);
        for (std::uint32_t rate = 0; rate < record.num_rates; ++rate) {
            const auto range = load<virtio_video_format_range>(video, at);
            require(range.min >= 1 && range.max >= range.min, "a rate range");
            at += sizeof(range);
        }
    }
    require(at == video.size(), "the answer holds exactly its records");
    require(std::find(sizes.begin(), sizes.end(), std::make_pair(1920u, 1080u)) != sizes.end() &&
                std::find(sizes.begin(), sizes.end(), std::make_pair(1280u, 720u)) != sizes.end(),
            "1080p and 720p are offered");

    // Stills: JPEG sizes, and RAW only for the calibrated lens.
    const auto walk = [](const std::vector<std::uint8_t>& bytes, std::vector<std::uint32_t>& formats,
                         std::vector<std::pair<std::uint32_t, std::uint32_t>>& stillSizes) {
        const auto head = load<virtio_video_query_capability_resp>(bytes);
        std::size_t pos = sizeof(head);
        for (std::uint32_t index = 0; index < head.num_descs; ++index) {
            const auto d = load<virtio_video_format_desc>(bytes, pos);
            pos += sizeof(d);
            formats.push_back(d.format);
            for (std::uint32_t frame = 0; frame < d.num_frames; ++frame) {
                const auto record = load<virtio_video_format_frame>(bytes, pos);
                stillSizes.emplace_back(record.width.min, record.height.min);
                pos += sizeof(record) + record.num_rates * sizeof(virtio_video_format_range);
            }
        }
        return pos == bytes.size();
    };
    std::vector<std::uint32_t> formats;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stills;
    require(walk(cameraFormatsResponse(cameras[2], VIRTIO_VIDEO_CAMERA_STREAM_STILL), formats, stills), "the stills answer is whole");
    require(formats == std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG},
            "JPEG, and RAW for the calibrated lens");
    require(stills.front() == std::make_pair(4032u, 3024u) && stills.back() == std::make_pair(2016u, 1512u), "their sizes");
    formats.clear();
    stills.clear();
    require(walk(cameraFormatsResponse(cameras[3], VIRTIO_VIDEO_CAMERA_STREAM_STILL), formats, stills), "another lens' answer");
    require(formats == std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_CAMERA_JPEG}, "JPEG only");
    // The kinds that carry no format list answer with an empty list.
    require(load<virtio_video_query_capability_resp>(cameraFormatsResponse(cameras[2], VIRTIO_VIDEO_CAMERA_STREAM_META)).num_descs == 0,
            "a metadata stream has no formats");
}

void testRates()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto cameras = phone.arrange(CameraTier::small);
    const auto& wide = cameras[2];
    require(supportedFrameRate(wide, 1280, 720, 30) == 30, "30 fps is run at 720p");
    require(supportedFrameRate(wide, 1280, 720, 240) == 120, "a rate above the camera's is the camera's highest");
    require(supportedFrameRate(wide, 1920, 1080, 120) == 60, "1080p runs at 60");
    require(supportedFrameRate(wide, 640, 480, 0) == 1, "and below, the lowest");
    require(supportedFrameRate(wide, 3840, 2160, 30) == 0 && supportedFrameRate(wide, 1281, 720, 30) == 0 &&
                supportedFrameRate(wide, 8, 8, 30) == 0,
            "a size the camera does not offer has no rate");
    require(videoSizeAllowed(wide, 320, 240) && videoSizeAllowed(wide, 1920, 1080) && !videoSizeAllowed(wide, 1922, 1080) &&
                !videoSizeAllowed(wide, 1919, 1080),
            "a size may be below the camera's but not above, and is even");
}

void testLayout()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto small = phone.arrange(CameraTier::small);
    auto layout = summarizeLayout(small, CameraTier::small);
    require(layout.back && layout.front && layout.autofocus && layout.flash && layout.manualSensor && !layout.raw && layout.concurrent &&
                layout.frontMaximumVideoHeight == 1080 && layout.tier == CameraTier::small,
            "a phone's layout");
    layout = summarizeLayout(phone.arrange(CameraTier::large), CameraTier::large);
    require(layout.frontMaximumVideoHeight == 2160, "the front camera's best height follows the tier");

    const Host laptop(SyntheticCameraProfile::laptop);
    layout = summarizeLayout(laptop.arrange(CameraTier::small), CameraTier::small);
    require(!layout.back && layout.front && !layout.autofocus && !layout.flash && !layout.concurrent, "a laptop's layout");
    layout = summarizeLayout({}, CameraTier::small);
    require(!layout.back && !layout.front && !layout.manualSensor && !layout.raw && !layout.concurrent, "no cameras");
}

} // namespace

int main()
{
    testPhoneOrder();
    testOtherHosts();
    testTiers();
    testOfferedStillsFollowTheFormats();
    testConcurrentCamerasOfferWhatAMultiCameraSessionMakes();
    testHostsWithoutCaptureFormatsOfferTheirSizesUpToTheCap();
    testHostMemoryReserve();
    testRawNeedsCalibration();
    testWireFormats();
    testQueryPages();
    testFormatAnswers();
    testRates();
    testLayout();
    return 0;
}
