// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_stabilization.hpp"

namespace uml::platform {

VideoStabilization chooseVideoStabilization(bool wanted, VideoStabilizationSupport support) noexcept
{
    if (!wanted) return VideoStabilization::off;
    if (support.lowLatency) return VideoStabilization::lowLatency;
    if (support.standard) return VideoStabilization::standard;
    return VideoStabilization::off;
}

} // namespace uml::platform
