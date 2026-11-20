// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, so no guest command stream may make the
// host read bytes the guest did not send, dereference an object the guest did
// not name, or fail to tear its context down. The pipeline binary commands
// take several guest-chosen values the generated codec leaves unbounded, and
// each one is checked here the way a guest sends it:
//
//  - vkCreatePipelineBinariesKHR imports a binary under a key whose length the
//    guest names in VkPipelineBinaryKeyKHR::keySize. The registry marks that
//    field noautovalidity and the codec decodes it as a plain number beside a
//    key array that always holds VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR (32)
//    bytes, so a guest chooses keySize freely. An import under a key of 32
//    bytes succeeds (the control); any other size is answered with
//    VK_ERROR_VALIDATION_FAILED and leaves the context live.
//  - The same command takes its binaries from exactly one of a list of keys
//    and data, a pipeline, or a pipeline create info. A request with none of
//    them, or with only a create info on a driver that has no internal cache
//    to look it up in (this one), has no source to read; it is answered with
//    VK_ERROR_VALIDATION_FAILED and leaves the context live.
//  - A command that names a binary by a null handle, or a pipeline creation
//    that reads one, reaches the driver as a null pointer, because the
//    codec's lookup yields NULL for object id 0 and leaves the refusal to the
//    handler. The context is lost instead, as it is for every other null
//    handle a command needs. An id the renderer never made, or made and then
//    destroyed, is refused by the lookup itself.
//  - A second object created under an id that is already in use loses the
//    context, and tearing it down must still be clean.
//
// The test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits hand-encoded Venus command streams, in the style of
// venus_render_pass_lifetime_tests.cpp. A row asks for a reply and reads the
// VkResult the host answered with, or finds that the context was lost.
//
// The overruns and the stale pointers these rows provoke lie past the end of a
// heap block or in freed memory, so they fault deterministically only under
// Guard Malloc, which ends every allocation at a page boundary and unmaps what
// is freed. The test therefore runs each row in a child process that this one
// starts with Guard Malloc inserted, and the child names the frames of a fault
// on its standard error. The parent passes when every child exits with
// status 0.
//
// It is not run under AddressSanitizer, whose allocator Guard Malloc cannot
// share a process with.

#include "require.hpp"
#include "venus_wire.hpp"

#include <execinfo.h>
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
#include <functional>
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
    instanceId = 1, physDevId = 2, deviceId = 3, binaryId = 10, otherBinaryId = 11,
    pipelineId = 20, neverMadeId = 77,
};
constexpr std::uint32_t keyBytes = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
constexpr std::uint32_t replyResourceId = 900;
constexpr std::uint64_t replySize = 16384;       // one host page
constexpr std::size_t binaryDataBytes = 8;

// A request to import binaries, or to make them from another source.
struct CreateRequest {
    bool keysAndData = false;                    // pKeysAndDataInfo: one binary
    std::uint32_t keySize = keyBytes;            // ... under a key of this size
    std::uint64_t pipeline = 0;                  // pipeline
    bool createInfo = false;                     // pPipelineCreateInfo
    std::uint64_t binary = binaryId;             // the guest's id for the binary
};

// vkCreatePipelineBinariesKHR for one binary under the guest's object id
// request.binary, over a key array of the 32 bytes the codec holds and eight
// bytes of data.
void encodeCreate(Stream& s, const CreateRequest& request)
{
    s.command(createPipelineBinaries, true);
    s.u64(deviceId);
    s.ptr(true);                                // pCreateInfo
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR); s.ptr(false);
    s.ptr(request.keysAndData);                 // pKeysAndDataInfo
    if (request.keysAndData) {
        s.u32(1);                               // binaryCount
        s.arraySize(1);                         // pPipelineBinaryKeys
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
        s.u32(request.keySize);
        s.arraySize(keyBytes);
        std::array<std::uint8_t, keyBytes> key{};
        for (std::uint32_t i = 0; i < keyBytes; ++i)
            key[i] = static_cast<std::uint8_t>(0x40 + i);
        s.blob(key.data(), key.size());
        s.arraySize(1);                         // pPipelineBinaryData
        constexpr std::array<std::uint8_t, binaryDataBytes> data = {1, 2, 3, 4, 5, 6, 7, 8};
        s.u64(data.size());
        s.arraySize(data.size());
        s.blob(data.data(), data.size());
    }
    s.u64(request.pipeline);                    // pipeline
    s.ptr(request.createInfo);                  // pPipelineCreateInfo
    if (request.createInfo) {
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
        s.ptr(true);                            // pNext: the flags of the pipeline
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO); s.ptr(false);
        s.u64(0);
    }
    s.ptr(false);                               // pAllocator
    s.ptr(true);                                // pBinaries
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR); s.ptr(false);
    s.u32(1);                                   // pipelineBinaryCount
    s.arraySize(1); s.u64(request.binary);
}

// vkGetPipelineBinaryDataKHR asking how large the binary's data is.
void encodeGetData(Stream& s, std::uint64_t binary)
{
    s.command(getPipelineBinaryData, true);
    s.u64(deviceId);
    s.ptr(true);                                // pInfo
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR); s.ptr(false);
    s.u64(binary);
    s.ptr(true);                                // pPipelineBinaryKey
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
    s.ptr(true); s.u64(0);                      // pPipelineBinaryDataSize
    s.arraySize(0);                             // pPipelineBinaryData: none
}

void encodeDestroy(Stream& s, std::uint64_t binary)
{
    s.command(destroyPipelineBinary);
    s.u64(deviceId);
    s.u64(binary);
    s.ptr(false);                               // pAllocator
}

void encodeRelease(Stream& s, std::uint64_t pipeline)
{
    s.command(releaseCapturedPipelineData, true);
    s.u64(deviceId);
    s.ptr(true);                                // pInfo
    s.u32(VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR); s.ptr(false);
    s.u64(pipeline);
    s.ptr(false);                               // pAllocator
}

// vkCreateComputePipelines for one pipeline, its guest id `pipelineId`, that
// reads one pipeline binary the guest names by id. The stage has no module
// and the layout is none: the binary is what the driver builds the pipeline
// from.
void encodeComputePipelineFromBinary(Stream& s, std::uint64_t binary)
{
    s.command(createComputePipelines, true);
    s.u64(deviceId);
    s.u64(0);                                   // pipelineCache
    s.u32(1); s.arraySize(1);                   // pCreateInfos
    s.u32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    s.ptr(true);                                // pNext: the binaries
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR); s.ptr(false);
    s.u32(1); s.arraySize(1); s.u64(binary);
    s.u32(0);                                   // flags
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO); s.ptr(false);
    s.u32(0);                                   // stage flags
    s.u32(VK_SHADER_STAGE_COMPUTE_BIT);
    s.u64(0);                                   // module: none
    s.string("main");
    s.ptr(false);                               // pSpecializationInfo
    s.u64(0);                                   // layout
    s.u64(0);                                   // basePipelineHandle
    s.u32(0);                                   // basePipelineIndex
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipelineId);          // pPipelines
}

void encodeSetup(Stream& s)
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME};
    extras.pipelineBinaries = true;
    encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId, extras);
}

// The renderer says why it refuses a command; the log makes a failure here
// read as one.
void logRenderer(int, const char* message, void*)
{
    std::fprintf(stderr, "  renderer: %s", message);
}
void retireFence(std::uint32_t, std::uint32_t, std::uint64_t) {}
void contextFatal(std::uint32_t) {}

// What the host did with a command that asked for a reply.
struct Answer {
    bool submitted = false;      // false: the renderer lost the context
    std::int32_t command = 0;    // the reply's command type, when there is one
    std::int32_t result = 0;     // ... and its VkResult
    std::uint64_t dataSize = 0;  // vkGetPipelineBinaryDataKHR's size, when asked
};

// A Venus context with a device that has pipeline binaries and a
// shared-memory reply stream, all gone again when the session ends.
class Session {
public:
    explicit Session(std::uint32_t ctxId) : ctxId_(ctxId)
    {
        require(vkr_renderer_create_context(ctxId_, virtgpuDrmCapsetVenus, 4, "test"),
            "the Venus context could not be created");
        Stream setup;
        encodeSetup(setup);
        require(vkr_renderer_submit_cmd(ctxId_, setup.bytes().data(), setup.size()),
            "the context was lost bringing the device up; the host driver may be "
            "unavailable in this environment, or lacks pipeline binaries");

        int fdType = 0;
        int fd = -1;
        std::uint32_t mapInfo = 0;
        alignas(16) unsigned char vulkanInfo[256] {};
        require(vkr_renderer_create_resource(ctxId_, replyResourceId, 0, replySize,
                    blobFlagUseMappable, &fdType, &fd, &mapInfo, vulkanInfo),
            "the reply stream could not be created");
        fd_ = fd;
        reply_ = mmap(nullptr, replySize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        require(reply_ != MAP_FAILED, "the reply stream could not be mapped");
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session()
    {
        munmap(reply_, replySize);
        close(fd_);
        vkr_renderer_destroy_resource(ctxId_, replyResourceId);
        vkr_renderer_destroy_context(ctxId_);
    }

    // Sends one command, with the reply stream set first, and reads the reply.
    Answer ask(Stream& command)
    {
        std::memset(reply_, 0, 128);
        Stream stream;
        stream.command(setReplyCommandStream);
        stream.ptr(true);
        stream.u32(replyResourceId); stream.u64(0); stream.u64(replySize);
        stream.bytes().insert(stream.bytes().end(), command.bytes().begin(),
            command.bytes().end());
        Answer answer;
        answer.submitted = vkr_renderer_submit_cmd(ctxId_, stream.bytes().data(), stream.size());
        std::int32_t words[2] {};
        std::memcpy(words, reply_, sizeof(words));
        answer.command = words[0];
        answer.result = words[1];
        // vkGetPipelineBinaryDataKHR's reply: command, result, the key as a
        // pointer and a 56-byte structure, then the size's pointer and the
        // size.
        std::memcpy(&answer.dataSize, static_cast<const std::uint8_t*>(reply_) + 80, 8);
        return answer;
    }

    // Sends a command no reply is asked for; false when the context was lost.
    bool send(Stream& command)
    {
        return vkr_renderer_submit_cmd(ctxId_, command.bytes().data(), command.size());
    }

private:
    std::uint32_t ctxId_;
    int fd_ = -1;
    void* reply_ = nullptr;
};

// An import under a key of the size every key has, into the guest's id; true
// when the host made the binary.
bool importsABinary(Session& session, std::uint64_t id)
{
    Stream s;
    encodeCreate(s, {.keysAndData = true, .binary = id});
    const Answer answer = session.ask(s);
    return answer.submitted && answer.command == static_cast<std::int32_t>(createPipelineBinaries)
        && answer.result == VK_SUCCESS;
}

void requireAnswered(const Answer& answer, Cmd command, std::int32_t result, const char* what)
{
    require(answer.submitted, std::string(what) + ": the context was lost instead of the command being answered");
    require(answer.command == static_cast<std::int32_t>(command),
        std::string(what) + ": the reply names another command");
    if (answer.result != result)
        std::fprintf(stderr, "  answered %d, expected %d\n", answer.result, result);
    require(answer.result == result, what);
}

struct Row {
    const char* name;
    std::function<void(Session&)> run;
};

// A row that imports under a key of one size and is answered with the result.
Row keyRow(const char* name, std::uint32_t keySize, std::int32_t result)
{
    return {name, [keySize, result](Session& session) {
        Stream s;
        encodeCreate(s, {.keysAndData = true, .keySize = keySize});
        requireAnswered(session.ask(s), createPipelineBinaries, result,
            result == VK_SUCCESS ? "an import under a key of 32 bytes was refused"
                                 : "an import under a key of the wrong size was not refused");
    }};
}

// A create request that has no source the binaries can come from is refused
// and the context lives on: the next import is served.
Row noSourceRow(const char* name, const CreateRequest& request)
{
    return {name, [request](Session& session) {
        Stream s;
        encodeCreate(s, request);
        requireAnswered(session.ask(s), createPipelineBinaries, VK_ERROR_VALIDATION_FAILED,
            "a request with no source for its binaries was not refused");
        require(importsABinary(session, otherBinaryId),
            "the context did not serve an import after a refused request");
    }};
}

// A command that names an object the renderer cannot find loses the context
// and the host lives on. `encode` builds the command, after any setup it
// does itself, and a binary is made first when `makeABinaryFirst`.
Row contextLostRow(const char* name, std::function<void(Session&, Stream&)> encode,
    bool makeABinaryFirst = false)
{
    return {name, [encode = std::move(encode), makeABinaryFirst](Session& session) {
        if (makeABinaryFirst)
            require(importsABinary(session, binaryId), "the setup import was refused");
        Stream s;
        encode(session, s);
        const Answer answer = session.ask(s);
        require(!answer.submitted,
            "a command that names an object the renderer cannot find was served");
    }};
}

const std::vector<Row>& rows()
{
    static const std::vector<Row> all = {
        // keySize.
        keyRow("key-of-32", keyBytes, VK_SUCCESS),
        keyRow("key-of-0", 0, VK_ERROR_VALIDATION_FAILED),
        keyRow("key-of-31", keyBytes - 1, VK_ERROR_VALIDATION_FAILED),
        keyRow("key-of-33", keyBytes + 1, VK_ERROR_VALIDATION_FAILED),
        keyRow("key-of-4096", 4096, VK_ERROR_VALIDATION_FAILED),
        keyRow("key-of-0xffffffff", 0xffffffffu, VK_ERROR_VALIDATION_FAILED),

        // The source of a create request.
        noSourceRow("create-from-no-source", {}),
        noSourceRow("create-from-a-create-info-only", {.createInfo = true}),

        // vkGetPipelineBinaryDataKHR: a binary that is there, and the three
        // ways of naming one that is not.
        {"get-data-of-a-binary", [](Session& session) {
            require(importsABinary(session, binaryId), "the setup import was refused");
            Stream s;
            encodeGetData(s, binaryId);
            const Answer answer = session.ask(s);
            requireAnswered(answer, getPipelineBinaryData, VK_SUCCESS, "a binary's data size was refused");
            require(answer.dataSize == binaryDataBytes, "the binary's data size is not the imported one");
        }},
        contextLostRow("get-data-of-the-null-handle",
            [](Session&, Stream& s) { encodeGetData(s, 0); }),
        contextLostRow("get-data-of-an-id-never-made",
            [](Session&, Stream& s) { encodeGetData(s, neverMadeId); }),
        contextLostRow("get-data-of-a-destroyed-binary",
            [](Session& session, Stream& s) {
                Stream destroy;
                encodeDestroy(destroy, binaryId);
                require(session.send(destroy), "destroying a binary lost the context");
                encodeGetData(s, binaryId);
            },
            true),

        // vkDestroyPipelineBinaryKHR: a null handle is a no-op the command
        // defines, and an id never made is refused.
        {"destroy-the-null-handle", [](Session& session) {
            Stream s;
            encodeDestroy(s, 0);
            require(session.send(s), "destroying the null handle lost the context");
            require(importsABinary(session, binaryId),
                "the context did not serve an import after destroying the null handle");
        }},
        contextLostRow("destroy-an-id-never-made",
            [](Session&, Stream& s) { encodeDestroy(s, neverMadeId); }),

        // vkReleaseCapturedPipelineDataKHR names a pipeline.
        contextLostRow("release-the-null-pipeline",
            [](Session&, Stream& s) { encodeRelease(s, 0); }),
        contextLostRow("release-a-pipeline-never-made",
            [](Session&, Stream& s) { encodeRelease(s, neverMadeId); }),

        // A pipeline creation reads the binaries its create info names.
        contextLostRow("compute-pipeline-from-the-null-binary",
            [](Session&, Stream& s) { encodeComputePipelineFromBinary(s, 0); }),

        // An object id the guest already used.
        {"create-again-under-a-used-id", [](Session& session) {
            require(importsABinary(session, binaryId), "the setup import was refused");
            Stream s;
            encodeCreate(s, {.keysAndData = true, .binary = binaryId});
            const Answer answer = session.ask(s);
            require(!answer.submitted, "a second object under a used id was accepted");
            // The session's end destroys the context with both objects in it.
        }},
    };
    return all;
}

// Names the frames of a fault, so that a row that crashes the host says where.
void reportFault(int sig, siginfo_t* info, void*)
{
    char text[128];
    const int length = std::snprintf(text, sizeof(text),
        "FAULT: signal %d at address %p\n", sig, info->si_addr);
    (void)!write(STDERR_FILENO, text, static_cast<std::size_t>(length));
    void* frames[40];
    const int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

void installFaultReport()
{
    struct sigaction action {};
    action.sa_sigaction = reportFault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGABRT})
        sigaction(sig, &action, nullptr);
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

// The body: runs one row in the child, with Guard Malloc.
int runChild(const char* name)
{
    if (!guardMallocLoaded()) {
        std::fprintf(stderr, "Guard Malloc is not loaded in the child\n");
        return guardMallocMissingExitCode;
    }
    installFaultReport();
    for (const Row& row : rows()) {
        if (std::strcmp(name, row.name) != 0)
            continue;
        const vkr_renderer_callbacks callbacks {
            .debug_logger = logRenderer,
            .retire_fence = retireFence,
            .context_fatal = contextFatal,
        };
        require(vkr_renderer_init(
                    vkrRendererThreadSync | vkrRendererAsyncFenceCb, &callbacks),
            "the Venus renderer could not be initialised");
        {
            Session session(1);
            row.run(session);
        }
        vkr_renderer_fini();
        return 0;
    }
    uml::testing::fail("no row has the name given");
}

[[nodiscard]] std::string executablePath()
{
    std::vector<char> path(4096);
    auto size = static_cast<std::uint32_t>(path.size());
    require(_NSGetExecutablePath(path.data(), &size) == 0,
        "the test's own path is too long");
    return path.data();
}

// Starts this binary as the child for one row, under Guard Malloc, and returns
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

// With no argument every row runs, each in a child of its own under Guard
// Malloc; with `--child NAME`, that row alone, in this process.
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
    for (const Row& row : rows()) {
        const int status = runChildUnderGuardMalloc(row.name);
        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "row %s: FAILED, the child ended with signal %d\n",
                row.name, WTERMSIG(status));
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
            std::fprintf(stderr, "row %s: FAILED, status %d\n", row.name, code);
            ++failures;
            continue;
        }
        std::fprintf(stderr, "row %s: ok\n", row.name);
    }
    require(failures == 0, std::to_string(failures) + " pipeline binary request rows failed");
    return 0;
#endif
}
