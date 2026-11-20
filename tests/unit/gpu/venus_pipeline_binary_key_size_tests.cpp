// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, so a guest command stream must never make
// the host copy bytes the guest did not send, or write past what it allocated.
// vkCreatePipelineBinariesKHR imports a pipeline binary under a key whose
// length the guest names in VkPipelineBinaryKeyKHR::keySize. The registry marks
// that field noautovalidity and the codec decodes it as a plain number beside
// a key array that always holds VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR (32)
// bytes, so a guest chooses keySize freely. The driver's runtime used to copy
// that many bytes into the 32-byte key of the binary it was making, guarded
// only by an assert that release builds compile out, and the renderer
// forwarded the field unchecked.
//
// This test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits hand-encoded Venus command streams, in the style of
// venus_render_pass_lifetime_tests.cpp. Each case runs in a context of its own,
// asks for a reply and reads the VkResult the host answered with:
//
//  - an import under a key of 32 bytes succeeds (the control);
//  - an import under a key of 0, 31, 33, 4096 or 0xffffffff bytes is answered
//    with VK_ERROR_VALIDATION_FAILED and leaves the context live.
//
// The read and the write that a key of more than 32 bytes causes lie past the
// end of a heap block, so they fault deterministically only under Guard
// Malloc, which ends every allocation at a page boundary. The test therefore
// runs each case in a child process that this one starts with Guard Malloc
// inserted. The parent passes when every child exits with status 0. On a
// renderer and driver without the checks the 4096 and 0xffffffff cases fault
// the child, and the others are answered with success.
//
// It is not run under AddressSanitizer, whose allocator Guard Malloc cannot
// share a process with.

#include "require.hpp"
#include "venus_wire.hpp"

#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_TEST_ADDRESS_SANITIZER 1
#endif
#endif

namespace {

using uml::testing::require;
using namespace std::chrono_literals;
using namespace uml::gpu::testing::venus;

constexpr const char* childArgument = "--child";
constexpr const char* guardMalloc = "/usr/lib/libgmalloc.dylib";
constexpr int guardMallocMissingExitCode = 2;
constexpr int skippedExitCode = 77;
constexpr auto childTimeBudget = 100s;

// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, binaryId = 10,
};
constexpr std::uint32_t keyBytes = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
constexpr std::uint32_t replyResourceId = 900;
constexpr std::uint64_t replySize = 16384;       // one host page

void encodeSetup(Stream& s)
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME};
    extras.pipelineBinaries = true;
    encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId, extras);
}

// vkCreatePipelineBinariesKHR importing one binary under a key that claims
// keySize bytes, over a key array of the 32 the codec holds and eight bytes of
// data, into the guest's object id binaryId.
void encodeImport(Stream& s, std::uint32_t keySize)
{
    s.command(createPipelineBinaries, true);
    s.u64(deviceId);
    s.ptr(true);                                // pCreateInfo
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR); s.ptr(false);
    s.ptr(true);                                // pKeysAndDataInfo
    s.u32(1);                                   // binaryCount
    s.arraySize(1);                             // pPipelineBinaryKeys
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
    s.u32(keySize);
    s.arraySize(keyBytes);
    std::array<std::uint8_t, keyBytes> key{};
    for (std::uint32_t i = 0; i < keyBytes; ++i)
        key[i] = static_cast<std::uint8_t>(0x40 + i);
    s.blob(key.data(), key.size());
    s.arraySize(1);                             // pPipelineBinaryData
    constexpr std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    s.u64(data.size());
    s.arraySize(data.size());
    s.blob(data.data(), data.size());
    s.u64(0);                                   // pipeline: none
    s.ptr(false);                               // pPipelineCreateInfo
    s.ptr(false);                               // pAllocator
    s.ptr(true);                                // pBinaries
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR); s.ptr(false);
    s.u32(1);                                   // pipelineBinaryCount
    s.arraySize(1); s.u64(binaryId);
}

// The renderer says why it refuses a command; the log makes a failure here
// read as one.
void logRenderer(int, const char* message, void*)
{
    std::fprintf(stderr, "  renderer: %s", message);
}
void retireFence(std::uint32_t, std::uint32_t, std::uint64_t) {}
void contextFatal(std::uint32_t) {}

struct Case {
    const char* name;
    std::uint32_t keySize;
    // What the host answers the import with.
    std::int32_t result;
};

constexpr Case cases[] = {
    {"key-of-32", keyBytes, VK_SUCCESS},
    {"key-of-0", 0, VK_ERROR_VALIDATION_FAILED},
    {"key-of-31", keyBytes - 1, VK_ERROR_VALIDATION_FAILED},
    {"key-of-33", keyBytes + 1, VK_ERROR_VALIDATION_FAILED},
    {"key-of-4096", 4096, VK_ERROR_VALIDATION_FAILED},
    {"key-of-0xffffffff", 0xffffffffu, VK_ERROR_VALIDATION_FAILED},
};

// Brings a context up, gives it a shared-memory reply stream, imports a binary
// under the case's key size and checks the result that came back.
void runCase(std::uint32_t ctxId, const Case& c)
{
    require(vkr_renderer_create_context(ctxId, virtgpuDrmCapsetVenus, 4, "test"),
        "the Venus context could not be created");

    Stream setup;
    encodeSetup(setup);
    require(vkr_renderer_submit_cmd(ctxId, setup.bytes().data(), setup.size()),
        "the context was lost bringing the device up; the host driver may be "
        "unavailable in this environment, or lacks pipeline binaries");

    int fdType = 0;
    int fd = -1;
    std::uint32_t mapInfo = 0;
    alignas(16) unsigned char vulkanInfo[256] {};
    require(vkr_renderer_create_resource(ctxId, replyResourceId, 0, replySize,
                blobFlagUseMappable, &fdType, &fd, &mapInfo, vulkanInfo),
        "the reply stream could not be created");
    void* reply = mmap(nullptr, replySize, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    require(reply != MAP_FAILED, "the reply stream could not be mapped");

    Stream stream;
    stream.command(setReplyCommandStream);
    stream.ptr(true);
    stream.u32(replyResourceId); stream.u64(0); stream.u64(replySize);
    encodeImport(stream, c.keySize);
    require(vkr_renderer_submit_cmd(ctxId, stream.bytes().data(), stream.size()),
        "an import under a key of the wrong size lost the context instead of "
        "being refused");

    // The reply starts with the command's type, then its VkResult.
    std::int32_t words[2] {};
    std::memcpy(words, reply, sizeof(words));
    require(words[0] == static_cast<std::int32_t>(createPipelineBinaries),
        "the reply names another command");
    if (words[1] != c.result)
        std::fprintf(stderr, "  answered %d, expected %d\n", words[1], c.result);
    require(words[1] == c.result,
        c.result == VK_SUCCESS
            ? "an import under a key of 32 bytes was refused"
            : "an import under a key of the wrong size was not refused");

    munmap(reply, replySize);
    close(fd);
    vkr_renderer_destroy_resource(ctxId, replyResourceId);
    vkr_renderer_destroy_context(ctxId);
}

[[nodiscard]] bool guardMallocLoaded()
{
    for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char* const name = _dyld_get_image_name(i);
        if (name != nullptr && std::strstr(name, "libgmalloc") != nullptr)
            return true;
    }
    return false;
}

// The body: runs one case in the child, with Guard Malloc.
int runChild(const char* name)
{
    if (!guardMallocLoaded()) {
        std::fprintf(stderr, "Guard Malloc is not loaded in the child\n");
        return guardMallocMissingExitCode;
    }
    for (const Case& c : cases) {
        if (std::strcmp(name, c.name) != 0)
            continue;
        const vkr_renderer_callbacks callbacks {
            .debug_logger = logRenderer,
            .retire_fence = retireFence,
            .context_fatal = contextFatal,
        };
        require(vkr_renderer_init(
                    vkrRendererThreadSync | vkrRendererAsyncFenceCb, &callbacks),
            "the Venus renderer could not be initialised");
        runCase(1, c);
        vkr_renderer_fini();
        return 0;
    }
    uml::testing::fail("no case has the name given");
}

[[nodiscard]] std::string executablePath()
{
    std::vector<char> path(4096);
    auto size = static_cast<std::uint32_t>(path.size());
    require(_NSGetExecutablePath(path.data(), &size) == 0,
        "the test's own path is too long");
    return path.data();
}

// Starts this binary as the child for one case, under Guard Malloc, and returns
// its wait status.
[[nodiscard]] int runChildUnderGuardMalloc(const char* name)
{
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string text = *entry;
        if (text.starts_with("DYLD_INSERT_LIBRARIES="))
            continue;
        variables.push_back(text);
    }
    variables.push_back(std::string("DYLD_INSERT_LIBRARIES=") + guardMalloc);
    std::vector<char*> environment;
    for (std::string& text : variables)
        environment.push_back(text.data());
    environment.push_back(nullptr);

    const std::string path = executablePath();
    std::array<char*, 4> arguments {
        const_cast<char*>(path.c_str()),
        const_cast<char*>(childArgument),
        const_cast<char*>(name),
        nullptr,
    };
    pid_t child = 0;
    const int spawned = posix_spawn(&child, path.c_str(), nullptr, nullptr,
        arguments.data(), environment.data());
    require(spawned == 0, "the child could not be started");

    const auto deadline = std::chrono::steady_clock::now() + childTimeBudget;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return status;
        require(waited == 0, "waiting for the child failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            uml::testing::fail("the child did not finish in time");
        }
        std::this_thread::sleep_for(50ms);
    }
}

} // namespace

// With no argument every case runs, each in a child of its own under Guard
// Malloc; with `--child NAME`, that case alone, in this process.
int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], childArgument) == 0)
        return runChild(argv[2]);

#ifdef UML_TEST_ADDRESS_SANITIZER
    std::fprintf(stderr,
        "Guard Malloc cannot share a process with AddressSanitizer; skipping\n");
    return skippedExitCode;
#else
    if (::access(guardMalloc, R_OK) != 0) {
        std::fprintf(stderr, "Guard Malloc is not installed; skipping\n");
        return skippedExitCode;
    }

    std::uint32_t failures = 0;
    for (const Case& c : cases) {
        const int status = runChildUnderGuardMalloc(c.name);
        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "case %s: FAILED, the child ended with signal %d\n",
                c.name, WTERMSIG(status));
            ++failures;
            continue;
        }
        require(WIFEXITED(status), "the child neither exited nor was signalled");
        const int code = WEXITSTATUS(status);
        if (code == skippedExitCode)
            return code;
        require(code != guardMallocMissingExitCode,
            "the child ran without Guard Malloc, so it cannot show the fault");
        if (code != 0) {
            std::fprintf(stderr, "case %s: FAILED, status %d\n", c.name, code);
            ++failures;
            continue;
        }
        std::fprintf(stderr, "case %s: ok\n", c.name);
    }
    require(failures == 0, std::to_string(failures) + " pipeline binary key size cases failed");
    return 0;
#endif
}
