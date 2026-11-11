// SPDX-License-Identifier: GPL-2.0-or-later
//
// The camera HAL's rules for what a camera is (android/lineage-24/overlay/camera/hal):
// the IDs Android sees for the host's cameras, the capabilities and the stream
// tables its description and formats earn.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "camera_ids.hpp"
#include "camera_plan.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

std::vector<CameraKey> triple_host()
{
    return {{0, abi::facing::back, abi::lens::zoom},
            {1, abi::facing::front, abi::lens::wide},
            {2, abi::facing::back, abi::lens::wide},
            {3, abi::facing::back, abi::lens::ultra_wide},
            {4, abi::facing::back, abi::lens::telephoto}};
}

std::uint32_t id_of(const std::vector<IdAssignment>& ids, std::uint32_t index)
{
    for (const auto& entry : ids)
        if (entry.index == index) return entry.id;
    return 99;
}

void check_ids()
{
    auto keys = triple_host();
    auto ids = assign_ids(keys);
    require(ids.size() == 5, "a triple host lists five cameras");
    require(id_of(ids, 0) == 0 && id_of(ids, 1) == 1 && id_of(ids, 2) == 2 && id_of(ids, 3) == 3 && id_of(ids, 4) == 4,
            "zoom 0, front 1, wide 2, ultra wide 3, telephoto 4");
    for (std::size_t i = 1; i < ids.size(); ++i) require(ids[i - 1].id < ids[i].id, "ascending");

    // A dual wide device (no telephoto): there is no "4".
    keys.pop_back();
    ids = assign_ids(keys);
    require(ids.size() == 4 && id_of(ids, 3) == 3, "no telephoto, no ID 4");

    // A single-lens device: the one lens is "0", the front camera "1", and there is no "2".
    const std::vector<CameraKey> mini = {{0, abi::facing::back, abi::lens::wide}, {1, abi::facing::front, abi::lens::wide}};
    ids = assign_ids(mini);
    require(ids.size() == 2 && id_of(ids, 0) == 0 && id_of(ids, 1) == 1, "single lens");

    // The host orders its lenses by the lens kind it has, so a missing wide shifts the indices; the IDs follow the kind.
    const std::vector<CameraKey> odd = {{0, abi::facing::back, abi::lens::zoom},
                                        {1, abi::facing::front, abi::lens::wide},
                                        {2, abi::facing::back, abi::lens::telephoto}};
    ids = assign_ids(odd);
    require(id_of(ids, 2) == 4, "a telephoto keeps ID 4 whatever its index");

    // A Mac: a front camera only. Its ID is "0".
    ids = assign_ids(std::vector<CameraKey>{{0, abi::facing::front, abi::lens::wide}});
    require(ids.size() == 1 && ids[0].id == 0 && ids[0].index == 0, "the front camera alone is ID 0");

    require(assign_ids(std::vector<CameraKey>{}).empty(), "no camera, no ID");
    const std::vector<CameraKey> external = {{0, abi::facing::external, abi::lens::external}};
    require(assign_ids(external).empty(), "an external camera is not listed");
    const std::vector<CameraKey> twice = {{0, abi::facing::back, abi::lens::wide},
                                          {1, abi::facing::back, abi::lens::ultra_wide},
                                          {2, abi::facing::back, abi::lens::ultra_wide}};
    require(assign_ids(twice).size() == 2, "a second camera of one lens is left out");
}

abi::Desc lens_desc()
{
    abi::Desc desc{};
    desc.index = 2;
    desc.facing = abi::facing::back;
    desc.lens = abi::lens::wide;
    desc.flags = abi::feature::autofocus | abi::feature::flash | abi::feature::manual_exposure | abi::feature::raw;
    desc.pixel_array_width = 4032;
    desc.pixel_array_height = 3024;
    desc.focal_length_um = 5100;
    desc.aperture_x100 = 180;
    desc.min_focus_distance_mm = 100;
    desc.iso_min = 32;
    desc.iso_max = 3200;
    desc.exposure_min_ns = 25'000;
    desc.exposure_max_ns = 1'000'000'000;
    desc.bayer_order = abi::bayer::rggb;
    desc.raw_width = 4032;
    desc.raw_height = 3024;
    return desc;
}

Formats phone_formats()
{
    Formats formats;
    formats.video = {{3840, 2160, 1, 30}, {3840, 2160, 60, 60}, {1920, 1080, 1, 60}, {1280, 720, 1, 120}, {640, 480, 1, 30}};
    formats.jpeg = {{4032, 3024}, {1920, 1080}, {640, 480}};
    formats.raw = {4032, 3024};
    return formats;
}

void check_capabilities()
{
    auto desc = lens_desc();
    auto formats = phone_formats();
    auto caps = capabilities_of(desc, formats);
    require(caps.manual_sensor && caps.read_sensor_settings, "a lens with manual exposure has MANUAL_SENSOR");
    require(caps.raw, "a calibrated lens has RAW");
    require(caps.burst_capture, "4K at 30 is a burst camera");
    require(array_size(desc, caps) == Size({4032, 3024}), "the array is the Bayer size");

    desc.flags &= ~abi::feature::raw;
    require(!capabilities_of(desc, formats).raw, "an uncalibrated lens has no RAW");
    desc.flags |= abi::feature::raw;
    desc.bayer_order = abi::bayer::none;
    require(!capabilities_of(desc, formats).raw, "RAW needs a Bayer order");
    desc.bayer_order = abi::bayer::rggb;
    formats.raw = {};
    require(!capabilities_of(desc, formats).raw, "RAW needs a Bayer size");

    // The zoom camera is automatic only, whatever the host says.
    desc = lens_desc();
    desc.lens = abi::lens::zoom;
    caps = capabilities_of(desc, phone_formats());
    require(!caps.manual_sensor && !caps.read_sensor_settings && !caps.raw, "the zoom camera has no manual capabilities");

    // The small tier stops at 1080p: no burst capture.
    formats = phone_formats();
    formats.video = {{1920, 1080, 1, 60}, {640, 480, 1, 30}};
    require(!capabilities_of(lens_desc(), formats).burst_capture, "1080p is not a burst camera");
    require(pipeline_depth(formats) == 4, "the small tier has four buffers");
    require(pipeline_depth(phone_formats()) == 6, "the large tier has six");
}

void check_tables()
{
    const auto desc = lens_desc();
    const auto formats = phone_formats();
    const auto caps = capabilities_of(desc, formats);
    const auto tables = stream_tables(formats, caps);
    require(tables.configurations.size() % 4 == 0, "four numbers a configuration");
    const auto entries = tables.configurations.size() / 4;
    require(tables.min_frame_durations.size() == entries * 4 && tables.stall_durations.size() == entries * 4,
            "a duration for each configuration");
    // 4 video sizes (4K, 1080p, 720p, VGA) twice, three JPEG sizes, one RAW.
    require(entries == 4 * 2 + 3 + 1, "the number of entries");

    auto find = [&](std::int32_t format, std::int32_t width, std::int32_t height) -> int {
        for (std::size_t i = 0; i < entries; ++i)
            if (tables.configurations[4 * i] == format && tables.configurations[4 * i + 1] == width &&
                tables.configurations[4 * i + 2] == height)
                return int(i);
        return -1;
    };
    const int yuv4k = find(pixel_format::yuv_420_888, 3840, 2160);
    require(yuv4k >= 0 && tables.configurations[4 * std::size_t(yuv4k) + 3] == 0, "an output at 4K");
    require(tables.min_frame_durations[4 * std::size_t(yuv4k) + 3] == 1'000'000'000 / 60, "4K runs at the fastest rate it is offered at");
    const int yuv720 = find(pixel_format::implementation_defined, 1280, 720);
    require(yuv720 >= 0 && tables.min_frame_durations[4 * std::size_t(yuv720) + 3] == 1'000'000'000 / 120, "720p at 120");
    const int blob = find(pixel_format::blob, 4032, 3024);
    require(blob >= 0 && tables.stall_durations[4 * std::size_t(blob) + 3] > 300'000'000, "a 12 MP JPEG stalls for a while");
    require(find(pixel_format::blob, 640, 480) >= 0, "a small JPEG");
    const int raw = find(pixel_format::raw16, 4032, 3024);
    require(raw >= 0 && tables.stall_durations[4 * std::size_t(raw) + 3] > 0, "RAW16 at the Bayer size has a stall");
    for (std::size_t i = 0; i < entries; ++i) {
        require(tables.min_frame_durations[4 * i] == tables.configurations[4 * i], "the format repeats");
        require(tables.min_frame_durations[4 * i + 1] == tables.configurations[4 * i + 1], "the width repeats");
    }
    require(find(pixel_format::raw16, 4032, 3024) >= 0, "RAW present");
    require(find(pixel_format::yuv_420_888, 4032, 3024) < 0, "no YUV at a still size the video does not offer");

    Capabilities none;
    const auto without = stream_tables(formats, none);
    require(without.configurations.size() == tables.configurations.size() - 4, "no RAW entry without RAW");
}

void check_numbers()
{
    const auto ranges = fps_ranges(phone_formats());
    require(ranges.size() % 2 == 0, "pairs");
    auto has = [&](std::int32_t low, std::int32_t high) {
        for (std::size_t i = 0; i + 1 < ranges.size(); i += 2)
            if (ranges[i] == low && ranges[i + 1] == high) return true;
        return false;
    };
    require(has(30, 30) && has(15, 30) && has(60, 60) && has(120, 120), "ranges for 30, 60 and 120");
    require(!has(240, 240), "none for 240 when the host has none");
    Formats slow;
    slow.video = {{640, 480, 1, 30}};
    require(fps_ranges(slow).size() == 6, "a 30 fps host has three ranges");

    const auto sizes = thumbnail_sizes();
    require(sizes.size() >= 4 && sizes[0] == 0 && sizes[1] == 0, "(0, 0) first");
    for (std::size_t i = 2; i + 3 < sizes.size(); i += 2)
        require(std::int64_t(sizes[i]) * sizes[i + 1] <= std::int64_t(sizes[i + 2]) * sizes[i + 3], "ascending area");

    require(jpeg_max_size(phone_formats()) == 4032 * 3024 * 3 / 2 + 65000 + 8, "the largest JPEG");
    const auto desc = lens_desc();
    require(focal_length_mm(desc) == 5.1f && aperture(desc) == 1.8f, "optics");
    require(minimum_focus_dioptres(desc) == 10.0f, "100 mm is ten dioptres");
    auto fixed = desc;
    fixed.min_focus_distance_mm = 0;
    require(minimum_focus_dioptres(fixed) == 0.0f, "fixed focus has no focus distance");
    require(sensitivity_min(desc) == 32 && sensitivity_max(desc) == 3200, "sensitivity");
    auto coarse = desc;
    coarse.iso_min = 200;
    coarse.iso_max = 64;
    require(sensitivity_min(coarse) == 100 && sensitivity_max(coarse) == 100, "the range includes the default ISO");
    require(from_q16(to_q16(0.5)) == 0.5f && from_q16(to_q16(-1.25)) == -1.25f, "Q16 round trip");
    require(max_frame_duration_ns(desc) == 1'000'000'000, "the longest frame is a second");
}

// The host's concurrency: which cameras stream together, and which exclude one another.
void check_concurrency()
{
    const std::uint32_t front = 1u << 1;
    // The host index is the ID here, and the zoom camera runs with the front one only.
    const std::vector<Concurrency> pair = {{0, 0, front, 0}, {1, 1, 1u << 0, 0}, {2, 2, 0, 0}};
    auto sets = concurrent_sets(pair);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "the zoom and the front camera stream together");

    // One side naming the other is not enough.
    const std::vector<Concurrency> lopsided = {{0, 0, front, 0}, {1, 1, 0, 0}};
    require(concurrent_sets(lopsided).empty(), "both masks must agree");

    // Three cameras that all name one another are one set, not three pairs.
    const std::vector<Concurrency> triple = {{0, 0, 0b110, 0}, {1, 1, 0b101, 0}, {2, 2, 0b011, 0}};
    sets = concurrent_sets(triple);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1, 2}), "a maximal set");

    // A chain a-b, b-c is two sets.
    const std::vector<Concurrency> chain = {{0, 0, 0b010, 0}, {1, 1, 0b101, 0}, {2, 2, 0b010, 0}};
    sets = concurrent_sets(chain);
    require(sets.size() == 2 && sets[0] == std::vector<std::uint32_t>({0, 1}) && sets[1] == std::vector<std::uint32_t>({1, 2}),
            "two pairs");

    // The ID differs from the host's number.
    const std::vector<Concurrency> renumbered = {{0, 4, 1u << 5, 0}, {1, 5, 1u << 4, 0}};
    sets = concurrent_sets(renumbered);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "masks are in the host's numbers");

    require(concurrent_sets(std::vector<Concurrency>{}).empty(), "no cameras, no sets");

    const std::vector<Concurrency> conflicts = {{0, 0, 0, 1u << 3}, {1, 1, 0, 0}, {3, 3, 0, 0}, {4, 4, 1u << 0, 0}};
    auto ids = conflicting_ids(conflicts, 0);
    require(ids == std::vector<std::uint32_t>({3}), "the conflict mask names camera 3");
    ids = conflicting_ids(conflicts, 3);
    require(ids == std::vector<std::uint32_t>({0}), "a conflict is symmetric");
    require(conflicting_ids(conflicts, 1).empty(), "camera 1 conflicts with none");
    require(conflicting_ids(conflicts, 9).empty(), "an unknown camera has no conflicts");
}

} // namespace

int main()
{
    check_ids();
    check_concurrency();
    check_capabilities();
    check_tables();
    check_numbers();
    return 0;
}
