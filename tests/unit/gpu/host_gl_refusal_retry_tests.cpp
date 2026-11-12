// SPDX-License-Identifier: GPL-2.0-or-later

// With a bounded device memory budget the host driver refuses an allocation
// the budget cannot hold, and a budget that is full is often one that still
// holds memory for work that has finished: the buffers of draws the GPU has
// run, the storage of a texture the application deleted. The host used to
// drop what the refused allocation was for without a word, so the
// application lost a draw, or kept a texture with its older contents. The
// contract under test is that it gives back what it can and tries once more
// before it drops anything, and that it loses nothing more when that is not
// enough.
//
// The budget is a pool the test states for the driver (host_gl_injected_pool
// .hpp), which refuses a request of a mebibyte or more that would take the
// live device memory past the limit the test sets: no number of the host's
// enters the test.
//
//  - draws: the constants of 32 KiB a draw uploads fill, within a few
//    hundred draws, the 2 MiB uploader buffers that the draws in flight
//    still hold; no draw is refused over a thousand of them under a limit of
//    eight buffers, for the work that has finished gives its buffers back;
//  - uploads: a texture the application deleted after the GPU used it still
//    holds its memory, and an upload to a texture the GPU still uses is
//    staged through a buffer the limit has no room for; the upload lands,
//    because the memory of the deleted texture is given back first; and
//  - uploads that cannot fit however much is given back are dropped, the host
//    and the context survive them, and the same upload lands once the limit
//    is lifted.
//
// One process makes one of the three shapes, "draw", "upload" or "persist":
// what an earlier shape left in the driver's caches would give the last one
// the room it must not have.

#include "host_gl_injected_pool.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/gl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

namespace {

using uml::gpu::testing::InjectedPool;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

#if defined(UML_REFUSAL_RETRY_UPLOAD)
constexpr const char* shapeName = "upload";
#elif defined(UML_REFUSAL_RETRY_PERSIST)
constexpr const char* shapeName = "persist";
#else
constexpr const char* shapeName = "draw";
#endif

#if !defined(UML_REFUSAL_RETRY_UPLOAD) && !defined(UML_REFUSAL_RETRY_PERSIST)
#define UML_REFUSAL_RETRY_DRAW 1
#endif

#if defined(UML_REFUSAL_RETRY_DRAW)
constexpr const char* vertexShader = R"(#version 450 core
uniform vec4 values[2048];
uniform int index;
out vec4 tint;
void main()
{
    tint = values[index & 2047];
    gl_Position = vec4(-0.75, -0.75, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";

constexpr const char* fragmentShader = R"(#version 450 core
in vec4 tint;
out vec4 color;
void main()
{
    color = tint;
}
)";
#endif

using Pixel = std::array<std::uint8_t, 4>;

[[nodiscard]] Pixel pixelAt(GLint x, GLint y)
{
    Pixel pixel {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
}

#if !defined(UML_REFUSAL_RETRY_DRAW)

constexpr GLsizei side = 2048;

// The picture an upload writes: every pixel is told apart by its place.
[[nodiscard]] std::vector<std::uint8_t> picture(GLsizei width, unsigned seed)
{
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(width) * 4);
    for (GLsizei y = 0; y < width; ++y) {
        for (GLsizei x = 0; x < width; ++x) {
            std::uint8_t* const pixel = &bytes[(static_cast<std::size_t>(y)
                                                       * static_cast<std::size_t>(width)
                                                   + static_cast<std::size_t>(x))
                * 4];
            pixel[0] = static_cast<std::uint8_t>(x + seed);
            pixel[1] = static_cast<std::uint8_t>(y * 3 + seed);
            pixel[2] = static_cast<std::uint8_t>(
                (x >> 8) | ((y >> 8) << 2) | (seed << 4));
            pixel[3] = 255;
        }
    }
    return bytes;
}

[[nodiscard]] Pixel expected(
    const std::vector<std::uint8_t>& bytes, GLsizei width, GLsizei x, GLsizei y)
{
    const std::size_t at = (static_cast<std::size_t>(y)
                                   * static_cast<std::size_t>(width)
                               + static_cast<std::size_t>(x))
        * 4;
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}

struct Target {
    GLuint texture {};
    GLuint framebuffer {};
};

// A texture with storage for a whole picture, cleared to red and idle, and a
// framebuffer that renders to it. Nothing is uploaded to it, so that no
// staging buffer of its size is left in the driver's caches.
[[nodiscard]] Target makeTarget(GLsizei width)
{
    Target target;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, width);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        target.texture, 0);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the target was not made");
    return target;
}

// Writes the upper half of the picture over the texture while the GPU still
// uses it: the clear that comes first is work the upload has to wait for,
// and the upload does not cover all of the texture, so the clear is not
// dropped; the driver stages the upload instead of copying it in directly.
// It needs 8 MiB of staging.
void uploadWhileBusy(
    const Target& target, GLsizei width, const std::vector<std::uint8_t>& bytes)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, width / 2, GL_RGBA,
        GL_UNSIGNED_BYTE, bytes.data());
}

constexpr std::array<std::array<GLsizei, 2>, 4> samples {
    {{0, 0}, {701, 305}, {2047, 1023}, {5, 900}}};

// Whether the texture shows the picture at the places the samples name, all
// of them in the half the upload writes.
[[nodiscard]] bool shows(const Target& target, GLsizei width,
    const std::vector<std::uint8_t>& bytes)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    for (const auto& at : samples) {
        if (pixelAt(at[0], at[1]) != expected(bytes, width, at[0], at[1]))
            return false;
    }
    return true;
}

#endif

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    // The renderer states its own pool when it starts, which this replaces:
    // the driver charges its allocations to this one from here on.
    InjectedPool pool;

    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

#if defined(UML_REFUSAL_RETRY_DRAW)
    constexpr int drawsPerRound = 256;
    constexpr int totalDraws = 1536;

    const GLuint program = uml::gpu::testing::linkProgram(
        {uml::gpu::testing::compileShader(GL_VERTEX_SHADER, vertexShader),
            uml::gpu::testing::compileShader(
                GL_FRAGMENT_SHADER, fragmentShader)});
    glUseProgram(program);
    const GLint indexLocation = glGetUniformLocation(program, "index");
    const GLint valuesLocation = glGetUniformLocation(program, "values");
    require(indexLocation >= 0 && valuesLocation >= 0,
        "the program's uniforms were not found");

    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    const auto target = uml::gpu::testing::colourTarget(
        4, 4, uml::gpu::testing::TextureStorage::immutable);
    require(target.complete, "the target is not complete");
    glViewport(0, 0, 4, 4);
    glEnable(GL_PROGRAM_POINT_SIZE);

    // values[0] is green; the draws below read it. One draw first, so that
    // what the program needs is made before the limit is set.
    const std::array<GLfloat, 4> green {0.0F, 1.0F, 0.0F, 1.0F};
    glUniform4fv(valuesLocation, 1, green.data());
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1i(indexLocation, 0);
    glDrawArrays(GL_POINTS, 0, 1);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the first draw raised an error");

    // Each draw uploads its constants again, 32 KiB of them, so the
    // uploader's 2 MiB buffers fill up and the draws in flight keep them.
    // The limit is eight of them.
    pool.limit(16 * mebibyte);
    const auto begin = std::chrono::steady_clock::now();
    int draws = 0;
    int refusedRounds = 0;
    while (draws < totalDraws) {
        for (int index = 0; index < drawsPerRound; ++index) {
            glUniform1i(indexLocation, ++draws);
            glDrawArrays(GL_POINTS, 0, 1);
        }
        const GLenum error = glGetError();
        if (error == GL_OUT_OF_MEMORY) {
            ++refusedRounds;
        } else {
            require(error == GL_NO_ERROR,
                "a draw raised an error other than out of memory");
        }
    }
    glUniform1i(indexLocation, 0);
    glDrawArrays(GL_POINTS, 0, 1);
    glFinish();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    std::fprintf(stderr,
        "%d draws in %lld ms; %d rounds of %d met a refused upload; the pool "
        "refused %u requests\n",
        draws,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()),
        refusedRounds, drawsPerRound, pool.refusals());
    pool.unlimit();
    require(refusedRounds == 0,
        "a draw was refused for memory that the finished work gives back");
    require(pixelAt(0, 0) == Pixel {0, 255, 0, 255},
        "the draws did not show the program's colour");
#elif defined(UML_REFUSAL_RETRY_UPLOAD)
    // The picture the upload writes, over a texture that is cleared to red.
    const auto second = picture(side, 2);

    const Target target = makeTarget(side);
    require(!shows(target, side, second), "the second picture is there");

    // The garbage: a texture of the same size the GPU used and the
    // application deleted. It keeps its memory until the context that used it
    // recycles its batch.
    {
        GLuint garbage = 0;
        glGenTextures(1, &garbage);
        glBindTexture(GL_TEXTURE_2D, garbage);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, side, side);
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, garbage, 0);
        glClearColor(0.5F, 0.5F, 0.5F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &garbage);
    }
    require(glGetError() == GL_NO_ERROR, "the garbage was not made");

    // An upload that needs 8 MiB of staging, with 2 MiB of room and 16 MiB of
    // garbage: the texture shows its new picture when the garbage has been
    // given back.
    pool.limit(2 * mebibyte);
    uploadWhileBusy(target, side, second);
    glFinish();
    const GLenum raised = glGetError();
    std::fprintf(stderr, "the upload raised %#x; the pool refused %u requests\n",
        raised, pool.refusals());
    pool.unlimit();
    require(pool.refusals() > 0, "nothing was refused, so nothing was proven");
    require(shows(target, side, second),
        "the upload was dropped for memory that the finished work gives back");
#else
    // An upload that needs 8 MiB of staging, with the limit 2 MiB above what the live textures
    // hold and nothing finished to give back. It is dropped, as it always
    // was, and the host, the context, and the time of the calls are unharmed.
    const auto picture3 = picture(side, 3);
    const Target target = makeTarget(side);
    pool.limit(2 * mebibyte);
    const auto begin = std::chrono::steady_clock::now();
    constexpr int refusedUploads = 8;
    for (int count = 0; count < refusedUploads; ++count)
        uploadWhileBusy(target, side, picture3);
    glFinish();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    std::fprintf(stderr,
        "%d uploads that cannot fit took %lld ms; the pool refused %u "
        "requests; they raised %#x\n",
        refusedUploads, elapsedMs, pool.refusals(), glGetError());
    require(pool.refusals() > 0,
        "the uploads were not refused, so nothing was proven");
    require(elapsed < 20s, "uploads that cannot fit took too long");
    require(!shows(target, side, picture3),
        "an upload that cannot fit landed, so nothing was proven");

    // The limit goes, and the same upload lands.
    pool.unlimit();
    uploadWhileBusy(target, side, picture3);
    glFinish();
    require(shows(target, side, picture3),
        "the upload did not land once the limit was lifted");
#endif

    std::fprintf(stderr, "Refusal retry (%s): ok\n", shapeName);
    return EXIT_SUCCESS;
}
