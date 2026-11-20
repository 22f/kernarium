// SPDX-License-Identifier: GPL-2.0-or-later
#include "turned_still.hpp"

#include "nv12_transform.hpp"
#include "photo_dimensions.hpp"
#include "video_encoder.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace uml::platform {
namespace {

// The encoder takes a few milliseconds for a still this size; a bound that is never reached on a healthy host.
constexpr std::chrono::milliseconds kEncodeBound {5000};

struct CfRelease {
    void operator()(CFTypeRef value) const noexcept { if (value) CFRelease(value); }
};
template <typename T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, CfRelease>;

class PixelLock {
public:
    explicit PixelLock(CVPixelBufferRef pixels) : pixels_(pixels) { ok_ = CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess; }
    ~PixelLock() { if (ok_) CVPixelBufferUnlockBaseAddress(pixels_, kCVPixelBufferLock_ReadOnly); }
    PixelLock(const PixelLock&) = delete;
    PixelLock& operator=(const PixelLock&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    CVPixelBufferRef pixels_;
    bool ok_ {};
};

// The photo's colour as an ICC profile: the one it carries, or the one its colour attachments describe. Empty where it
// states none.
std::vector<std::uint8_t> colourProfileOf(CVPixelBufferRef photo)
{
    Cf<CFTypeRef> stated(CVBufferCopyAttachment(photo, kCVImageBufferICCProfileKey, nullptr));
    Cf<CFDataRef> data;
    if (stated && CFGetTypeID(stated.get()) == CFDataGetTypeID() && CFDataGetLength(static_cast<CFDataRef>(stated.get())) > 0) {
        data.reset(static_cast<CFDataRef>(stated.release()));
    } else {
        Cf<CFDictionaryRef> attachments(CVBufferCopyAttachments(photo, kCVAttachmentMode_ShouldPropagate));
        if (!attachments) return {};
        Cf<CGColorSpaceRef> space(CVImageBufferCreateColorSpaceFromAttachments(attachments.get()));
        if (!space) return {};
        data.reset(CGColorSpaceCopyICCData(space.get()));
    }
    if (!data) return {};
    const auto* bytes = CFDataGetBytePtr(data.get());
    return std::vector<std::uint8_t>(bytes, bytes + CFDataGetLength(data.get()));
}

bool statesAnotherMatrix(CVPixelBufferRef photo)
{
    Cf<CFTypeRef> matrix(CVBufferCopyAttachment(photo, kCVImageBufferYCbCrMatrixKey, nullptr));
    return matrix && !CFEqual(matrix.get(), kCVImageBufferYCbCrMatrix_ITU_R_601_4);
}

} // namespace

TurnedStill turnedStillOf(CVPixelBufferRef photo, std::uint32_t jpegQuality)
{
    TurnedStill result;
    const auto fail = [&](TurnedStillFailure failure) {
        result.jpeg.clear();
        result.failure = failure;
        return std::move(result);
    };
    if (photo == nullptr) return fail(TurnedStillFailure::noPixels);
    const std::size_t width = CVPixelBufferGetWidth(photo);
    const std::size_t height = CVPixelBufferGetHeight(photo);
    if (CVPixelBufferGetPixelFormatType(photo) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        CVPixelBufferGetPlaneCount(photo) != 2 || width == 0 || height == 0 || ((width | height) & 1) != 0 || width > 0xffff || height > 0xffff)
        return fail(TurnedStillFailure::unsupportedFormat);
    if (statesAnotherMatrix(photo)) return fail(TurnedStillFailure::notBt601);
    const std::vector<std::uint8_t> profile = colourProfileOf(photo);
    if (profile.empty()) return fail(TurnedStillFailure::noProfile);

    auto encoder = makeVideoEncoder();
    if (!encoder) return fail(TurnedStillFailure::noEncoder);
    VideoEncodeSettings settings;
    settings.codec = VideoCodec::jpeg;
    settings.profile = VideoEncodeProfile::jpegBaseline;
    settings.input = RawVideoLayout::nv12;
    settings.width = std::uint32_t(width);
    settings.height = std::uint32_t(height);
    // The samples go to the encoder as they are: a JFIF image states BT.601 over the full range, and the encoder
    // converts nothing for JPEG.
    settings.color = {VideoColorPrimaries::unspecified, VideoTransferFunction::unspecified, VideoMatrixCoefficients::bt601, true};
    settings.jpegQuality = std::clamp<std::uint32_t>(jpegQuality, 1, 100);
    if (encoder->configure(settings) != VideoEncodeStatus::ok) return fail(TurnedStillFailure::noEncoder);
    auto frame = encoder->acquireFrame();
    if (!frame || frame->planeCount() < 2) return fail(TurnedStillFailure::noEncoder);
    {
        const PixelLock lock(photo);
        if (!lock.ok()) return fail(TurnedStillFailure::noPixels);
        auto luma = frame->plane(0);
        auto chroma = frame->plane(1);
        if (frame->stride(0) < width || frame->rows(0) < height || frame->stride(1) < width || frame->rows(1) < height / 2 ||
            luma.size() < std::size_t(frame->stride(0)) * height || chroma.size() < std::size_t(frame->stride(1)) * (height / 2))
            return fail(TurnedStillFailure::noEncoder);
        turnPlaneHalf(static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(photo, 0)), CVPixelBufferGetBytesPerRowOfPlane(photo, 0),
                      luma.data(), frame->stride(0), std::uint32_t(width), std::uint32_t(height), false);
        turnPlaneHalf(static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(photo, 1)), CVPixelBufferGetBytesPerRowOfPlane(photo, 1),
                      chroma.data(), frame->stride(1), std::uint32_t(width / 2), std::uint32_t(height / 2), true);
    }
    encoder->submit(std::move(frame), 0, true);
    encoder->drain();

    std::vector<std::uint8_t> image;
    bool drained = false, errored = false;
    const auto deadline = std::chrono::steady_clock::now() + kEncodeBound;
    while (!drained && std::chrono::steady_clock::now() < deadline) {
        pollfd readable {encoder->readinessDescriptor(), POLLIN, 0};
        (void)::poll(&readable, 1, 20);
        for (auto& item : encoder->takeFrames()) {
            switch (item.kind) {
            case EncodedVideoFrameKind::frame: image = std::move(item.bytes); break;
            case EncodedVideoFrameKind::drained: drained = true; break;
            case EncodedVideoFrameKind::error: errored = true; break;
            case EncodedVideoFrameKind::parameterSets: break;
            }
        }
    }
    if (!drained) return fail(TurnedStillFailure::timedOut);
    if (errored || image.empty()) return fail(TurnedStillFailure::failed);
    auto withProfile = jpegWithColourProfile(image, profile);
    if (!withProfile) return fail(TurnedStillFailure::failed);
    result.jpeg = std::move(*withProfile);
    return result;
}

} // namespace uml::platform
