// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A still of a camera whose pictures the host presents half around (CameraInfo::presentationTurn): the uncompressed
// photo turned and encoded as the JPEG the guest takes. The platform's own JPEG cannot be turned, because a JPEG is a
// coded picture; the pixels are therefore asked for uncompressed, turned while they are copied into the hardware
// encoder's own frame, and encoded at the guest's quality.

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace uml::platform {

// Why a photo could not be turned and encoded. Fixed words, as the rest of the still's failures: no framework message
// or value of the photo enters them.
enum class TurnedStillFailure : std::uint8_t {
    none,
    noPixels,           // the photo has no pixel buffer
    unsupportedFormat,  // the pixels are not two planes of full-range 4:2:0, or a side is odd
    notBt601,           // the pixels state a matrix a JPEG cannot carry (JFIF is BT.601)
    noProfile,          // the pixels state no colour a profile can be made from
    noEncoder,          // the host has no hardware JPEG encoder that takes this picture
    timedOut,           // the encoder did not finish in time
    failed,             // the encoder reported an error, or its image was not a JPEG
};

[[nodiscard]] constexpr std::string_view turnedStillFailureName(TurnedStillFailure failure) noexcept
{
    switch (failure) {
    case TurnedStillFailure::none: return "none";
    case TurnedStillFailure::noPixels: return "the photo has no pixels";
    case TurnedStillFailure::unsupportedFormat: return "the photo is not full-range 4:2:0 of even size";
    case TurnedStillFailure::notBt601: return "the photo's YCbCr matrix is not BT.601";
    case TurnedStillFailure::noProfile: return "the photo states no colour space";
    case TurnedStillFailure::noEncoder: return "no hardware JPEG encoder takes the photo";
    case TurnedStillFailure::timedOut: return "the JPEG encoder did not finish in time";
    case TurnedStillFailure::failed: return "the JPEG encoder failed";
    }
    return "unknown";
}

struct TurnedStill {
    // A JFIF image of the photo's size, turned half around, with the photo's colour profile in it; empty on failure.
    std::vector<std::uint8_t> jpeg;
    TurnedStillFailure failure {TurnedStillFailure::none};
};

// Turns `photo` (full-range 4:2:0 in two planes, with BT.601 or no matrix stated) half around and encodes it at
// `jpegQuality` (1 to 100) in the hardware encoder. The photo is only read. The encoder lives for this call.
[[nodiscard]] TurnedStill turnedStillOf(CVPixelBufferRef photo, std::uint32_t jpegQuality);

} // namespace uml::platform
