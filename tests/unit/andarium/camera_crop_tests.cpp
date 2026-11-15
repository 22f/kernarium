// SPDX-License-Identifier: GPL-2.0-or-later
//
// The crop region a result of the camera HAL states
// (android/lineage-24/overlay/camera/hal/camera_crop.hpp). The camera service
// splits an application's zoom into the ratio and a residual crop region and
// joins them in the result, so the result must hand back the residual the request
// carried; the camera is CENTER_ONLY, so that residual is the centred region of
// the requested size, clamped to the array and to the smallest size the maximum
// digital zoom allows (times the request's ratio). The rows hold the residual that
// CTS testDigitalZoom made on a 4032 x 3024 front camera, the asymmetric request
// of the HAL review (a full-width, half-height region reports top 756), and the
// cases around them: no region, a short entry, an empty or a negative size, a
// region that sticks out of the array or lies outside it, the minimum size with
// and without a zoom ratio, an array with an origin, and sums that overflow.
#include <cstddef>
#include <cstdint>
#include <limits>

#include "camera_crop.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

constexpr CropRegion array{0, 0, 4032, 3024};
// A camera that advertises 8x; the CTS front camera of the first rows advertises 100x.
constexpr double eight = 8.0;
constexpr double hundred = 100.0;

bool is(const CropRegion& region, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height)
{
    return region.left == left && region.top == top && region.width == width && region.height == height;
}

CropRegion ask(const std::int32_t (&region)[4], double ratio = 1.0, double maximum = eight,
               const CropRegion& within = array)
{
    return result_crop_region(region, 4, within, ratio, maximum);
}

void check_the_residual_of_a_high_zoom_comes_back_centred()
{
    // CTS testDigitalZoom step 12 on camera 1: the residual crop region the camera service sent with the ratio 77.5385.
    // The camera centres it, and the result states what the camera applied: the size is the request's.
    const std::int32_t step12[4] = {77, 115, 3955, 2870};
    require(is(ask(step12, 77.5385, hundred), 38, 77, 3955, 2870), "the residual of a 75.8x zoom, centred, at its size");
    const std::int32_t asymmetric[4] = {71, 90, 3961, 2934};
    require(is(ask(asymmetric, 77.5385, hundred), 35, 45, 3961, 2934), "an origin that is not centred is not kept");
    const std::int32_t whole[4] = {0, 0, 4032, 3024};
    require(is(ask(whole), 0, 0, 4032, 3024), "a request for the whole array is the whole array");
}

void check_an_asymmetric_request_is_recentred()
{
    // The HAL review's request: zoom ratio 1, the top half of a 4032 x 3024 array. A CENTER_ONLY camera cannot apply it.
    const std::int32_t top_half[4] = {0, 0, 4032, 1512};
    require(is(ask(top_half), 0, 756, 4032, 1512), "the same size, centred: top 756");
    const std::int32_t left_half[4] = {0, 0, 2016, 3024};
    require(is(ask(left_half), 1008, 0, 2016, 3024), "and the left half is the middle half");
    const std::int32_t inside[4] = {1000, 800, 2000, 1500};
    require(is(ask(inside), 1016, 762, 2000, 1500), "a region inside the array is centred as well");
    const std::int32_t odd[4] = {0, 0, 3955, 2870};
    require(is(ask(odd), 38, 77, 3955, 2870), "an odd remainder goes to the right and the bottom");
}

void check_what_the_request_does_not_state()
{
    require(is(result_crop_region(nullptr, 0, array, 1.0, eight), 0, 0, 4032, 3024), "no entry: the array");
    const std::int32_t entry[4] = {10, 10, 1000, 1000};
    require(is(result_crop_region(entry, 0, array, 1.0, eight), 0, 0, 4032, 3024), "an entry with no values: the array");
    require(is(result_crop_region(entry, 3, array, 1.0, eight), 0, 0, 4032, 3024), "a short entry: the array");
    require(is(result_crop_region(nullptr, 4, array, 1.0, eight), 0, 0, 4032, 3024), "values that are not there: the array");
}

void check_an_unusable_size_falls_back_to_the_array()
{
    const std::int32_t empty[4] = {10, 10, 0, 100};
    require(is(ask(empty), 0, 0, 4032, 3024), "no width: the array");
    const std::int32_t flat[4] = {10, 10, 100, 0};
    require(is(ask(flat), 0, 0, 4032, 3024), "no height: the array");
    const std::int32_t negative[4] = {10, 10, -5, 100};
    require(is(ask(negative), 0, 0, 4032, 3024), "a negative width: the array");
    constexpr CropRegion nothing{0, 0, 0, 0};
    const std::int32_t some[4] = {0, 0, 100, 100};
    require(is(result_crop_region(some, 4, nothing, 1.0, eight), 0, 0, 0, 0), "an empty array stays empty");
}

void check_a_region_partly_or_wholly_outside_is_centred_at_its_size()
{
    const std::int32_t partly[4] = {-10, -20, 1000, 800};
    require(is(ask(partly), 1516, 1112, 1000, 800), "a region that starts before the array keeps its size");
    const std::int32_t past_the_edge[4] = {4000, 3000, 1000, 800};
    require(is(ask(past_the_edge), 1516, 1112, 1000, 800), "and one that ends past it");
    const std::int32_t right[4] = {4032, 0, 1000, 800};
    require(is(ask(right), 1516, 1112, 1000, 800), "and one that lies right of it, as the origin is not used");
    const std::int32_t larger[4] = {-5, -5, 5000, 4000};
    require(is(ask(larger), 0, 0, 4032, 3024), "a region around the array is cut to the array");
    const std::int32_t one_side[4] = {0, 0, 5000, 1000};
    require(is(ask(one_side), 0, 1012, 4032, 1000), "and a side that is too long is cut alone");
}

void check_the_smallest_size_follows_the_maximum_zoom_and_the_ratio()
{
    const std::int32_t tiny[4] = {0, 0, 1, 1};
    // 4032 / 8 = 504 and 3024 / 8 = 378.
    require(is(ask(tiny), 1764, 1323, 504, 378), "the minimum size: the array over the maximum zoom");
    const std::int32_t just_below[4] = {0, 0, 503, 377};
    require(is(ask(just_below), 1764, 1323, 504, 378), "a size just below it is the minimum");
    const std::int32_t at[4] = {0, 0, 504, 378};
    require(is(ask(at), 1764, 1323, 504, 378), "and one at it stays");
    const std::int32_t above[4] = {0, 0, 505, 379};
    require(is(ask(above), 1763, 1322, 505, 379), "one above it stays");
    // With a ratio of 2 the camera service reports twice the zoom of the region: the minimum doubles (1008 x 756).
    require(is(ask(tiny, 2.0), 1512, 1134, 1008, 756), "the minimum size scales with the request's zoom ratio");
    const std::int32_t middling[4] = {0, 0, 900, 700};
    require(is(ask(middling, 2.0), 1512, 1134, 1008, 756), "a size below the scaled minimum is raised to it");
    require(is(ask(middling, 1.0), 1566, 1162, 900, 700), "and is kept at the ratio 1");
    // At the maximum ratio, or past it, no crop is left to make.
    require(is(ask(tiny, 8.0), 0, 0, 4032, 3024), "at the maximum ratio the region is the array");
    require(is(ask(tiny, 20.0), 0, 0, 4032, 3024), "past it too");
    // A ratio that is not positive counts as 1; one below 1 (a camera that zooms out) lowers the minimum.
    require(is(ask(tiny, 0.0), 1764, 1323, 504, 378), "a ratio of zero counts as 1");
    require(is(ask(tiny, -3.0), 1764, 1323, 504, 378), "and a negative one");
    require(is(ask(tiny, 0.5), 1890, 1417, 252, 189), "a ratio of one half halves the minimum");
    // A camera without digital zoom (maximum 1, or none stated) can crop nothing.
    require(is(ask(tiny, 1.0, 1.0), 0, 0, 4032, 3024), "a maximum zoom of 1: the array");
    require(is(ask(tiny, 1.0, 0.0), 0, 0, 4032, 3024), "none stated: the array");
    // The minimum is rounded up, so the zoom the service computes never exceeds the maximum.
    constexpr CropRegion odd{0, 0, 1001, 751};
    const std::int32_t smallest[4] = {0, 0, 1, 1};
    const CropRegion region = ask(smallest, 1.0, 3.0, odd);
    require(region.width == 334 && region.height == 251, "1001 / 3 and 751 / 3 round up");
    require(1001.0 / region.width <= 3.0 && 751.0 / region.height <= 3.0, "so the zoom stays within the maximum");
}

void check_the_array_may_have_an_origin_and_the_sums_may_overflow()
{
    constexpr CropRegion moved{100, 50, 1000, 800};
    const std::int32_t inside[4] = {200, 150, 300, 200};
    require(is(ask(inside, 1.0, eight, moved), 450, 350, 300, 200), "centred in an array that has an origin");
    const std::int32_t before[4] = {0, 0, 3000, 2000};
    require(is(ask(before, 1.0, eight, moved), 100, 50, 1000, 800), "and cut to it");
    constexpr std::int32_t big = std::numeric_limits<std::int32_t>::max();
    const std::int32_t overflow[4] = {big - 1, big - 1, big, big};
    require(is(ask(overflow), 0, 0, 4032, 3024), "a size beyond the array is the array, nothing wraps");
    const std::int32_t huge[4] = {0, 0, big, big};
    require(is(ask(huge), 0, 0, 4032, 3024), "a huge region is the array");
    constexpr CropRegion wide{big - 10, big - 10, 4, 4};
    const std::int32_t small[4] = {0, 0, 2, 2};
    require(is(result_crop_region(small, 4, wide, 1.0, eight), big - 9, big - 9, 2, 2), "an array at the end of the range is centred without overflow");
}

} // namespace

int main()
{
    check_the_residual_of_a_high_zoom_comes_back_centred();
    check_an_asymmetric_request_is_recentred();
    check_what_the_request_does_not_state();
    check_an_unusable_size_falls_back_to_the_array();
    check_a_region_partly_or_wholly_outside_is_centred_at_its_size();
    check_the_smallest_size_follows_the_maximum_zoom_and_the_ratio();
    check_the_array_may_have_an_origin_and_the_sums_may_overflow();
    return 0;
}
