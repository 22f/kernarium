// SPDX-License-Identifier: GPL-2.0-or-later

// A texture the guest hands to another context is exported through a shared
// memory segment, and the renderer copies what GL drew into the segment before
// the importer can look at it: when the importer asks for the segment, and at
// the next fence after a draw. The copy is a read-back, which the host driver
// can refuse when its memory budget is full.
//
// The contract under test: a read-back the host refused is not forgotten. The
// texture stays the newer copy, and the next fence writes it into the segment
// once the memory is back, so the importer never keeps an older picture than
// GL holds for longer than the budget stayed full.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t roomBytes = 96 * mebibyte;
// 16 MiB: a read-back stages the whole texture, which is more than the
// uploader's small buffers and so comes from the budget itself.
constexpr std::uint32_t side = 2048;
constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t exportedId = 20;
constexpr std::uint32_t firstFillerId = 100;

[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

struct Color {
    float red;
    float green;
    float blue;
    std::array<std::uint8_t, 3> bytes;
};

// The segment's first row, where a clear to this colour shows.
constexpr Color first {0.25F, 0.5F, 0.75F, {64, 128, 191}};
constexpr Color second {0.9F, 0.1F, 0.4F, {230, 26, 102}};

[[nodiscard]] bool near(const std::uint8_t* pixel, const Color& color)
{
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int difference = static_cast<int>(pixel[channel]) -
            static_cast<int>(color.bytes[channel]);
        if (difference > 2 || difference < -2)
            return false;
    }
    return true;
}

// Draws the colour over the whole texture without waiting: no fence follows.
void clear(uml::gpu::VirglRenderer& renderer, std::uint32_t surface,
    const Color& color)
{
    Commands commands;
    commands.add(commandCreateObject, objectSurface,
        {surface, exportedId, virglFormatR8g8b8a8Unorm, 0, 0});
    commands.add(commandSetFramebufferState, 0, {1, 0, surface});
    commands.add(commandClear, 0,
        {4U, bits(color.red), bits(color.green), bits(color.blue), bits(1.0F),
            0, 0, 0});
    require(commands.submit(renderer) == 0, "the clear was refused");
}

void fence(uml::gpu::VirglRenderer& renderer, std::uint64_t& counter)
{
    require(renderer.createFenceAndWait(contextId, 0, ++counter, nullptr) == 0,
        "the fence did not complete");
}

// A segment of the exported texture, mapped for reading.
class Segment final {
public:
    explicit Segment(int descriptor)
    {
        struct stat info {};
        require(::fstat(descriptor, &info) == 0, "the segment has no size");
        size_ = static_cast<std::size_t>(info.st_size);
        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, descriptor, 0);
        require(mapping_ != MAP_FAILED, "the segment could not be mapped");
        ::close(descriptor);
    }

    ~Segment() { ::munmap(mapping_, size_); }
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    [[nodiscard]] const std::uint8_t* pixel(std::size_t x, std::size_t y) const
    {
        // The rows are the texture's width, which is a whole number of 16 byte
        // groups already, and four bytes a pixel.
        return static_cast<const std::uint8_t*>(mapping_) +
            (y * side + x) * 4U;
    }

private:
    void* mapping_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace

int main()
{
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(used() + roomBytes);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(contextId, virgl2CapsetId, "shm export") ==
            0,
        "the guest context could not be created");

    require(renderer->createResource3d(
                uml::gpu::VirglRenderer::Resource3d {
                    .resourceId = exportedId,
                    .target = pipeTexture2d,
                    .format = virglFormatR8g8b8a8Unorm,
                    .bind = virglBindRenderTarget | virglBindSamplerView,
                    .width = side,
                    .height = side,
                    .depth = 1,
                    .arraySize = 1,
                }) == 0,
        "the texture could not be created");
    require(renderer->attachResource(contextId, exportedId) == 0,
        "the texture could not be attached");

    std::uint32_t type = 0;
    int descriptor = -1;
    if (renderer->exportResourceBlob(exportedId, type, descriptor) != 0 ||
        descriptor < 0)
        return uml::gpu::testing::skip(
            "this host does not export a texture through a segment");
    Segment segment(descriptor);

    std::uint64_t counter = 1000;
    std::uint32_t surface = firstTransientHandle;

    // The machinery: a clear followed by a fence reaches the segment.
    clear(*renderer, surface++, first);
    fence(*renderer, counter);
    require(near(segment.pixel(7, 9), first),
        "a clear and a fence did not reach the segment");

    // GL draws a new colour; the segment still has the old one.
    clear(*renderer, surface++, second);

    // Fill the budget with textures until the host refuses one, in steps down
    // to one that leaves less free than a read-back's staging buffer.
    std::vector<std::uint32_t> fillers;
    std::uint32_t fillerId = firstFillerId;
    for (const std::uint32_t fillerSide : {2048U, 1024U, 512U, 256U}) {
        for (int attempt = 0; attempt < 128; ++attempt) {
            const int result = renderer->createResource3d(
                uml::gpu::VirglRenderer::Resource3d {
                    .resourceId = fillerId,
                    .target = pipeTexture2d,
                    .format = virglFormatR8g8b8a8Unorm,
                    .bind = virglBindRenderTarget | virglBindSamplerView,
                    .width = fillerSide,
                    .height = fillerSide,
                    .depth = 1,
                    .arraySize = 1,
                });
            if (result != 0)
                break;
            fillers.push_back(fillerId++);
        }
    }
    std::fprintf(stderr, "filled the budget with %zu textures, %.1f MiB used\n",
        fillers.size(), static_cast<double>(used()) / mebibyte);

    // The importer asks for the segment again: the renderer hands out what GL
    // drew, which needs the read-back the full budget refuses.
    int again = -1;
    std::uint32_t againType = 0;
    const int exported = renderer->exportResourceBlob(exportedId, againType, again);
    if (again >= 0)
        ::close(again);
    require(exported == 0, "the second export failed");
    require(near(segment.pixel(7, 9), first),
        "the read-back was not refused, so nothing was proven");

    for (const std::uint32_t id : fillers)
        renderer->unrefResource(id);
    fillers.clear();
    std::this_thread::sleep_for(200ms);

    // With the memory back, the next fence writes the texture into the segment.
    fence(*renderer, counter);
    const bool written = near(segment.pixel(7, 9), second) &&
        near(segment.pixel(side - 1, side - 1), second);
    std::fprintf(stderr, "segment after the refusal and a fence: %s\n",
        written ? "the newer picture" : "the older picture");
    if (!written) {
        std::fputs("Shm export refusal: a refused read-back was dropped\n",
            stderr);
        return 1;
    }
    std::fputs("Shm export refusal: ok\n", stderr);
    return 0;
}
