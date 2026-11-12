// SPDX-License-Identifier: GPL-2.0-or-later

// A gralloc BLOB buffer, such as the camera framework's JPEG output: a classic
// 2D resource of format R8, one row as wide as the buffer is long, whose bind
// flags name no GPU unit (shared, linear and the allocator's software, camera
// and codec flags). The guest writes it through its own mapping of the backing
// pages and brackets the access with transfers; the host never reads it, so
// the renderer keeps no storage for it and a texture limit of the GL driver
// does not apply.
//
// The contract under test:
//  - such a buffer is created at any size up to the renderer's bound, also
//    one a GL texture could not be (18,354,160 bytes is the largest JPEG of
//    a 12 megapixel camera) and one it could (100 and 16,385 bytes);
//  - the renderer holds no storage for it, and the bytes the guest wrote into
//    the backing are the bytes it reads back after a transfer in either
//    direction;
//  - a transfer that names bytes outside the buffer is refused;
//  - no command can name it: a view, a surface, a vertex or index buffer, a
//    copy and an inline write fail the command cleanly, and the presenter
//    gets no texture;
//  - a size the renderer does not accept is refused at creation, and every
//    other resource keeps its checks: the same size with a bind flag a GPU
//    unit uses is refused, and so is a buffer taller than one row.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;

constexpr std::uint32_t pipeTexture2dTarget = 2;
constexpr std::uint32_t virglBindShared = 1U << 20U;
constexpr std::uint32_t virglBindLinear = 1U << 22U;
constexpr std::uint32_t virglBindCameraWrite = 1U << 24U;
constexpr std::uint32_t virglBindSwReadOften = 1U << 28U;
constexpr std::uint32_t virglBindSwWriteOften = 1U << 30U;
// What minigbm names for a camera's JPEG buffer that software reads.
constexpr std::uint32_t blobBind = virglBindShared | virglBindLinear |
    virglBindCameraWrite | virglBindSwReadOften | virglBindSwWriteOften;
constexpr std::uint32_t commandInlineWrite = 9;
constexpr std::uint32_t commandSetIndexBuffer = 11;
constexpr std::uint32_t commandResourceCopyRegion = 17;

unsigned failures = 0;

void check(bool ok, const char* what, const char* message)
{
    if (ok)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", what, message);
}

[[nodiscard]] int create(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t bind, std::uint32_t width, std::uint32_t height)
{
    return renderer.createResource3d(VirglRenderer::Resource3d {
        .resourceId = id,
        .target = pipeTexture2dTarget,
        .format = virglFormatR8Unorm,
        .bind = bind,
        .width = width,
        .height = height,
        .depth = 1,
        .arraySize = 1,
    });
}

[[nodiscard]] int transfer(VirglRenderer& renderer, bool toHost,
    std::uint32_t context, std::uint32_t id, std::uint32_t x,
    std::uint32_t width)
{
    const VirglRenderer::Box box {
        .x = x, .y = 0, .z = 0, .width = width, .height = 1, .depth = 1};
    return renderer.transferResource(toHost, id, context, 0, 0, 0, box, x);
}

// What the renderer logs to stderr while `action` runs. A command that names a
// resource it cannot use reports a context error and, for some commands, still
// returns success to its caller; the log line names the resource.
std::string logOf(const std::function<void()>& action)
{
    std::fflush(stderr);
    std::FILE* file = std::tmpfile();
    const int saved = ::dup(STDERR_FILENO);
    require(file != nullptr && saved >= 0, "the log could not be captured");
    ::dup2(::fileno(file), STDERR_FILENO);
    action();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::string text;
    std::rewind(file);
    std::array<char, 512> chunk {};
    while (const std::size_t count =
               std::fread(chunk.data(), 1, chunk.size(), file))
        text.append(chunk.data(), count);
    std::fclose(file);
    std::fputs(text.c_str(), stderr);
    return text;
}

void pattern(std::vector<std::uint8_t>& bytes)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(index * 131U + 7U);
}

// A buffer of `size` bytes, written and read back through the transfers.
void roundTrip(VirglRenderer& renderer, std::uint32_t id, std::uint32_t size)
{
    char name[64];
    std::snprintf(name, sizeof(name), "buffer of %u bytes", size);

    check(create(renderer, id, blobBind, size, 1) == 0, name,
        "the buffer was not created");
    check(renderer.resourceStorageBytes(id) == 0, name,
        "the renderer holds storage for it");

    std::vector<std::uint8_t> backing(size);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, name,
        "its backing was not attached");
    check(renderer.attachResource(contextId, id) == 0, name,
        "it was not attached to the context");

    check(transfer(renderer, true, contextId, id, 0, size) == 0, name,
        "the transfer to the host failed");
    check(backing == written, name, "the transfer to the host changed it");
    check(transfer(renderer, false, contextId, id, 0, size) == 0, name,
        "the transfer from the host failed");
    check(backing == written, name,
        "the transfer from the host did not leave the bytes written");

    // The guest rewrites a part of it, as the camera does while it fills the
    // buffer, and the transfers neither undo nor repeat the write.
    const std::uint32_t part = std::min<std::uint32_t>(size / 2U, 4096U);
    std::fill_n(backing.begin() + size / 4U, part, std::uint8_t {0xa5});
    const std::vector<std::uint8_t> rewritten = backing;
    check(transfer(renderer, true, contextId, id, size / 4U, part) == 0, name,
        "the transfer of a part to the host failed");
    check(transfer(renderer, false, contextId, id, 0, size) == 0, name,
        "the transfer of the whole from the host failed");
    check(backing == rewritten, name,
        "the transfers did not leave the rewritten bytes");

    VirglRenderer::NativeTexture texture;
    check(renderer.resourceMetalTexture(id, texture) != 0, name,
        "a presenter was given a texture for it");

    (void)renderer.detachResource(contextId, id);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// Names the buffer in commands that need a texture or a host buffer. Each one
// ends the context it is sent to, so each goes to a context of its own.
void commandsRefused(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t firstContext)
{
    constexpr std::uint32_t size = 100000;
    check(create(renderer, id, blobBind, size, 1) == 0, "commands",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(size);
    pattern(backing);
    const std::vector<std::uint8_t> written = backing;
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "commands",
        "its backing was not attached");

    // An ordinary texture to copy to and from.
    const std::uint32_t other = id + 1000;
    check(create(renderer, other, virglBindSamplerView | virglBindRenderTarget,
              16, 16) == 0,
        "commands", "the texture to copy was not created");

    struct Attempt {
        const char* name;
        std::uint32_t command;
        std::uint32_t object;
        std::vector<std::uint32_t> payload;
    };
    const std::vector<Attempt> attempts {
        {"sampler view", commandCreateObject, objectSamplerView,
            {firstTransientHandle, id, virglFormatR8Unorm | (2U << 24U), 0, 0,
                0U | (1U << 3U) | (2U << 6U) | (3U << 9U)}},
        {"surface", commandCreateObject, objectSurface,
            {firstTransientHandle, id, virglFormatR8Unorm, 0, 0}},
        {"vertex buffer", commandSetVertexBuffers, 0, {4, 0, id}},
        {"index buffer", commandSetIndexBuffer, 0, {id, 1, 0}},
        {"copy from it", commandResourceCopyRegion, 0,
            {other, 0, 0, 0, 0, id, 0, 0, 0, 0, 1, 1, 1}},
        {"copy into it", commandResourceCopyRegion, 0,
            {id, 0, 0, 0, 0, other, 0, 0, 0, 0, 1, 1, 1}},
        {"inline write", commandInlineWrite, 0,
            {id, 0, 0, 0, 0, 0, 0, 0, 4, 1, 1, 0x01020304U}},
    };

    std::uint32_t context = firstContext;
    for (const Attempt& attempt : attempts) {
        check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
            attempt.name, "the context was not created");
        check(renderer.attachResource(context, id) == 0, attempt.name,
            "the buffer was not attached to the context");
        check(renderer.attachResource(context, other) == 0, attempt.name,
            "the texture was not attached to the context");
        std::vector<std::uint32_t> words;
        words.push_back(
            (static_cast<std::uint32_t>(attempt.payload.size()) << 16U) |
            (attempt.object << 8U) | attempt.command);
        words.insert(words.end(), attempt.payload.begin(),
            attempt.payload.end());
        const std::string log = logOf([&] {
            (void)renderer.submit(
                context,
                {reinterpret_cast<const std::uint8_t*>(words.data()),
                    words.size() * sizeof(std::uint32_t)});
        });
        check(log.find("context error reported " + std::to_string(context)) !=
                std::string::npos,
            attempt.name, "a command naming the buffer was accepted");
        check(log.find("Illegal resource " + std::to_string(id)) !=
                std::string::npos,
            attempt.name, "the error did not name the buffer");
        renderer.destroyContext(context);
        ++context;
    }
    check(backing == written, "commands", "a refused command changed it");

    renderer.detachBacking(id);
    renderer.unrefResource(id);
    renderer.unrefResource(other);
}

// A transfer outside the buffer is refused, and ends the context it is sent to.
void transferRefused(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 4096;
    check(create(renderer, id, blobBind, size, 1) == 0, "transfer",
        "the buffer was not created");
    std::vector<std::uint8_t> backing(size);
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()}};
    check(renderer.attachBacking(id, vectors) == 0, "transfer",
        "its backing was not attached");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "transfer", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "transfer",
        "the buffer was not attached to the context");
    check(transfer(renderer, true, context, id, size - 1, 2) != 0, "transfer",
        "a box past the end of the buffer was accepted");
    renderer.destroyContext(context);
    renderer.detachBacking(id);
    renderer.unrefResource(id);
}

// A buffer without backing: nothing was written, and a transfer is refused as
// for any resource without backing.
void transferWithoutBacking(VirglRenderer& renderer, std::uint32_t id,
    std::uint32_t context)
{
    constexpr std::uint32_t size = 4096;
    check(create(renderer, id, blobBind, size, 1) == 0, "no backing",
        "the buffer was not created");
    check(renderer.createContext(context, virgl2CapsetId, "hostless") == 0,
        "no backing", "the context was not created");
    check(renderer.attachResource(context, id) == 0, "no backing",
        "the buffer was not attached to the context");
    check(transfer(renderer, true, context, id, 0, size) != 0, "no backing",
        "a transfer without backing was accepted");
    renderer.destroyContext(context);
    renderer.unrefResource(id);
}

void sizesRefused(VirglRenderer& renderer, std::uint32_t id)
{
    for (const std::uint32_t width :
        {0U, (1U << 30U) + 1U, 0x7fffffffU, 0xffffffffU}) {
        char name[64];
        std::snprintf(name, sizeof(name), "width %u", width);
        const int result = create(renderer, id, blobBind, width, 1);
        check(result != 0, name, "the buffer was created");
        if (result == 0)
            renderer.unrefResource(id);
    }

    // The checks of every other resource stay: the same width with a bind flag
    // a GPU unit uses is a texture the host cannot make, and a buffer of more
    // than one row is a texture too.
    for (const std::uint32_t bind : {blobBind | virglBindSamplerView,
             blobBind | virglBindRenderTarget, virglBindSamplerView}) {
        char name[64];
        std::snprintf(name, sizeof(name), "bind 0x%x", bind);
        const int result = create(renderer, id, bind, 18354160, 1);
        check(result != 0, name, "a texture past the limit was created");
        if (result == 0)
            renderer.unrefResource(id);
    }
    const int tall = create(renderer, id, blobBind, 18354160, 2);
    check(tall != 0, "two rows", "a texture past the limit was created");
    if (tall == 0)
        renderer.unrefResource(id);

    // A texture the GPU samples keeps its storage.
    check(create(renderer, id, virglBindSamplerView | virglBindShared, 64,
              64) == 0,
        "sampled texture", "it was not created");
    check(renderer.resourceStorageBytes(id) != 0, "sampled texture",
        "it has no storage");
    renderer.unrefResource(id);
}

} // namespace

int main()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(
                contextId, virgl2CapsetId, "hostless buffer") == 0,
        "the guest context could not be created");

    std::uint32_t id = 10;
    roundTrip(*renderer, id++, 18354160);
    roundTrip(*renderer, id++, 16385);
    roundTrip(*renderer, id++, 100);
    roundTrip(*renderer, id++, 1U << 20U);

    commandsRefused(*renderer, id++, 20);
    transferRefused(*renderer, id++, 40);
    transferWithoutBacking(*renderer, id++, 41);
    sizesRefused(*renderer, id++);

    if (failures != 0)
        return 1;
    std::fputs("Host-less buffer: ok\n", stderr);
    return 0;
}
