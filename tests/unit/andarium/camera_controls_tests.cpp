// SPDX-License-Identifier: GPL-2.0-or-later
//
// How the camera HAL turns a request into the camera's controls and an
// exposure's metadata into the request's results, and how it edits a JPEG that
// the host encoded (android/lineage-24/overlay/camera/hal).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "camera_controls.hpp"
#include "camera_exif.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;
namespace av = android_value;

namespace {

Limits lens_limits()
{
    Limits limits;
    limits.flags = abi::feature::autofocus | abi::feature::flash | abi::feature::torch | abi::feature::zoom |
        abi::feature::manual_exposure | abi::feature::manual_focus | abi::feature::manual_white_balance;
    limits.array_width = 4000;
    limits.array_height = 3000;
    limits.max_wb_gain_q16 = to_q16(4.0);
    limits.min_focus_distance_mm = 100;
    limits.zoom_min_q16 = to_q16(1.0);
    limits.zoom_max_q16 = to_q16(10.0);
    limits.exposure_min_ns = 25'000;
    limits.exposure_max_ns = 1'000'000'000;
    limits.iso_min = 32;
    limits.iso_max = 3200;
    limits.manual_sensor = true;
    return limits;
}

const ControlWrite* find(const std::vector<ControlWrite>& writes, std::uint32_t id)
{
    for (const auto& write : writes)
        if (write.id == id) return &write;
    return nullptr;
}

void check_automatic()
{
    ControlPlanner planner(lens_limits());
    Request request;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_aperture_priority, "AE on is automatic");
    require(find(writes, abi::cid::iso_sensitivity_auto)->value == abi::v4l2_value::iso_automatic, "ISO automatic");
    require(find(writes, abi::cid::focus_auto)->value == 1, "continuous picture is continuous AF");
    require(find(writes, abi::cid::auto_white_balance)->value == 1, "AWB auto");
    require(find(writes, abi::cid::exposure_ns) == nullptr, "no manual exposure in AE");
    require(find(writes, abi::cid::lock_3a)->value == 0, "no locks");
    require(find(writes, abi::cid::zoom_q16)->value == std::int64_t(to_q16(1.0)), "zoom 1.0");
    require(find(writes, abi::cid::flash_led_mode)->value == abi::v4l2_value::flash_led_off, "torch off");
    require(find(writes, abi::cid::still_flash)->value == 0, "still flash off");
    require(find(writes, abi::cid::test_pattern)->value == 0, "no test pattern");

    // The same request writes nothing again.
    require(planner.plan(request).empty(), "an unchanged request writes nothing");
    // A reset forgets.
    planner.reset();
    require(!planner.plan(request).empty(), "after a reset everything is written");
}

void check_manual()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_mode = av::ae_off;
    request.exposure_ns = 8'333'333;
    request.sensitivity = 400;
    request.af_mode = av::af_off;
    request.focus_distance = 5.0f;
    request.awb_mode = av::awb_off;
    request.wb_gains = {2.0f, 1.0f, 1.0f, 1.5f};
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_manual, "manual exposure");
    require(find(writes, abi::cid::iso_sensitivity_auto)->value == abi::v4l2_value::iso_manual, "manual ISO");
    require(find(writes, abi::cid::exposure_ns)->value == 8'333'333, "the exposure in nanoseconds");
    require(find(writes, abi::cid::iso)->value == 400, "the ISO");
    require(find(writes, abi::cid::auto_exposure_bias) == nullptr, "no bias in manual exposure");
    require(find(writes, abi::cid::focus_auto)->value == 0, "AF off");
    require(find(writes, abi::cid::focus_absolute)->value == 5000, "five dioptres in thousandths");
    require(find(writes, abi::cid::auto_white_balance)->value == 0, "manual white balance");
    require(find(writes, abi::cid::wb_gain_red)->value == std::int64_t(to_q16(2.0)), "red gain");
    require(find(writes, abi::cid::wb_gain_blue)->value == std::int64_t(to_q16(1.5)), "blue gain");

    // Out-of-range values are clamped to the camera's.
    request.exposure_ns = 10;
    request.sensitivity = 100000;
    request.focus_distance = 50.0f;
    request.wb_gains = {9.0f, 1.0f, 1.0f, 0.1f};
    writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_ns)->value == 25'000, "exposure clamped up");
    require(find(writes, abi::cid::iso)->value == 3200, "ISO clamped down");
    require(find(writes, abi::cid::focus_absolute)->value == 10000, "focus clamped to the minimum distance");
    require(find(writes, abi::cid::wb_gain_red)->value == std::int64_t(to_q16(4.0)), "gain clamped to the maximum");
    require(find(writes, abi::cid::wb_gain_blue)->value == std::int64_t(to_q16(1.0)), "gain at least one");

    // A camera without manual sensor (the zoom camera) stays automatic.
    auto zoom = lens_limits();
    zoom.manual_sensor = false;
    ControlPlanner automatic(zoom);
    request.ae_mode = av::ae_off;
    writes = automatic.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_aperture_priority, "no manual exposure");
    require(find(writes, abi::cid::exposure_ns) == nullptr, "no exposure write");
}

void check_focus_and_locks()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.af_mode = av::af_auto;
    request.af_trigger = av::af_trigger_start;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::focus_auto)->value == 0, "single AF is not continuous");
    require(find(writes, abi::cid::auto_focus_start) != nullptr, "the trigger starts a scan");

    request.af_trigger = av::af_trigger_idle;
    writes = planner.plan(request);
    require(find(writes, abi::cid::auto_focus_start) == nullptr, "an idle trigger starts nothing");

    request.af_trigger = av::af_trigger_cancel;
    writes = planner.plan(request);
    require(find(writes, abi::cid::auto_focus_stop) != nullptr, "cancel stops it");

    // In continuous AF a trigger locks focus until it is cancelled.
    ControlPlanner continuous(lens_limits());
    Request pic;
    pic.af_trigger = av::af_trigger_start;
    writes = continuous.plan(pic);
    require(find(writes, abi::cid::lock_3a)->value == abi::v4l2_value::lock_focus, "focus locked by the trigger");
    pic.af_trigger = av::af_trigger_idle;
    writes = continuous.plan(pic);
    require(find(writes, abi::cid::lock_3a) == nullptr, "the lock holds");
    pic.af_trigger = av::af_trigger_cancel;
    writes = continuous.plan(pic);
    require(find(writes, abi::cid::lock_3a)->value == 0, "cancel releases it");

    Request locked;
    locked.ae_lock = true;
    locked.awb_lock = true;
    ControlPlanner locks(lens_limits());
    require(find(locks.plan(locked), abi::cid::lock_3a)->value ==
                (abi::v4l2_value::lock_exposure | abi::v4l2_value::lock_white_balance),
            "exposure and white balance locks");

    // A fixed-focus camera writes no focus controls.
    auto fixed = lens_limits();
    fixed.flags &= ~abi::feature::autofocus;
    ControlPlanner nofocus(fixed);
    writes = nofocus.plan(Request{});
    require(find(writes, abi::cid::focus_auto) == nullptr && find(writes, abi::cid::af_region) == nullptr, "fixed focus");
}

void check_other_controls()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_compensation_steps = 3;
    request.awb_mode = av::awb_daylight;
    request.zoom_ratio = 25.0f;
    request.flash_mode = av::flash_torch;
    request.ae_mode = av::ae_on_auto_flash;
    request.test_pattern = av::pattern_solid_color;
    request.rotate_and_crop = av::rotate_auto;
    request.jpeg_quality = 120;
    request.video_stabilization = true;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::auto_exposure_bias)->value == 1000, "three thirds of a stop is a thousand");
    require(find(writes, abi::cid::auto_white_balance)->value == 0 && find(writes, abi::cid::auto_n_preset_white_balance)->value == 6,
            "the daylight preset");
    require(find(writes, abi::cid::zoom_q16)->value == std::int64_t(to_q16(10.0)), "zoom clamped to the camera's");
    require(find(writes, abi::cid::flash_led_mode)->value == abi::v4l2_value::flash_led_torch, "torch");
    require(find(writes, abi::cid::still_flash)->value == 1, "automatic flash for the still");
    require(find(writes, abi::cid::test_pattern)->value == 1, "a test pattern is black");
    require(find(writes, abi::cid::rotate_and_crop)->value == abi::v4l2_value::rotate_automatic, "automatic rotation");
    require(find(writes, abi::cid::image_stabilization)->value == 1, "stabilization");
    require(find(writes, abi::cid::jpeg_compression_quality)->value == 100, "quality clamped");

    request.ae_mode = av::ae_on_always_flash;
    request.flash_mode = av::flash_off;
    planner.reset();
    require(find(planner.plan(request), abi::cid::still_flash)->value == 2, "always flash");
    request.ae_mode = av::ae_on;
    request.flash_mode = av::flash_single;
    planner.reset();
    require(find(planner.plan(request), abi::cid::still_flash)->value == 2, "a single flash");

    for (const auto mode : {av::awb_incandescent, av::awb_fluorescent, av::awb_warm_fluorescent, av::awb_twilight, av::awb_daylight,
                            av::awb_cloudy_daylight, av::awb_shade})
        require(awb_preset(mode).has_value(), "presets");
    require(!awb_preset(av::awb_auto) && !awb_preset(av::awb_off), "auto and off are not presets");
    require(rotate_value(av::rotate_none) == 0 && rotate_value(av::rotate_90) == 1 && rotate_value(av::rotate_180) == 2 &&
                rotate_value(av::rotate_270) == 3,
            "rotation values");

    // A camera without a torch or flash writes no flash controls.
    auto plain = lens_limits();
    plain.flags &= ~(abi::feature::flash | abi::feature::torch | abi::feature::zoom);
    ControlPlanner without(plain);
    writes = without.plan(request);
    require(find(writes, abi::cid::flash_led_mode) == nullptr && find(writes, abi::cid::still_flash) == nullptr &&
                find(writes, abi::cid::zoom_q16) == nullptr,
            "no flash, no zoom");
}

void check_regions()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_region = {1000, 750, 3000, 2250, 500};
    auto writes = planner.plan(request);
    const auto* region = find(writes, abi::cid::ae_region);
    require(region != nullptr && region->is_region, "an AE region");
    require(region->region[0] == to_q16(0.25) && region->region[1] == to_q16(0.25), "origin");
    require(region->region[2] == to_q16(0.5) && region->region[3] == to_q16(0.5), "size");
    require(region->region[4] == 500, "weight");
    require(planner.plan(request).empty() || find(planner.plan(request), abi::cid::ae_region) == nullptr, "unchanged");

    request.ae_region.weight = 0;
    region = find(planner.plan(request), abi::cid::ae_region);
    require(region != nullptr && region->region == std::array<std::uint32_t, abi::region_words>{}, "weight 0 clears");

    // Regions outside the array are clipped, and an empty one clears.
    const auto words = region_words({-100, -100, 8000, 6000, 1500}, lens_limits());
    require(words[0] == 0 && words[2] == to_q16(1.0) && words[3] == to_q16(1.0) && words[4] == 1000, "clipped");
    require(region_words({10, 10, 10, 50, 100}, lens_limits())[4] == 0, "an empty region is cleared");
}

void check_results()
{
    abi::Meta meta{};
    meta.exposure_ns = 10'000'000;
    meta.frame_duration_ns = 33'333'333;
    meta.iso = 125;
    meta.ae_state = av::ae_state_converged;
    meta.af_state = av::af_state_focused_locked;
    meta.awb_state = av::awb_state_converged;
    meta.lens_position_q16 = to_q16(2.5);
    meta.aperture_x100 = 180;
    meta.focal_length_um = 5100;
    meta.zoom_ratio_q16 = to_q16(1.5);
    meta.wb_gain_q16[0] = to_q16(1.8);
    meta.wb_gain_q16[1] = to_q16(1.0);
    meta.wb_gain_q16[2] = to_q16(1.4);
    meta.flash_state = 0;

    Request request;
    auto result = result_of(meta, request, true);
    require(result.ae_state == av::ae_state_converged && result.af_state == av::af_state_focused_locked &&
                result.awb_state == av::awb_state_converged,
            "the host's states");
    require(result.exposure_ns == 10'000'000 && result.sensitivity == 125 && result.frame_duration_ns == 33'333'333, "what applied");
    require(result.focus_distance == 2.5f && result.aperture == 1.8f && result.focal_length_mm == 5.1f, "optics");
    require(result.zoom_ratio == 1.5f, "zoom");
    require(result.wb_gains[0] > 1.79f && result.wb_gains[1] == result.wb_gains[2] && result.wb_gains[3] > 1.39f, "gains");
    require(result.flash_state == av::flash_state_ready, "a flash that has no state is ready");

    request.ae_precapture_trigger = av::ae_trigger_start;
    require(result_of(meta, request, true).ae_state == av::ae_state_precapture, "a precapture trigger reports PRECAPTURE");
    request.ae_precapture_trigger = av::ae_trigger_idle;
    request.ae_mode = av::ae_off;
    request.af_mode = av::af_off;
    request.awb_mode = av::awb_off;
    result = result_of(meta, request, false);
    require(result.ae_state == av::ae_state_inactive && result.af_state == av::af_state_inactive &&
                result.awb_state == av::awb_state_inactive,
            "manual modes are inactive");
    require(result.flash_state == av::flash_state_unavailable, "no flash");
    meta.zoom_ratio_q16 = 0;
    require(result_of(meta, request, false).zoom_ratio == 1.0f, "a record without zoom says 1.0");
}

void check_exif()
{
    std::vector<std::uint8_t> jpeg(80'000, 0xAA);
    const std::uint32_t offset = 20;
    jpeg[offset - 4] = 0xFF;
    jpeg[offset - 3] = 0xE1;
    std::fill(jpeg.begin() + offset, jpeg.begin() + offset + abi::exif_gap_length, 0);
    const std::array<std::uint8_t, 8> exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I'};
    require(place_exif(jpeg, offset, abi::exif_gap_length, exif), "the Exif data fits");
    require(std::memcmp(jpeg.data() + offset, exif.data(), exif.size()) == 0, "it is at the start of the gap");
    require(jpeg[offset + exif.size()] == 0 && jpeg[offset + abi::exif_gap_length - 1] == 0, "the rest is zero");
    require(jpeg[offset + abi::exif_gap_length] == 0xAA && jpeg[offset - 5] == 0xAA, "nothing outside the gap changes");

    // A shorter one over an older, longer one clears the remainder.
    std::fill(jpeg.begin() + offset, jpeg.begin() + offset + abi::exif_gap_length, 0x55);
    require(place_exif(jpeg, offset, abi::exif_gap_length, exif), "again");
    require(jpeg[offset + exif.size() + 100] == 0, "the old bytes are gone");

    std::vector<std::uint8_t> big(abi::exif_gap_length + 1, 1);
    auto copy = jpeg;
    require(!place_exif(copy, offset, abi::exif_gap_length, big) && copy == jpeg, "too much Exif data changes nothing");
    require(!place_exif(copy, offset, abi::exif_gap_length - 1, exif), "a gap of another length is not the device's");
    require(!place_exif(copy, 79'000, abi::exif_gap_length, exif), "a gap that overruns the buffer");
    require(!place_exif(copy, 2, abi::exif_gap_length, exif), "no room for the segment header");
    copy[offset - 3] = 0xE0;
    require(!place_exif(copy, offset, abi::exif_gap_length, exif), "the segment must be an APP1");
    require(place_exif(jpeg, offset, abi::exif_gap_length, {}), "an empty Exif clears the gap");

    std::vector<std::uint8_t> buffer(1000, 0);
    require(write_blob_header(buffer, 900), "the header fits");
    JpegBlobHeader header{};
    std::memcpy(&header, buffer.data() + buffer.size() - sizeof(header), sizeof(header));
    require(header.blob_id == 0x00FF && header.jpeg_size == 900, "the blob header");
    require(!write_blob_header(buffer, 993), "a picture that leaves no room for the header");
    require(write_blob_header(buffer, 992), "the largest picture");
    std::vector<std::uint8_t> tiny(4, 0);
    require(!write_blob_header(tiny, 0), "a buffer smaller than the header");
}

} // namespace

int main()
{
    check_automatic();
    check_manual();
    check_focus_and_locks();
    check_other_controls();
    check_regions();
    check_results();
    check_exif();
    return 0;
}
