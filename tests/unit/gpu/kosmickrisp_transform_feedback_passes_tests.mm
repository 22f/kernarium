// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "require.hpp"
#include "shaders/transform_feedback_passes_spirv.hpp"

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/* A GPU particle simulation, as a GL application on a virtualized stack
 * submits it, steps its particles by transform feedback with the rasterizer
 * discarded: every step is a render pass over the frame's attachment (loaded
 * and stored, never drawn into), one discard draw that captures the next
 * state, and a barrier. A frame can hold hundreds of them, and the last pass
 * draws the particles.
 *
 * On a Metal backend the draw that captures runs compute (the vertex stage)
 * before its render draw (the capture's stores), and a compute encoder that
 * follows a render encoder's start cannot share its command buffer's place: it
 * gets a command buffer of its own, and the render pass is split to put the
 * compute before it. A command buffer that starts with a render encoder
 * allocates 2.5 MiB of command storage, so hundreds of passes recorded that
 * way are over a gigabyte in one recording, held until the GPU finishes it.
 * The driver instead starts the encoder of a pass whose attachments are all
 * loaded at its first draw, after the compute that draw launches, and stops it
 * for the counter copies around a capture. A recording's passes then share one
 * main command buffer, in which each further encoder costs a few hundred KiB.
 *
 * The guard counts what that costs instead of timing it. It replaces three
 * selectors of the Metal 4 command buffer class (beginning a command buffer,
 * and making a render or a compute encoder) and the allocator's reset, which
 * the driver calls once the GPU has finished with an allocator's storage; the
 * allocator's size at that reset is the storage the recording held. The counts
 * depend on what the driver records, not on how busy the host is. */

namespace {

using uml::gpu::test::DrawContext;
using uml::gpu::test::FeedbackChainConfig;
using uml::gpu::test::FeedbackChainResult;
using uml::testing::require;

constexpr uint32_t kExtent = 16;
constexpr auto kCompletionDeadline = std::chrono::seconds(30);
/* Metal command buffers in a recording: a few, however many passes it has.
 * The variants record in 1, the long chain in 3 (its statistics query adds two
 * of its own); a driver that opens one per pass records hundreds. */
constexpr uint32_t kMaxCommandBuffers = 4;
/* The largest allocator when the GPU is done with the recording. The 129-pass
 * chain holds 86 MiB (about 0.6 MiB a pass: a render and a compute encoder
 * that share a command buffer); the bound leaves half again for the Metal
 * versions and devices that size encoders differently. The driver that opened
 * a command buffer per pass held 656 MiB for the same chain. The 25-pass
 * variants hold 18 to 20 MiB against 130 MiB. */
constexpr uint64_t kMaxChainAllocatorBytes = 128ull << 20;
constexpr uint64_t kMaxVariantAllocatorBytes = 32ull << 20;

struct Counters {
    std::atomic<uint32_t> commandBuffers{0};
    std::atomic<uint32_t> renderEncoders{0};
    std::atomic<uint32_t> computeEncoders{0};
};
Counters counters;

/* The allocators a recording began command buffers on, and for each when it
 * was last begun on, last reset and how large it was at that reset. */
struct AllocatorInfo {
    uint64_t beganAt = 0;
    uint64_t resetAt = 0;
    uint64_t resetSize = 0;
};
std::mutex mutex;
std::map<const void *, AllocatorInfo> allocators;
std::set<const void *> inRecording;
uint64_t sequence = 0;
bool armed = false;

IMP beginImplementation = nullptr;
IMP renderImplementation = nullptr;
IMP computeImplementation = nullptr;
IMP resetImplementation = nullptr;

/* The replacements forward to the originals through untyped function
 * pointers; ARC adds no retain or release around objects it only passes
 * through. */
void countedBegin(id receiver, SEL command, id allocator)
{
    counters.commandBuffers.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex);
        const void *key = (__bridge const void *)allocator;
        allocators[key].beganAt = ++sequence;
        if (armed)
            inRecording.insert(key);
    }
    reinterpret_cast<void (*)(id, SEL, id)>(beginImplementation)(receiver, command, allocator);
}

id countedRender(id receiver, SEL command, id descriptor)
{
    counters.renderEncoders.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<id (*)(id, SEL, id)>(renderImplementation)(receiver, command,
                                                                       descriptor);
}

id countedCompute(id receiver, SEL command)
{
    counters.computeEncoders.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<id (*)(id, SEL)>(computeImplementation)(receiver, command);
}

void countedReset(id receiver, SEL command)
{
    id<MTL4CommandAllocator> allocator = (id<MTL4CommandAllocator>)receiver;
    const uint64_t before = [allocator allocatedSize];
    reinterpret_cast<void (*)(id, SEL)>(resetImplementation)(receiver, command);
    const uint64_t size = std::max<uint64_t>(before, [allocator allocatedSize]);
    std::lock_guard lock(mutex);
    AllocatorInfo &info = allocators[(__bridge const void *)receiver];
    info.resetAt = ++sequence;
    info.resetSize = size;
}

Method method_of(id object, SEL selector, const char *what)
{
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    require(method != nullptr, std::string("the Metal object lacks ") + what);
    return method;
}

/* The classes the default device's command buffers and allocators belong to
 * are learned from samples made before the replacement. */
void count_metal()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    require(device != nil, "no Metal device");
    id<MTL4CommandBuffer> buffer = [device newCommandBuffer];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
    require(buffer != nil && allocator != nil, "the device made no Metal 4 command buffer");
    beginImplementation = method_setImplementation(
        method_of(buffer, @selector(beginCommandBufferWithAllocator:), "begin"),
        reinterpret_cast<IMP>(countedBegin));
    renderImplementation = method_setImplementation(
        method_of(buffer, @selector(renderCommandEncoderWithDescriptor:), "render encoders"),
        reinterpret_cast<IMP>(countedRender));
    computeImplementation = method_setImplementation(
        method_of(buffer, @selector(computeCommandEncoder), "compute encoders"),
        reinterpret_cast<IMP>(countedCompute));
    resetImplementation = method_setImplementation(
        method_of(allocator, @selector(reset), "reset"), reinterpret_cast<IMP>(countedReset));
}

struct Spent {
    uint32_t commandBuffers = 0;
    uint32_t renderEncoders = 0;
    uint32_t computeEncoders = 0;
    uint32_t allocators = 0;
    uint64_t largestAllocator = 0;
};

/* Records and runs the chain, then waits for the driver to hand back every
 * allocator the recording used: the point at which its storage is known. */
Spent run(DrawContext &context, FeedbackChainConfig config, FeedbackChainResult &result)
{
    config.step_vertex_shader = feedback_step_vertex;
    config.raster_vertex_shader = feedback_raster_vertex;
    config.raster_fragment_shader = feedback_raster_fragment;
    Spent spent;
    uint32_t commandBuffers = 0, renderEncoders = 0, computeEncoders = 0;
    config.recording_begins = [&] {
        {
            std::lock_guard lock(mutex);
            inRecording.clear();
            armed = true;
        }
        commandBuffers = counters.commandBuffers.load();
        renderEncoders = counters.renderEncoders.load();
        computeEncoders = counters.computeEncoders.load();
    };
    config.recording_complete = [&] {
        {
            std::lock_guard lock(mutex);
            armed = false;
        }
        spent.commandBuffers = counters.commandBuffers.load() - commandBuffers;
        spent.renderEncoders = counters.renderEncoders.load() - renderEncoders;
        spent.computeEncoders = counters.computeEncoders.load() - computeEncoders;
    };
    result = context.feedback_chain(config);

    const auto deadline = std::chrono::steady_clock::now() + kCompletionDeadline;
    for (;;) {
        bool done = true;
        {
            std::lock_guard lock(mutex);
            spent.allocators = uint32_t(inRecording.size());
            spent.largestAllocator = 0;
            for (const void *key : inRecording) {
                const AllocatorInfo &info = allocators[key];
                if (info.resetAt < info.beganAt) {
                    done = false;
                    continue;
                }
                spent.largestAllocator = std::max(spent.largestAllocator, info.resetSize);
            }
        }
        if (done)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("the recording's command storage was not handed back");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return spent;
}

using Pixel = std::array<uint8_t, 4>;

/* What the chain must have produced. Every step maps a word x to 3x + c with
 * c = 1, 2, 3, 4 for the four words of a vertex, and the seed is 1, 2, 3, ...;
 * there are steps + 1 discard draws, the last of them in the pass that
 * rasterizes. */
void check_results(const FeedbackChainConfig &config, const FeedbackChainResult &result,
                   bool blueBackground, const std::string &what)
{
    require(result.captured.size() == size_t(config.points) * 4,
            what + ": the capture has the wrong size");
    for (size_t word = 0; word < result.captured.size(); ++word) {
        uint32_t value = uint32_t(word) + 1u;
        for (uint32_t step = 0; step <= config.steps; ++step)
            value = value * 3u + uint32_t(word % 4) + 1u;
        require(result.captured[word] == value,
                what + ": word " + std::to_string(word) + " captured " +
                    std::to_string(result.captured[word]) + ", expected " + std::to_string(value));
    }

    /* The discard passes leave the attachment as it was, whatever painted it
     * last; the rasterizing draw adds one green point per vertex on row 8. */
    const Pixel background = blueBackground ? Pixel{0, 0, 255, 255} : Pixel{0, 0, 0, 255};
    const Pixel point{0, 255, 0, 255};
    require(result.pixels.size() == size_t(kExtent) * kExtent, what + ": wrong pixel count");
    for (uint32_t y = 0; y < kExtent; ++y) {
        for (uint32_t x = 0; x < kExtent; ++x) {
            const Pixel &got = result.pixels[size_t(y) * kExtent + x];
            const Pixel &want = (y == 8 && x < config.points) ? point : background;
            require(got == want,
                    what + ": pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " +
                        std::to_string(got[0]) + "," + std::to_string(got[1]) + "," +
                        std::to_string(got[2]) + "," + std::to_string(got[3]));
        }
    }

    /* The query spans the passes and counts the samples of the one draw that
     * rasterizes. */
    const uint64_t samples = uint64_t(config.points) * uint32_t(config.samples);
    require(result.occlusion_samples == samples,
            what + ": the occlusion query counted " + std::to_string(result.occlusion_samples) +
                " samples, expected " + std::to_string(samples));
    if (!result.statistics.empty()) {
        /* Input assembly vertices, vertex shader invocations: every draw
         * assembles and shades its vertices, discarded or not. */
        const uint64_t vertices = uint64_t(config.points) * (config.steps + 2);
        require(result.statistics.size() == 2 && result.statistics[0] == vertices &&
                    result.statistics[1] == vertices,
                what + ": the statistics query counted " + std::to_string(result.statistics[0]) +
                    " and " + std::to_string(result.statistics[1]) + ", expected " +
                    std::to_string(vertices));
    }
    if (!result.timestamps.empty()) {
        require(result.timestamps.size() == 2 && result.timestamps[0] != 0 &&
                    result.timestamps[1] >= result.timestamps[0],
                what + ": the timestamps are not set in order");
    }
}

void report(const char *what, const FeedbackChainConfig &config, const Spent &spent)
{
    std::printf("transform feedback passes: %s, %u passes: %u command buffers, %u render and %u "
                "compute encoders, %u allocators, largest %.2f MiB\n",
                what, config.steps + 1, spent.commandBuffers, spent.renderEncoders,
                spent.computeEncoders, spent.allocators,
                double(spent.largestAllocator) / double(1 << 20));
    std::fflush(stdout);
}

void check_spent(const char *what, const FeedbackChainConfig &config, const Spent &spent,
                 uint64_t maxAllocatorBytes)
{
    report(what, config, spent);
    require(spent.commandBuffers <= kMaxCommandBuffers,
            std::string(what) + ": " + std::to_string(spent.commandBuffers) +
                " Metal command buffers, over the bound of " +
                std::to_string(kMaxCommandBuffers));
    require(spent.largestAllocator <= maxAllocatorBytes,
            std::string(what) + ": an allocator held " +
                std::to_string(spent.largestAllocator >> 20) + " MiB when the GPU finished, over " +
                std::to_string(maxAllocatorBytes >> 20) + " MiB");
}

} // namespace

int main()
{
    try {
        count_metal();
        DrawContext context;
        require(context.capabilities().driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP,
                "the guard counts the KosmicKrisp driver's Metal calls");
        require(context.capabilities().transform_feedback, "transform feedback unavailable");
        const bool statistics = context.capabilities().pipeline_statistics;

        /* The burst: a long chain with every query live over it. */
        {
            FeedbackChainConfig config;
            config.steps = 128;
            config.statistics_query = statistics;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            check_results(config, result, false, "chain");
            check_spent("chain", config, spent, kMaxChainAllocatorBytes);
        }
        std::puts("PASS transform feedback passes: a long chain records in a few encoders");

        /* An all-LOAD pass over a multisampled attachment: the encoder that
         * starts at the rasterizing draw must be the attachment's sample count,
         * not a default. */
        {
            FeedbackChainConfig config;
            config.steps = 24;
            config.samples = VK_SAMPLE_COUNT_4_BIT;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            check_results(config, result, false, "multisampled");
            check_spent("multisampled", config, spent, kMaxVariantAllocatorBytes);
        }
        std::puts("PASS transform feedback passes: a multisampled pass starts at its sample count");

        /* A pass with no draw at all starts no encoder and leaves the
         * attachment alone. */
        {
            FeedbackChainConfig config;
            config.steps = 24;
            config.empty_pass = true;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            check_results(config, result, false, "empty pass");
            check_spent("empty pass", config, spent, kMaxVariantAllocatorBytes);
        }
        std::puts("PASS transform feedback passes: a pass without a draw records nothing");

        /* A clear in the middle pass touches the attachment, so that pass
         * starts its encoder: the cost is that one pass, not the chain. */
        {
            FeedbackChainConfig config;
            config.steps = 24;
            config.clear_in_pass = true;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            check_results(config, result, true, "clear in pass");
            check_spent("clear in pass", config, spent, kMaxVariantAllocatorBytes);
        }
        std::puts("PASS transform feedback passes: a clear starts the pass's encoder");

        /* A timestamp of a render stage belongs to the pass's render encoder,
         * which it starts. */
        {
            FeedbackChainConfig config;
            config.steps = 24;
            config.timestamp_in_pass = true;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            check_results(config, result, false, "timestamp in pass");
            check_spent("timestamp in pass", config, spent, kMaxVariantAllocatorBytes);
        }
        std::puts("PASS transform feedback passes: a render-stage timestamp starts the pass's encoder");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL transform feedback passes: %s\n", error.what());
        return 1;
    }
}
