// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng_color.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

// Builds a TIFF in either byte order: IFD0 with the colour tags and a SubIFD
// that holds the raw image's size and CFA pattern, and an Exif directory.
class Builder {
public:
    explicit Builder(bool little) : little_(little) {}

    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) { little_ ? (u8(v & 0xff), u8(v >> 8)) : (u8(v >> 8), u8(v & 0xff)); }
    void u32(std::uint32_t v)
    {
        if (little_) { u16(v & 0xffff); u16(v >> 16); } else { u16(v >> 16); u16(v & 0xffff); }
    }
    void patch32(std::size_t at, std::uint32_t v)
    {
        const auto saved = bytes_.size();
        bytes_.resize(at);
        u32(v);
        bytes_.resize(saved);
    }
    std::size_t size() const { return bytes_.size(); }
    std::vector<std::uint8_t> bytes_;

    struct Tag {
        std::uint16_t tag, type;
        std::uint32_t count;
        std::vector<std::uint8_t> payload;  // already in file byte order
    };
    std::vector<std::uint8_t> encode(std::uint32_t v)
    {
        Builder scratch(little_);
        scratch.u32(v);
        return scratch.bytes_;
    }
    std::vector<std::uint8_t> encode16(std::uint16_t v)
    {
        Builder scratch(little_);
        scratch.u16(v);
        return scratch.bytes_;
    }
    std::vector<std::uint8_t> srationals(const std::vector<std::pair<int, int>>& values)
    {
        Builder scratch(little_);
        for (const auto& [n, d] : values) { scratch.u32(std::uint32_t(n)); scratch.u32(std::uint32_t(d)); }
        return scratch.bytes_;
    }

    // Writes a directory at the current position; values over four bytes go after the entries.
    std::size_t directory(const std::vector<Tag>& tags, std::uint32_t next = 0)
    {
        const auto start = bytes_.size();
        u16(std::uint16_t(tags.size()));
        const auto tableEnd = start + 2 + tags.size() * 12 + 4;
        std::size_t spill = tableEnd;
        std::vector<std::uint8_t> spilled;
        for (const auto& t : tags) {
            u16(t.tag);
            u16(t.type);
            u32(t.count);
            if (t.payload.size() <= 4) {
                for (std::size_t i = 0; i < 4; ++i) u8(i < t.payload.size() ? t.payload[i] : 0);
            } else {
                u32(std::uint32_t(spill + spilled.size()));
                spilled.insert(spilled.end(), t.payload.begin(), t.payload.end());
            }
        }
        u32(next);
        bytes_.insert(bytes_.end(), spilled.begin(), spilled.end());
        return start;
    }

private:
    bool little_;
};

std::vector<std::uint8_t> makeDng(bool little, bool withColor = true)
{
    Builder b(little);
    // Header.
    if (little) { b.u8('I'); b.u8('I'); } else { b.u8('M'); b.u8('M'); }
    b.u16(42);
    b.u32(8);

    // Lay out the Exif and SubIFD directories after IFD0 by writing them first
    // into a scratch builder at known offsets: simplest is to write IFD0 last.
    // IFD0 lives at 8; it is built after the others, whose offsets follow it.
    // Build the child directories into a second builder placed after a
    // reserved IFD0 region of fixed size.
    const std::size_t reserved = 600;
    Builder children(little);
    children.bytes_.assign(8 + reserved, 0);

    const auto exifOffset = children.directory({{37386, 5, 1, children.srationals({{570, 100}})}});
    // 570/100 above is a rational with the unsigned layout: same bytes.
    const auto subOffset = children.directory({
        {256, 4, 1, children.encode(4032)},
        {257, 4, 1, children.encode(3024)},
        {33422, 1, 4, {2, 1, 1, 0}},  // BGGR
    });

    std::vector<Builder::Tag> tags;
    tags.push_back({330, 4, 1, children.encode(std::uint32_t(subOffset))});
    tags.push_back({34665, 4, 1, children.encode(std::uint32_t(exifOffset))});
    if (withColor) {
        std::vector<std::pair<int, int>> m1, m2, f1, f2;
        for (int i = 0; i < 9; ++i) {
            m1.push_back({i + 1, 10});
            m2.push_back({-(i + 1), 10});
            f1.push_back({i, 100});
            f2.push_back({100 - i, 100});
        }
        tags.push_back({50714, 3, 4, {}});
        {
            Builder v(little);
            for (int i = 0; i < 4; ++i) v.u16(std::uint16_t(1020 + i * 2));
            tags.back().payload = v.bytes_;
        }
        tags.push_back({50717, 3, 1, children.encode16(16383)});
        tags.push_back({50721, 10, 9, children.srationals(m1)});
        tags.push_back({50722, 10, 9, children.srationals(m2)});
        tags.push_back({50778, 3, 1, children.encode16(17)});
        tags.push_back({50779, 3, 1, children.encode16(21)});
        tags.push_back({50964, 10, 9, children.srationals(f1)});
        tags.push_back({50965, 10, 9, children.srationals(f2)});
    }
    // IFD0 must list its tags in ascending order.
    std::sort(tags.begin(), tags.end(), [](const auto& a, const auto& c) { return a.tag < c.tag; });
    Builder top(little);
    top.bytes_.assign(8, 0);
    top.directory(tags);
    require(top.size() <= 8 + reserved);
    std::vector<std::uint8_t> file = children.bytes_;
    std::copy(top.bytes_.begin() + 8, top.bytes_.end(), file.begin() + 8);
    std::copy(b.bytes_.begin(), b.bytes_.end(), file.begin());
    // The spilled values of IFD0 were placed relative to its own offset (8),
    // which is where it sits.
    return file;
}

void parses(bool little)
{
    const auto data = parseDngColorData(makeDng(little));
    require(data.has_value());
    require(data->bayer == BayerOrder::bggr);
    require(data->rawWidth == 4032 && data->rawHeight == 3024);
    require(data->whiteLevel == 16383);
    require(data->blackLevel == 1023);  // the mean of 1020, 1022, 1024, 1026 rounded
    require(data->illuminant1 == 17 && data->illuminant2 == 21);
    for (int i = 0; i < 9; ++i) {
        require(std::abs(data->colorMatrix1[std::size_t(i)] - (i + 1) / 10.0) < 1e-9);
        require(std::abs(data->colorMatrix2[std::size_t(i)] + (i + 1) / 10.0) < 1e-9);
        require(std::abs(data->forwardMatrix1[std::size_t(i)] - i / 100.0) < 1e-9);
        require(std::abs(data->forwardMatrix2[std::size_t(i)] - (100 - i) / 100.0) < 1e-9);
    }
    require(std::abs(data->focalLengthMillimetres - 5.7) < 1e-9);
}

void refuses()
{
    require(!parseDngColorData({}).has_value());
    auto bad = makeDng(true);
    bad[0] = 'X';
    require(!parseDngColorData(bad).has_value());
    bad = makeDng(true);
    bad[2] = 43;
    require(!parseDngColorData(bad).has_value());
    require(!parseDngColorData(makeDng(true, false)).has_value());  // no colour matrix
    // Any truncation fails cleanly rather than reading past the end.
    const auto whole = makeDng(true);
    for (std::size_t length = 0; length < whole.size(); length += 7) {
        const auto cut = std::vector<std::uint8_t>(whole.begin(), whole.begin() + std::ptrdiff_t(length));
        (void)parseDngColorData(cut);
    }
    // A directory that links to itself ends.
    std::vector<std::uint8_t> loop = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 8, 0, 0, 0};
    require(!parseDngColorData(loop).has_value());
}

} // namespace

int main()
{
    parses(true);
    parses(false);
    refuses();
    return 0;
}
