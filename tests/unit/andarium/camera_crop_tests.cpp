// SPDX-License-Identifier: GPL-2.0-or-later
//
// The crop region a result of the camera HAL states
// (android/lineage-24/overlay/camera/hal/camera_crop.hpp). The camera service
// splits an application's zoom into the ratio and a residual crop region and
// joins them in the result, so the result must hand back the residual the request
// carried. The rows hold the residual that CTS testDigitalZoom made on a 4032 x 3024
// front camera, where the result used to state the whole array, and the cases
// around it: no region, a short entry, an empty or a negative size, a region
// that sticks out of the array or lies outside it, and sums that overflow.
#include <cstddef>
#include <cstdint>
#include <limits>

#include "camera_crop.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

constexpr CropRegion array{0, 0, 4032, 3024};

bool is(const CropRegion& region, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height)
{
    return region.left == left && region.top == top && region.width == width && region.height == height;
}

CropRegion ask(const std::int32_t (&region)[4], const CropRegion& within = array)
{
    return result_crop_region(region, 4, within);
}

void check_the_residual_of_a_high_zoom_comes_back()
{
    // CTS testDigitalZoom step 12 on camera 1: the residual crop region the camera service sent with the ratio 77.5385.
    const std::int32_t step12[4] = {77, 115, 3955, 2870};
    require(is(ask(step12), 77, 115, 3955, 2870), "the residual of a 75.8x zoom is the result's region");
    const std::int32_t asymmetric[4] = {71, 90, 3961, 2934};
    require(is(ask(asymmetric), 71, 90, 3961, 2934), "and so is one that is not centred");
    const std::int32_t whole[4] = {0, 0, 4032, 3024};
    require(is(ask(whole), 0, 0, 4032, 3024), "a request for the whole array is the whole array");
}

void check_what_the_request_does_not_state()
{
    require(is(result_crop_region(nullptr, 0, array), 0, 0, 4032, 3024), "no entry: the array");
    const std::int32_t entry[4] = {10, 10, 100, 100};
    require(is(result_crop_region(entry, 0, array), 0, 0, 4032, 3024), "an entry with no values: the array");
    require(is(result_crop_region(entry, 3, array), 0, 0, 4032, 3024), "a short entry: the array");
    require(is(result_crop_region(nullptr, 4, array), 0, 0, 4032, 3024), "values that are not there: the array");
}

void check_an_unusable_region_falls_back_to_the_array()
{
    const std::int32_t empty[4] = {10, 10, 0, 100};
    require(is(ask(empty), 0, 0, 4032, 3024), "no width: the array");
    const std::int32_t flat[4] = {10, 10, 100, 0};
    require(is(ask(flat), 0, 0, 4032, 3024), "no height: the array");
    const std::int32_t negative[4] = {10, 10, -5, 100};
    require(is(ask(negative), 0, 0, 4032, 3024), "a negative width: the array");
    const std::int32_t right[4] = {4032, 0, 100, 100};
    require(is(ask(right), 0, 0, 4032, 3024), "a region right of the array: the array");
    const std::int32_t below[4] = {0, 3024, 100, 100};
    require(is(ask(below), 0, 0, 4032, 3024), "a region below the array: the array");
    const std::int32_t left[4] = {-200, 0, 100, 100};
    require(is(ask(left), 0, 0, 4032, 3024), "a region left of the array: the array");
}

void check_a_region_that_sticks_out_is_cut_to_the_array()
{
    const std::int32_t negative_origin[4] = {-10, -20, 100, 100};
    require(is(ask(negative_origin), 0, 0, 90, 80), "an origin before the array is cut");
    const std::int32_t past_the_edge[4] = {4000, 3000, 100, 100};
    require(is(ask(past_the_edge), 4000, 3000, 32, 24), "a far edge past the array is cut");
    const std::int32_t larger[4] = {-5, -5, 5000, 4000};
    require(is(ask(larger), 0, 0, 4032, 3024), "a region around the array is the array");
}

void check_the_array_may_have_an_origin_and_the_sums_may_overflow()
{
    constexpr CropRegion moved{100, 50, 1000, 800};
    const std::int32_t inside[4] = {200, 150, 300, 200};
    require(is(ask(inside, moved), 200, 150, 300, 200), "a region inside an array with an origin stays");
    const std::int32_t before[4] = {0, 0, 300, 200};
    require(is(ask(before, moved), 100, 50, 200, 150), "and is cut to its origin");
    constexpr std::int32_t big = std::numeric_limits<std::int32_t>::max();
    const std::int32_t overflow[4] = {big - 1, big - 1, big, big};
    require(is(ask(overflow), 0, 0, 4032, 3024), "a sum beyond 32 bits is outside the array, not wrapped into it");
    const std::int32_t huge[4] = {0, 0, big, big};
    require(is(ask(huge), 0, 0, 4032, 3024), "a huge region is the array");
}

} // namespace

int main()
{
    check_the_residual_of_a_high_zoom_comes_back();
    check_what_the_request_does_not_state();
    check_an_unusable_region_falls_back_to_the_array();
    check_a_region_that_sticks_out_is_cut_to_the_array();
    check_the_array_may_have_an_origin_and_the_sums_may_overflow();
    return 0;
}
