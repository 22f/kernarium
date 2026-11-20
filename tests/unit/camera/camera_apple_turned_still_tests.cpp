// SPDX-License-Identifier: GPL-2.0-or-later
// A still of a camera presented half around, on a pixel buffer standing in for the photo: the hardware encoder's JPEG is
// the photo turned half around, a JFIF image of the photo's size with no colour profile (as the platform's own files had
// none: a reader takes sRGB), and a photo that cannot be turned, or whose colours are not sRGB, is refused with a fixed
// word. A host without a hardware JPEG encoder skips the encodes.
#include "apple/turned_still.hpp"
#include "require.hpp"
#include "video_encoder.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

template <class Ref>
class Owned {
public:
    explicit Owned(Ref ref = nullptr) : ref_(ref) {}
    Owned(Owned&& other) noexcept : ref_(other.ref_) { other.ref_ = nullptr; }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { if (ref_ != nullptr) CFRelease(ref_); }
    [[nodiscard]] Ref get() const { return ref_; }
    [[nodiscard]] Ref* put() { return &ref_; }
private:
    Ref ref_;
};

constexpr std::size_t kWidth = 120;
constexpr std::size_t kHeight = 96;

// The colour the photo states: its primaries and, besides them, an object or an ICC profile of its colour space.
enum class Primaries { none, bt709, p3 };

struct Tags {
    bool matrix601 = true;
    bool matrix709 = false;
    Primaries primaries = Primaries::bt709;
    bool p3Space = false;   // kCVImageBufferCGColorSpaceKey: Display P3
    bool p3Profile = false; // kCVImageBufferICCProfileKey: Display P3
};

// The upper half of the picture ramps up along x (16 to 254) and the lower half is flat grey; the left half of the
// picture is red and the right half has no colour. Turned half around, the ramp is in the lower half and falls, and the
// colour is on the right.
Owned<CVPixelBufferRef> photoOf(std::size_t width, std::size_t height, OSType format, const Tags& tags)
{
    Owned<CVPixelBufferRef> buffer;
    require(CVPixelBufferCreate(nullptr, width, height, format, nullptr, buffer.put()) == kCVReturnSuccess, "a pixel buffer");
    CVPixelBufferRef pixels = buffer.get();
    require(CVPixelBufferLockBaseAddress(pixels, 0) == kCVReturnSuccess);
    auto* luma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
    const std::size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            luma[y * lumaStride + x] = y < height / 2 ? std::uint8_t(std::min<std::size_t>(16 + 2 * x, 254)) : std::uint8_t(128);
    auto* chroma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
    const std::size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
    for (std::size_t y = 0; y < height / 2; ++y)
        for (std::size_t x = 0; x < width / 2; ++x) {
            const bool left = x < width / 4;
            chroma[y * chromaStride + 2 * x] = left ? 90 : 128;
            chroma[y * chromaStride + 2 * x + 1] = left ? 200 : 128;
        }
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    if (tags.matrix601) CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
    if (tags.matrix709) CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    if (tags.primaries != Primaries::none) {
        CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey,
                              tags.primaries == Primaries::p3 ? kCVImageBufferColorPrimaries_P3_D65 : kCVImageBufferColorPrimaries_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    }
    if (tags.p3Space) {
        Owned<CGColorSpaceRef> p3(CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3));
        CVBufferSetAttachment(pixels, kCVImageBufferCGColorSpaceKey, p3.get(), kCVAttachmentMode_ShouldPropagate);
    }
    if (tags.p3Profile) {
        Owned<CGColorSpaceRef> p3(CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3));
        Owned<CFDataRef> icc(CGColorSpaceCopyICCData(p3.get()));
        CVBufferSetAttachment(pixels, kCVImageBufferICCProfileKey, icc.get(), kCVAttachmentMode_ShouldPropagate);
    }
    return buffer;
}

// The decoded image as 8-bit sRGB, four bytes a pixel.
std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& jpeg, std::size_t width, std::size_t height)
{
    Owned<CFDataRef> data(CFDataCreate(nullptr, jpeg.data(), CFIndex(jpeg.size())));
    Owned<CGImageSourceRef> source(CGImageSourceCreateWithData(data.get(), nullptr));
    require(source.get() != nullptr, "ImageIO reads the file");
    Owned<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr));
    require(image.get() != nullptr, "ImageIO decodes it");
    require(CGImageGetWidth(image.get()) == width && CGImageGetHeight(image.get()) == height, "the image is the photo's size");
    Owned<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    std::vector<std::uint8_t> rgba(width * height * 4);
    Owned<CGContextRef> context(CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space.get(), kCGImageAlphaNoneSkipLast));
    CGContextDrawImage(context.get(), CGRectMake(0, 0, double(width), double(height)), image.get());
    return rgba;
}

int greyAt(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t x, std::size_t y)
{
    const auto* pixel = &rgba[(y * width + x) * 4];
    return (pixel[0] + pixel[1] + pixel[2]) / 3;
}

int redness(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t x, std::size_t y)
{
    const auto* pixel = &rgba[(y * width + x) * 4];
    return int(pixel[0]) - int(pixel[1]);
}

bool hasHardwareJpeg()
{
    for (const auto& capability : videoEncodeCapabilities())
        if (capability.codec == VideoCodec::jpeg) return true;
    return false;
}

// The segments of a JPEG up to the scan: (marker, payload).
std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> segmentsOf(const std::vector<std::uint8_t>& jpeg)
{
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> segments;
    for (std::size_t at = 2; at + 4 <= jpeg.size() && jpeg[at] == 0xff && jpeg[at + 1] != 0xda;) {
        const std::size_t length = (std::size_t(jpeg[at + 2]) << 8) | jpeg[at + 3];
        segments.emplace_back(jpeg[at + 1], std::vector<std::uint8_t>(jpeg.begin() + std::ptrdiff_t(at + 4), jpeg.begin() + std::ptrdiff_t(at + 2 + length)));
        at += 2 + length;
    }
    return segments;
}

void aPhotoIsTurnedHalfAroundWithNoProfile()
{
    const auto photo = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, {});
    const auto turned = turnedStillOf(photo.get(), 90);
    require(turned.failure == TurnedStillFailure::none, "the photo is turned and encoded");
    require(turned.jpeg.size() > 4 && turned.jpeg[0] == 0xff && turned.jpeg[1] == 0xd8, "a JPEG");
    const auto segments = segmentsOf(turned.jpeg);
    require(segments.size() >= 2 && segments[0].first == 0xe0 && std::memcmp(segments[0].second.data(), "JFIF", 4) == 0, "it starts with the JFIF segment");
    for (const auto& segment : segments) require(segment.first != 0xe2, "no colour profile: the file is read as sRGB, as the platform's own were");

    const auto rgba = decode(turned.jpeg, kWidth, kHeight);
    // The flat grey is now in the upper half, and the ramp in the lower half falls from left to right.
    require(std::abs(greyAt(rgba, kWidth, 10, 6) - greyAt(rgba, kWidth, 110, 6)) < 24, "the upper half is flat");
    require(greyAt(rgba, kWidth, 10, 90) > greyAt(rgba, kWidth, 110, 90) + 120, "the ramp falls: the rows are turned");
    // The red left half is the right half.
    require(redness(rgba, kWidth, 100, 40) > 40 && redness(rgba, kWidth, 100, 10) > 40, "the colour is on the right");
    require(std::abs(redness(rgba, kWidth, 20, 40)) < 25, "and the left has none");

    // The quality is the caller's.
    const auto rough = turnedStillOf(photo.get(), 10);
    require(rough.failure == TurnedStillFailure::none && rough.jpeg.size() < turned.jpeg.size(), "a lower quality is a smaller file");
    // A photo that states no colour at all is as the platform's own files were: untagged, and delivered.
    Tags bare;
    bare.primaries = Primaries::none;
    const auto plain = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bare);
    require(turnedStillOf(plain.get(), 90).failure == TurnedStillFailure::none, "a photo that states no colour is delivered untagged");
    std::printf("  %zux%zu photo: %zu bytes at quality 90, %zu at 10, no profile\n", kWidth, kHeight, turned.jpeg.size(), rough.jpeg.size());
}

void aPhotoThatCannotBeTurnedIsRefused()
{
    require(turnedStillOf(nullptr, 90).failure == TurnedStillFailure::noPixels, "no photo");
    const auto video = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, {});
    require(turnedStillOf(video.get(), 90).failure == TurnedStillFailure::unsupportedFormat, "video range is not turned");
    const auto odd = photoOf(kWidth + 1, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, {});
    require(turnedStillOf(odd.get(), 90).failure == TurnedStillFailure::unsupportedFormat, "an odd side is not");
    Tags bt709;
    bt709.matrix601 = false;
    bt709.matrix709 = true;
    const auto other = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bt709);
    require(turnedStillOf(other.get(), 90).failure == TurnedStillFailure::notBt601, "a BT.709 photo is refused: a JPEG would state BT.601");
    // The colours are sRGB or the photo is not delivered: other primaries, a wide-gamut colour space object or profile.
    Tags p3;
    p3.primaries = Primaries::p3;
    const auto widePrimaries = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, p3);
    require(turnedStillOf(widePrimaries.get(), 90).failure == TurnedStillFailure::notSrgb, "Display P3 primaries are refused");
    Tags space;
    space.matrix601 = true;
    space.primaries = Primaries::none;
    space.p3Space = true;
    const auto wideSpace = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, space);
    require(turnedStillOf(wideSpace.get(), 90).failure == TurnedStillFailure::notSrgb, "so is a Display P3 colour space object");
    Tags profile;
    profile.primaries = Primaries::none;
    profile.p3Profile = true;
    const auto wideProfile = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, profile);
    require(turnedStillOf(wideProfile.get(), 90).failure == TurnedStillFailure::notSrgb, "and a Display P3 profile");
    for (const auto failure : {TurnedStillFailure::noPixels, TurnedStillFailure::unsupportedFormat, TurnedStillFailure::notBt601,
                               TurnedStillFailure::notSrgb, TurnedStillFailure::noEncoder, TurnedStillFailure::timedOut, TurnedStillFailure::failed})
        require(!turnedStillFailureName(failure).empty() && turnedStillFailureName(failure) != "unknown");
}

} // namespace

int main()
{
    aPhotoThatCannotBeTurnedIsRefused();
    if (!hasHardwareJpeg()) {
        std::puts("no hardware JPEG encoder on this host; the encodes are skipped");
        return 0;
    }
    aPhotoIsTurnedHalfAroundWithNoProfile();
    return 0;
}
