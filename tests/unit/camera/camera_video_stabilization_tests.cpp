// SPDX-License-Identifier: GPL-2.0-or-later
// Which video stabilization a camera is asked for, over every combination of what its format supports (no
// camera is opened).
#include "require.hpp"
#include "video_stabilization.hpp"

using namespace uml::platform;
using uml::testing::require;

namespace {

void aGuestThatDoesNotWantStabilizationGetsNone()
{
    for (const bool standard : {false, true})
        for (const bool lowLatency : {false, true})
            require(chooseVideoStabilization(false, {standard, lowLatency}) == VideoStabilization::off,
                    "a picture the guest left unstabilized stays so, whatever the format supports");
}

void theLowLatencyModeIsPreferredWhereTheFormatSupportsIt()
{
    require(chooseVideoStabilization(true, {true, true}) == VideoStabilization::lowLatency,
            "low latency wins over standard");
    require(chooseVideoStabilization(true, {false, true}) == VideoStabilization::lowLatency,
            "low latency alone is taken");
}

void theStandardModeIsTheFallback()
{
    require(chooseVideoStabilization(true, {true, false}) == VideoStabilization::standard,
            "standard is taken when low latency is not supported");
}

void aFormatThatSupportsNeitherIsNotStabilized()
{
    require(chooseVideoStabilization(true, {false, false}) == VideoStabilization::off,
            "no mode the format supports leaves the picture unstabilized");
}

} // namespace

int main()
{
    aGuestThatDoesNotWantStabilizationGetsNone();
    theLowLatencyModeIsPreferredWhereTheFormatSupportsIt();
    theStandardModeIsTheFallback();
    aFormatThatSupportsNeitherIsNotStabilized();
    return 0;
}
