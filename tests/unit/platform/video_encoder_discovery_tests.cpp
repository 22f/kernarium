// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/video_encoder_internal.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

/* What the encoder's discovery makes of a host that does not always answer.
 *
 * A scripted host stands in for VideoToolbox: it answers every trial encode with
 * a stream that states what was asked for, refuses what the script refuses,
 * and can be busy, which is a probe that gets no session (the status a size
 * past the host's ceiling gets as well). The tests hold that a refusal is
 * stored, that no answer never is, that a ceiling still ends, and that the
 * process asks again, a bounded number of times, for a result that was cut
 * short. No hardware is used. */

namespace {
using namespace uml::platform;
using namespace uml::platform::apple_encoder;
using uml::testing::require;

enum class Answer {
    healthy,    // an encode that finishes and states what was asked
    busy,       // no session: the status of a busy host, and of a size beyond the ceiling
    refused,    // the configuration is refused as it stands
};

struct Host {
    std::vector<Probe> asked;
    unsigned pauses {};
    unsigned listedQueries {};   // one per discovery that ran
    std::uint64_t clock {1'000'000};
    std::vector<Discovery> stored;
    std::optional<Discovery> cache;
    // The ceilings of this host: a larger picture or bitrate gets no session.
    std::uint64_t largestSamples {4096ull * 2304ull};
    std::uint64_t largestBitrate {100'000'000ull};
    unsigned sessions {8};
    std::function<Answer(const Probe&, std::size_t call)> answer = [](const Probe&, std::size_t) { return Answer::healthy; };

    static bool is(const Probe& probe, VideoEncodeProfile profile, RawVideoLayout input)
    {
        return probe.settings.profile == profile && probe.settings.input == input && probe.settings.width == 128 &&
               probe.settings.height == 96 && probe.frames == 2;
    }
    // The call index of the first probe of this kind, or none yet.
    [[nodiscard]] bool askedBefore(const Probe& probe) const
    {
        return std::any_of(asked.begin(), asked.end(), [&](const Probe& earlier) {
            return earlier.settings.profile == probe.settings.profile && earlier.settings.input == probe.settings.input &&
                   earlier.settings.width == probe.settings.width && earlier.settings.height == probe.settings.height &&
                   earlier.frames == probe.frames && earlier.settings.rateControl == probe.settings.rateControl;
        });
    }

    static unsigned profileIdc(VideoEncodeProfile profile, unsigned& constraints)
    {
        constraints = 0;
        switch (profile) {
        case VideoEncodeProfile::h264Baseline: return 66;
        case VideoEncodeProfile::h264ConstrainedBaseline: constraints = 0x40; return 66;
        case VideoEncodeProfile::h264Main: return 77;
        case VideoEncodeProfile::h264High: return 100;
        case VideoEncodeProfile::h264ConstrainedHigh: constraints = 0x0c; return 100;
        case VideoEncodeProfile::h264High422: return 122;
        case VideoEncodeProfile::h264High444Predictive: return 244;
        default: return 0;
        }
    }

    // What the hardware writes: pictures whose first one carries an SPS that
    // states the profile and the level, and whose size follows the rate control
    // parameter the way a mode that works does.
    [[nodiscard]] static Stream healthyStream(const Probe& probe)
    {
        const auto& s = probe.settings;
        Stream out;
        out.status = VideoEncodeStatus::ok;
        out.encoded = true;
        out.seconds = 0.01 + 0.001 * probe.frames;
        unsigned constraints = 0;
        const unsigned idc = profileIdc(s.profile, constraints);
        std::size_t payload = 400;
        switch (s.rateControl) {
        case VideoRateControl::constantBitrate: payload += s.bitrate / 2000; break;
        case VideoRateControl::constantQuality: payload += s.quality * 20; break;
        case VideoRateControl::constantQp: payload += (60 - s.frameQp[1]) * 50; break;
        case VideoRateControl::variableBitrate: break;
        }
        for (unsigned i = 0; i < probe.frames; ++i) {
            EncodedVideoFrame frame;
            frame.kind = EncodedVideoFrameKind::frame;
            frame.type = i == 0 ? VideoFrameType::key : VideoFrameType::predicted;
            frame.timestamp = i;
            if (i == 0) {
                const std::uint8_t sps[] = {0, 0, 0, 1, 0x67, std::uint8_t(idc), std::uint8_t(constraints),
                                            std::uint8_t(s.level ? s.level : 31)};
                frame.bytes.assign(std::begin(sps), std::end(sps));
            }
            frame.bytes.resize(frame.bytes.size() + payload, 0xab);
            out.frames.push_back(std::move(frame));
        }
        return out;
    }

    [[nodiscard]] DiscoveryHooks hooks()
    {
        DiscoveryHooks h;
        h.run = [this](const Probe& probe) {
            const auto call = asked.size();
            auto what = answer(probe, call);
            // The ceilings are the host's own, whatever the script says.
            if (what == Answer::healthy && (std::uint64_t(probe.settings.width) * probe.settings.height > largestSamples ||
                                            probe.settings.bitrate > largestBitrate))
                what = Answer::busy;
            asked.push_back(probe);
            Stream stream;
            switch (what) {
            case Answer::healthy: return healthyStream(probe);
            case Answer::busy:
                stream.status = VideoEncodeStatus::exhausted;
                stream.diagnostic = "VTCompressionSessionCreate -12903";
                return stream;
            case Answer::refused:
                stream.status = VideoEncodeStatus::unsupported;
                stream.diagnostic = "cannot set ProfileLevel";
                return stream;
            }
            return stream;
        };
        h.countSessions = [this](const VideoEncodeSettings&) { return sessions; };
        h.hardwareListed = [this](VideoCodec codec) {
            if (codec == VideoCodec::h264) ++listedQueries;
            return codec == VideoCodec::h264;
        };
        h.listedProfiles = [](VideoCodec) { return std::set<std::string> {}; };
        h.pause = [this](unsigned milliseconds) {
            ++pauses;
            clock += milliseconds;
        };
        h.now = [this] { return clock; };
        h.load = [this]() -> std::optional<Discovery> { return cache; };
        h.store = [this](const Discovery& discovery) { stored.push_back(discovery); };
        return h;
    }

    [[nodiscard]] bool askedFor(RawVideoLayout layout) const
    {
        return std::any_of(asked.begin(), asked.end(), [&](const Probe& probe) { return probe.settings.input == layout; });
    }
};

const VideoEncodeCapability* h264Of(const Discovery& discovery)
{
    const auto* entry = discovery.find(VideoCodec::h264);
    return entry ? &entry->capability : nullptr;
}

const VideoEncodeProfileCapability* profileOf(const Discovery& discovery, VideoEncodeProfile profile)
{
    const auto* capability = h264Of(discovery);
    if (!capability) return nullptr;
    for (const auto& entry : capability->profiles)
        if (entry.profile == profile) return &entry;
    return nullptr;
}

bool lists(const VideoEncodeProfileCapability& profile, RawVideoLayout layout)
{
    return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
}

// Every profile lists the two packed orders together or neither.
void requirePackedPair(const Discovery& discovery, const char* what)
{
    const auto* capability = h264Of(discovery);
    require(capability != nullptr, what);
    for (const auto& profile : capability->profiles)
        require(lists(profile, RawVideoLayout::bgra) == lists(profile, RawVideoLayout::rgba), what);
}

constexpr unsigned kUnansweredLimit = 4;   // the Prober's limit of consecutive probes with no answer
constexpr unsigned kRetries = 3;           // retries of a probe with no answer

// ---- A host that answers everything.

void healthyHost()
{
    Host host;
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete && discovery.unanswered == 0, "a host that answers everything gives a complete discovery");
    const auto* capability = h264Of(discovery);
    require(capability != nullptr, "an H.264 capability");
    require(capability->profiles.size() == 7, "every candidate profile proved");
    requirePackedPair(discovery, "a profile lists BGRA and RGBA together");
    require(profileOf(discovery, VideoEncodeProfile::h264High)->inputs.size() == 4, "nv12, i420 and both packed orders");
    // One probe decides both packed orders.
    require(host.askedFor(RawVideoLayout::bgra), "BGRA is probed");
    require(!host.askedFor(RawVideoLayout::rgba), "and RGBA is not probed on its own");
    // The ceilings of the host end the probes that reach them, and are facts.
    require(capability->maximumWidth == 4096 && capability->maximumHeight == 2304, "the size ladder ended at the host's ceiling");
    require(capability->maximumBitrate == 100'000'000ull, "the bitrate search ended at the host's ceiling");
    require(capability->concurrentSessions == 8, "sessions");
    require(capability->rateControls.size() == 4, "every rate control mode takes effect");
    // A ceiling is one refusal asked again once: three refusals, each with one pause.
    require(host.pauses == 3, "a ceiling is asked once more and then taken for what it is");
}

// ---- One probe decides the packed pair, so a failure of it takes both or neither.

void packedPairSurvivesATransientFailure()
{
    // The first answer for BGRA on Main is busy; the retry is answered.
    Host host;
    host.answer = [&host](const Probe& probe, std::size_t) {
        if (Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra) && !host.askedBefore(probe)) return Answer::busy;
        return Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete, "a probe answered on its retry leaves the discovery complete");
    const auto* main = profileOf(discovery, VideoEncodeProfile::h264Main);
    require(main && lists(*main, RawVideoLayout::bgra) && lists(*main, RawVideoLayout::rgba), "both packed orders are listed");
    requirePackedPair(discovery, "the pair");
}

void packedPairRefusedTogether()
{
    // The host refuses BGRA on High every time while it answers the others.
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return Host::is(probe, VideoEncodeProfile::h264High, RawVideoLayout::bgra) ? Answer::busy : Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete, "a refusal the host keeps making, next to probes it answers, is an answer");
    const auto* high = profileOf(discovery, VideoEncodeProfile::h264High);
    require(high && !lists(*high, RawVideoLayout::bgra) && !lists(*high, RawVideoLayout::rgba), "neither packed order is listed");
    require(lists(*high, RawVideoLayout::nv12), "the other layouts are");
    requirePackedPair(discovery, "the pair");
}

// ---- A host that stops answering.

void retriesAreBoundedAndTheDiscoveryIsIncomplete()
{
    // From the second probe on, the host answers nothing.
    Host host;
    host.answer = [](const Probe&, std::size_t call) { return call >= 1 ? Answer::busy : Answer::healthy; };
    const auto discovery = runDiscovery(host.hooks());
    require(!discovery.complete, "probes with no answer make the discovery incomplete");
    require(discovery.unanswered == kUnansweredLimit, "the codec is given up after consecutive probes with no answer");
    // Each probe: asked, then three times with the control after each retry that failed.
    require(host.asked.size() == 1 + kUnansweredLimit * (1 + kRetries * 2), "every probe is asked a bounded number of times");
    require(host.pauses == kUnansweredLimit * kRetries, "with a pause before each retry");
    require(h264Of(discovery) == nullptr, "a codec nothing was proved for is not listed");
}

void aBusyHostIsNoAnswerWithoutAControl()
{
    // Nothing ever proves, so there is nothing to tell a refusal from.
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::busy; };
    const auto discovery = runDiscovery(host.hooks());
    require(!discovery.complete && discovery.codecs.empty(), "a host that never answers is not a host that refuses");
    require(host.asked.size() == kUnansweredLimit * (1 + kRetries), "and is asked a bounded number of times");
}

void aRefusalIsAnAnswerWithoutAControl()
{
    // A host that refuses every configuration says so.
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::refused; };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete && discovery.codecs.empty(), "a host that refuses every configuration is complete and lists nothing");
    require(host.pauses == 0, "a refusal is not asked again");
}

// ---- The result of the process.

void incompleteIsNeverStoredAndIsAskedAgain()
{
    Host host;
    bool busy = true;
    host.answer = [&busy](const Probe&, std::size_t) { return busy ? Answer::busy : Answer::healthy; };
    auto service = makeDiscoveryService(host.hooks());
    const auto first = service->query();
    require(first.empty(), "the discovery that found no answer lists nothing");
    require(host.stored.empty(), "and is not stored");
    require(host.listedQueries == 1, "one discovery so far");
    const auto answer = service->current();
    require(!answer->complete, "an incomplete answer is the process's answer for now");

    // Not in the same busy moment.
    busy = false;
    (void)service->query();
    require(host.listedQueries == 1, "a query right after the first does not run discovery again");

    host.clock += DiscoveryService::kRerunIntervalMilliseconds;
    const auto second = service->query();
    require(host.listedQueries == 2, "a later query runs it again");
    require(second.size() == 1 && second[0].codec == VideoCodec::h264, "the complete result is the answer");
    require(host.stored.size() == 1 && host.stored[0].complete, "and the complete result is stored");
    require(service->current()->complete, "it replaced the incomplete one");

    // Settled: no more runs.
    host.clock += 10 * DiscoveryService::kRerunIntervalMilliseconds;
    (void)service->query();
    (void)service->query();
    require(host.listedQueries == 2 && host.stored.size() == 1, "a complete answer is final");
}

void rerunsAreBounded()
{
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::busy; };
    auto service = makeDiscoveryService(host.hooks());
    for (unsigned query = 0; query < 10; ++query) {
        (void)service->query();
        host.clock += DiscoveryService::kRerunIntervalMilliseconds;
    }
    require(host.listedQueries == DiscoveryService::kMaximumRuns, "a process runs discovery a bounded number of times");
    require(host.stored.empty(), "and stores none of the incomplete results");
    require(!service->current()->complete, "the answer stays the incomplete one");
}

void aRefusalIsStored()
{
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return probe.settings.profile == VideoEncodeProfile::h264High422 ? Answer::refused : Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(host.stored.size() == 1, "what the host refuses is an answer, and the complete result is stored");
    require(profileOf(host.stored[0], VideoEncodeProfile::h264High422) == nullptr, "without the refused profile");
    require(profileOf(host.stored[0], VideoEncodeProfile::h264High) != nullptr, "and with the others");
    require(capabilities.size() == 1, "the answer");
    host.clock += 10 * DiscoveryService::kRerunIntervalMilliseconds;
    (void)service->query();
    require(host.listedQueries == 1, "a refusal is not asked again");
}

void aCeilingIsStored()
{
    Host host;
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(host.stored.size() == 1 && host.stored[0].complete, "the ceilings of a host are an answer, and the result is stored");
    require(capabilities.size() == 1 && capabilities[0].maximumWidth == 4096 && capabilities[0].maximumBitrate == 100'000'000ull,
            "with the ceilings in it");
}

void aMixedRunKeepsItsCeilingsAndIsAskedAgain()
{
    // The host is busy for exactly the calls of one probe: BGRA on Main.
    Host host;
    unsigned busyLeft = 0;
    bool triggered = false;
    host.answer = [&](const Probe& probe, std::size_t) {
        if (!triggered && Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra)) {
            triggered = true;
            busyLeft = 1 + kRetries * 2;
        }
        if (busyLeft) {
            --busyLeft;
            return Answer::busy;
        }
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    const auto first = service->query();
    const auto early = service->current();
    require(!early->complete && early->unanswered == 1, "one probe with no answer");
    require(host.stored.empty(), "is enough to keep the result out of the cache");
    requirePackedPair(*early, "the incomplete result keeps the pair together");
    const auto* main = profileOf(*early, VideoEncodeProfile::h264Main);
    require(main && !lists(*main, RawVideoLayout::bgra) && !lists(*main, RawVideoLayout::rgba) && lists(*main, RawVideoLayout::nv12),
            "it lacks the layout of the probe that had no answer, both orders of it");
    require(first.size() == 1 && first[0].maximumWidth == 4096 && first[0].maximumBitrate == 100'000'000ull,
            "while the ceilings, answered in the same run, are in it");

    host.clock += DiscoveryService::kRerunIntervalMilliseconds;
    const auto second = service->query();
    require(host.stored.size() == 1, "the run that was whole is stored");
    const auto* better = profileOf(*service->current(), VideoEncodeProfile::h264Main);
    require(better && lists(*better, RawVideoLayout::bgra) && lists(*better, RawVideoLayout::rgba), "with both orders");
    require(second.size() == 1 && second[0].maximumWidth == 4096, "and the same ceilings");
    // What the first answer promised is still promised: the better answer lists a superset.
    for (const auto& profile : early->find(VideoCodec::h264)->capability.profiles) {
        const auto* later = profileOf(*service->current(), profile.profile);
        require(later != nullptr, "a profile of the incomplete answer is in the complete one");
        for (const auto layout : profile.inputs) require(lists(*later, layout), "and so is each layout");
    }
}

void aCachedResultRunsNothing()
{
    Host host;
    Discovery cached;
    CodecDiscovery entry;
    entry.capability.codec = VideoCodec::h264;
    entry.capability.profiles.push_back({VideoEncodeProfile::h264High, {RawVideoLayout::nv12}, 52, true});
    cached.codecs.push_back(entry);
    host.cache = cached;
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(capabilities.size() == 1 && capabilities[0].profiles.size() == 1, "the cached capabilities");
    require(host.asked.empty() && host.listedQueries == 0 && host.stored.empty(), "a valid cache is neither probed nor rewritten");
}

} // namespace

int main(int argc, char** argv)
{
    struct Case {
        const char* name;
        void (*run)();
    };
    static const Case cases[] = {
        {"healthyHost", healthyHost},
        {"packedPairSurvivesATransientFailure", packedPairSurvivesATransientFailure},
        {"packedPairRefusedTogether", packedPairRefusedTogether},
        {"retriesAreBoundedAndTheDiscoveryIsIncomplete", retriesAreBoundedAndTheDiscoveryIsIncomplete},
        {"aBusyHostIsNoAnswerWithoutAControl", aBusyHostIsNoAnswerWithoutAControl},
        {"aRefusalIsAnAnswerWithoutAControl", aRefusalIsAnAnswerWithoutAControl},
        {"incompleteIsNeverStoredAndIsAskedAgain", incompleteIsNeverStoredAndIsAskedAgain},
        {"rerunsAreBounded", rerunsAreBounded},
        {"aRefusalIsStored", aRefusalIsStored},
        {"aCeilingIsStored", aCeilingIsStored},
        {"aMixedRunKeepsItsCeilingsAndIsAskedAgain", aMixedRunKeepsItsCeilingsAndIsAskedAgain},
        {"aCachedResultRunsNothing", aCachedResultRunsNothing},
    };
    // One case by name, for a run of the test against a changed discovery.
    const char* only = argc == 2 ? argv[1] : nullptr;
    unsigned ran = 0;
    for (const auto& entry : cases) {
        if (only && std::string(entry.name) != only) continue;
        try {
            entry.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", entry.name, error.what());
            return 1;
        }
        ++ran;
    }
    if (!ran) {
        std::fprintf(stderr, "no such case\n");
        return 2;
    }
    std::puts("video encoder discovery ok");
    return 0;
}
