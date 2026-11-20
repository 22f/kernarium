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

// The colour the photo states: its primaries and transfer function and, besides them, a colour space object and an ICC
// profile, each of which may be another colour space than sRGB.
enum class Primaries { none, bt709, p3, smpteC };
enum class Transfer { asPrimaries, none, bt709, srgb, linear };
enum class Space { none, srgb, bt709, p3, smpteC, linear };

struct Tags {
    bool matrix601 = true;
    bool matrix709 = false;
    Primaries primaries = Primaries::bt709;
    Transfer transfer = Transfer::asPrimaries;   // BT.709 when the primaries are stated, else none
    Space space = Space::none;                   // kCVImageBufferCGColorSpaceKey
    Space profile = Space::none;                 // kCVImageBufferICCProfileKey
};

Owned<CGColorSpaceRef> colourSpaceOf(Space space)
{
    switch (space) {
    case Space::srgb: return Owned<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    case Space::bt709: return Owned<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceITUR_709));
    case Space::p3: return Owned<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3));
    case Space::linear: return Owned<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB));
    case Space::smpteC: {
        // SMPTE-C primaries over D65, with a 2.2 gamma: a colour space that is not sRGB and has no name of its own.
        const CGFloat white[3] = {0.9505, 1.0, 1.089};
        const CGFloat gamma[3] = {2.2, 2.2, 2.2};
        const CGFloat matrix[9] = {0.3936, 0.2124, 0.0187, 0.3653, 0.7011, 0.1119, 0.1916, 0.0865, 0.9582};
        return Owned<CGColorSpaceRef>(CGColorSpaceCreateCalibratedRGB(white, nullptr, gamma, matrix));
    }
    case Space::none: break;
    }
    return Owned<CGColorSpaceRef>();
}

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
        CFStringRef primaries = tags.primaries == Primaries::p3 ? kCVImageBufferColorPrimaries_P3_D65
            : tags.primaries == Primaries::smpteC ? kCVImageBufferColorPrimaries_SMPTE_C : kCVImageBufferColorPrimaries_ITU_R_709_2;
        CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey, primaries, kCVAttachmentMode_ShouldPropagate);
    }
    const Transfer transfer = tags.transfer == Transfer::asPrimaries ? (tags.primaries == Primaries::none ? Transfer::none : Transfer::bt709) : tags.transfer;
    if (transfer != Transfer::none) {
        CFStringRef function = transfer == Transfer::srgb ? kCVImageBufferTransferFunction_sRGB
            : transfer == Transfer::linear ? kCVImageBufferTransferFunction_Linear : kCVImageBufferTransferFunction_ITU_R_709_2;
        CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, function, kCVAttachmentMode_ShouldPropagate);
    }
    if (tags.space != Space::none) {
        const auto space = colourSpaceOf(tags.space);
        CVBufferSetAttachment(pixels, kCVImageBufferCGColorSpaceKey, space.get(), kCVAttachmentMode_ShouldPropagate);
    }
    if (tags.profile != Space::none) {
        const auto space = colourSpaceOf(tags.profile);
        Owned<CFDataRef> icc(CGColorSpaceCopyICCData(space.get()));
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
    const auto turned = turnedStillOf(photo.get(), 90, 0, true);
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
    const auto rough = turnedStillOf(photo.get(), 10, 0, true);
    require(rough.failure == TurnedStillFailure::none && rough.jpeg.size() < turned.jpeg.size(), "a lower quality is a smaller file");
    // A photo that states no colour at all is as the platform's own files were: untagged, and delivered when the capture was
    // held in sRGB.
    Tags bare;
    bare.primaries = Primaries::none;
    const auto plain = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bare);
    require(turnedStillOf(plain.get(), 90, 0, true).failure == TurnedStillFailure::none, "a photo that states no colour is delivered untagged");
    // A photo that states its colours as sRGB's or BT.709's is delivered whatever the capture's colour space was.
    const auto delivered = [&](const char* what, const Tags& tags) {
        const auto photo = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, tags);
        require(turnedStillOf(photo.get(), 90, 0, false).failure == TurnedStillFailure::none, what);
    };
    delivered("BT.709 primaries over the BT.709 transfer", {});
    Tags tags;
    tags.transfer = Transfer::srgb;
    delivered("BT.709 primaries over the sRGB transfer", tags);
    tags = {};
    tags.primaries = Primaries::none;
    tags.space = Space::srgb;
    delivered("an sRGB colour space object alone", tags);
    tags.space = Space::bt709;
    delivered("a BT.709 colour space object", tags);
    tags = {};
    tags.primaries = Primaries::none;
    tags.profile = Space::srgb;
    delivered("an sRGB ICC profile alone", tags);
    tags.profile = Space::bt709;
    delivered("a BT.709 ICC profile", tags);
    std::printf("  %zux%zu photo: %zu bytes at quality 90, %zu at 10, no profile\n", kWidth, kHeight, turned.jpeg.size(), rough.jpeg.size());
}

// The size bound is the encoder's: a file past it is an error and its bytes are not copied out.
void aFileOverTheBoundIsNotDelivered()
{
    // Noise at the top quality is the largest file the encoder makes of a picture this size.
    Owned<CVPixelBufferRef> noisy;
    constexpr std::size_t width = 504, height = 378;
    require(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, nullptr, noisy.put()) == kCVReturnSuccess);
    require(CVPixelBufferLockBaseAddress(noisy.get(), 0) == kCVReturnSuccess);
    std::uint32_t seed = 12345;
    for (std::size_t plane = 0; plane < 2; ++plane) {
        auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(noisy.get(), plane));
        const std::size_t stride = CVPixelBufferGetBytesPerRowOfPlane(noisy.get(), plane);
        const std::size_t rows = plane == 0 ? height : height / 2;
        const std::size_t bytes = plane == 0 ? width : width;
        for (std::size_t y = 0; y < rows; ++y)
            for (std::size_t x = 0; x < bytes; ++x) {
                seed = seed * 1664525u + 1013904223u;
                base[y * stride + x] = std::uint8_t(seed >> 24);
            }
    }
    CVPixelBufferUnlockBaseAddress(noisy.get(), 0);
    CVBufferSetAttachment(noisy.get(), kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
    const auto whole = turnedStillOf(noisy.get(), 100, 0, true);
    require(whole.failure == TurnedStillFailure::none && whole.jpeg.size() > 100000, "the noisy picture makes a large file");
    const std::uint64_t exact = whole.jpeg.size();
    require(turnedStillOf(noisy.get(), 100, exact, true).failure == TurnedStillFailure::none, "a file at the bound is delivered");
    const auto over = turnedStillOf(noisy.get(), 100, exact - 1, true);
    require(over.failure == TurnedStillFailure::failed && over.jpeg.empty(), "a file one byte over is not");
    require(turnedStillOf(noisy.get(), 100, exact / 2, true).failure == TurnedStillFailure::failed, "nor is one far over");
}

void aPhotoThatCannotBeTurnedIsRefused()
{
    require(turnedStillOf(nullptr, 90, 0, true).failure == TurnedStillFailure::noPixels, "no photo");
    const auto video = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, {});
    require(turnedStillOf(video.get(), 90, 0, true).failure == TurnedStillFailure::unsupportedFormat, "video range is not turned");
    const auto odd = photoOf(kWidth + 1, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, {});
    require(turnedStillOf(odd.get(), 90, 0, true).failure == TurnedStillFailure::unsupportedFormat, "an odd side is not");
    Tags bt709;
    bt709.matrix601 = false;
    bt709.matrix709 = true;
    const auto other = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bt709);
    require(turnedStillOf(other.get(), 90, 0, true).failure == TurnedStillFailure::notBt601, "a BT.709 photo is refused: a JPEG would state BT.601");
    // The colours are verified sRGB or the photo is not delivered: other primaries, another transfer, or a colour space object
    // or a profile that is not sRGB's (BT.709's and sRGB's primaries are the same, and either transfer is accepted).
    const auto refused = [&](const char* what, const Tags& tags) {
        const auto photo = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, tags);
        // Whatever the capture's colour space is: what the photo states is what counts.
        require(turnedStillOf(photo.get(), 90, 0, true).failure == TurnedStillFailure::notSrgb, what);
    };
    Tags tags;
    tags.primaries = Primaries::p3;
    refused("Display P3 primaries are refused", tags);
    tags = {};
    tags.primaries = Primaries::smpteC;
    refused("SMPTE-C primaries are refused", tags);
    tags = {};
    tags.transfer = Transfer::linear;
    refused("BT.709 primaries over a linear transfer are refused: the untagged file would be read as sRGB", tags);
    for (const Space other : {Space::p3, Space::smpteC, Space::linear}) {
        tags = {};
        tags.primaries = Primaries::none;
        tags.space = other;
        refused("a colour space object that is not sRGB's is refused", tags);
        tags = {};
        tags.primaries = Primaries::none;
        tags.profile = other;
        refused("and so is an ICC profile that is not", tags);
    }
    tags = {};
    tags.primaries = Primaries::none;
    tags.profile = Space::srgb;
    tags.space = Space::smpteC;
    refused("one colour space that is not sRGB's decides, whatever the others say", tags);

    // A photo that states no colours is taken as the capture's own sRGB only where the capture is held in sRGB.
    Tags bare;
    bare.primaries = Primaries::none;
    const auto unstated = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bare);
    require(turnedStillOf(unstated.get(), 90, 0, false).failure == TurnedStillFailure::unknownColours, "a photo that states no colours is refused when its capture was not sRGB");
    Tags half;
    half.transfer = Transfer::none;   // BT.709 primaries and no transfer
    const auto partial = photoOf(kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, half);
    require(turnedStillOf(partial.get(), 90, 0, false).failure == TurnedStillFailure::unknownColours, "so is one that states the primaries and no transfer");
    for (const auto failure : {TurnedStillFailure::noPixels, TurnedStillFailure::unsupportedFormat, TurnedStillFailure::notBt601,
                               TurnedStillFailure::notSrgb, TurnedStillFailure::unknownColours, TurnedStillFailure::noEncoder, TurnedStillFailure::timedOut, TurnedStillFailure::failed})
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
    aFileOverTheBoundIsNotDelivered();
    return 0;
}
