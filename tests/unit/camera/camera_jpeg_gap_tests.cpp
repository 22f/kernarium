// SPDX-License-Identifier: GPL-2.0-or-later
// The Exif gap the camera device reserves in a JPEG still: its place, its
// length field, what happens to the segments around it, and that the scan is
// left exactly as the platform made it.

#include "jpeg_gap.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace uml::camera;
using uml::testing::require;

namespace {

void marker(std::vector<std::uint8_t>& out, std::uint8_t code, const std::vector<std::uint8_t>& payload)
{
    out.push_back(0xff);
    out.push_back(code);
    const std::size_t length = payload.size() + 2;
    out.push_back(std::uint8_t(length >> 8));
    out.push_back(std::uint8_t(length));
    out.insert(out.end(), payload.begin(), payload.end());
}

/* A JPEG's segments, in an order with an Exif segment, an XMP segment and a
 * JFIF segment before the tables; the scan carries a byte-stuffed 0xff. */
std::vector<std::uint8_t> sample(bool withScan = true)
{
    std::vector<std::uint8_t> jpeg = {0xff, 0xd8};
    marker(jpeg, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    marker(jpeg, 0xe1, {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0});
    marker(jpeg, 0xe1, {'h', 't', 't', 'p', ':', '/', '/', 'n', 's', '.', 'a', 'd', 'o', 'b', 'e'});
    marker(jpeg, 0xdb, std::vector<std::uint8_t>(65, 3));
    marker(jpeg, 0xc0, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
    marker(jpeg, 0xc4, std::vector<std::uint8_t>(20, 1));
    if (withScan) {
        marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
        for (const std::uint8_t byte : {0x12, 0xff, 0x00, 0x34, 0xff, 0xd0, 0x56}) jpeg.push_back(byte);
        jpeg.push_back(0xff);
        jpeg.push_back(0xd9);
    }
    return jpeg;
}

std::size_t find(const std::vector<std::uint8_t>& bytes, std::uint8_t code, std::size_t from = 2)
{
    for (std::size_t at = from; at + 1 < bytes.size(); ++at)
        if (bytes[at] == 0xff && bytes[at + 1] == code) return at;
    return std::size_t(-1);
}

void testGap()
{
    const auto jpeg = sample();
    const auto plan = reserveExifGap(jpeg);
    require(plan.has_value(), "a JPEG with a scan gets a gap");
    require(plan->gapLength == exifGapLength && plan->gapOffset == 6, "the gap starts after the segment header");
    require(plan->head[0] == 0xff && plan->head[1] == 0xd8 && plan->head[2] == 0xff && plan->head[3] == 0xe1,
            "the Exif segment follows the start of image");
    require(((plan->head[4] << 8) | plan->head[5]) == exifGapLength + 2, "the length field covers the payload and itself");
    require(std::memcmp(plan->head.data() + plan->gapOffset, "Exif\0\0", 6) == 0, "the payload starts as an Exif header");
    for (std::size_t at = plan->gapOffset + 6; at < plan->gapOffset + plan->gapLength; ++at)
        require(plan->head[at] == 0, "the rest of the gap is zero");
}

void testSegmentsAroundTheGap()
{
    const auto jpeg = sample();
    const auto plan = reserveExifGap(jpeg);
    require(plan.has_value(), "plan");
    // JFIF, DQT, SOF0 and DHT keep their order; both APP1 segments of the platform go.
    std::vector<std::uint8_t> rest(plan->head.begin() + plan->gapOffset + plan->gapLength, plan->head.end());
    const auto jfif = find(rest, 0xe0, 0), dqt = find(rest, 0xdb, 0), sof = find(rest, 0xc0, 0), dht = find(rest, 0xc4, 0);
    require(jfif == 0 && dqt != size_t(-1) && sof != size_t(-1) && dht != size_t(-1), "the other segments are kept");
    require(jfif < dqt && dqt < sof && sof < dht, "their order is kept");
    require(find(rest, 0xe1, 0) == size_t(-1), "the platform's own APP1 segments are dropped");
    // The scan and the end of image are the platform's bytes.
    require(plan->tail.size() > 0 && plan->tail[0] == 0xff && plan->tail[1] == 0xda, "the tail starts at the scan header");
    require(plan->tail.data() >= jpeg.data() && plan->tail.data() + plan->tail.size() == jpeg.data() + jpeg.size(),
            "the tail is the platform's bytes to its end, not a copy");
    require(plan->size() == plan->head.size() + plan->tail.size(), "size is the whole file");
    require(plan->tail[plan->tail.size() - 2] == 0xff && plan->tail.back() == 0xd9, "the end of image is last");
}

void testFillBytesAndOtherLengths()
{
    auto jpeg = sample();
    // Fill bytes before a marker are legal.
    jpeg.insert(jpeg.begin() + 2, {0xff, 0xff});
    require(reserveExifGap(jpeg).has_value(), "fill bytes before a marker are accepted");
    const auto small = reserveExifGap(sample(), 1024);
    require(small && small->gapLength == 1024 && small->head[4] == 0x04 && small->head[5] == 0x02, "another gap length");
}

void testRefusals()
{
    require(!reserveExifGap({}).has_value(), "empty data");
    require(!reserveExifGap(std::vector<std::uint8_t> {0x89, 'P', 'N', 'G', 0, 0}).has_value(), "not a JPEG");
    require(!reserveExifGap(sample(false)).has_value(), "no scan");
    auto truncated = sample();
    truncated.resize(30);
    require(!reserveExifGap(truncated).has_value(), "a segment cut short");
    auto lone = std::vector<std::uint8_t> {0xff, 0xd8, 0xff, 0xd8, 0xff, 0xda, 0, 2};
    require(!reserveExifGap(lone).has_value(), "a stand-alone marker before the scan");
    require(!reserveExifGap(sample(), 4).has_value(), "a gap too short to hold an Exif header");
    require(!reserveExifGap(sample(), 70000).has_value(), "a gap longer than a segment");
}

} // namespace

int main()
{
    testGap();
    testSegmentsAroundTheGap();
    testFillBytesAndOtherLengths();
    testRefusals();
    return 0;
}
