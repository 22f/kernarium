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
//  - A second object created under an id that is already in use, or under id 0,
//    loses the context, and tearing it down must still be clean: the object the
//    context refused is freed whichever order its owner and the owner of the
//    other object are torn down in, and the other object is left alone.
//  - vkGetPipelineKeyKHR takes its pipeline from the first structure of the
//    create info's chain, which the driver reads by that structure's type. A
//    request whose chain is empty, or begins with a structure that is not a
//    graphics, compute or ray tracing pipeline's, is refused with
//    VK_ERROR_VALIDATION_FAILED, and the key it replies with is zero. No
//    create info at all is the key of the device, which is served.
//
// A refusal that the driver would also make is told apart from the renderer's
// own by the line the renderer logs when it refuses: a row that expects the
// renderer to refuse names that line.
//
// The test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits hand-encoded Venus command streams, in the style of
// venus_render_pass_lifetime_tests.cpp. A row asks for a reply and reads the
// VkResult the host answered with, or finds that the context was lost.
//
// Each row runs in a child process under Guard Malloc, and the rows that name
// a leak run a second time under the leak checker, as venus_row_runner.hpp
// describes.

#include "require.hpp"
#include "venus_pipeline_wire.hpp"
#include "venus_row_runner.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
using namespace uml::gpu::testing::venus;


// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, binaryId = 10, otherBinaryId = 11,
    pipelineId = 20, poolId = 30, otherPoolId = 31, commandBufferId = 40, neverMadeId = 77,
};
constexpr std::uint32_t keyBytes = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
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
    encodeComputeInfo(s, {}, 0, {.binaries = true, .binary = binary});
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipelineId);          // pPipelines
}

// vkCreateGraphicsPipelines for one pipeline, its guest id `pipelineId`, that
// reads one pipeline binary the guest names by id. The create info is one the
// runtime accepts up to the point where it reads the binary: a complete
// pipeline whose rasterization state discards every primitive, with one vertex
// stage that has no module (the binary is what the driver builds the pipeline
// from), no layout and no render pass. Anything less faults earlier, in the
// runtime's own reading of the create info, and the row would not show the
// check it names.
void encodeGraphicsPipelineFromBinary(Stream& s, std::uint64_t binary)
{
    s.command(createGraphicsPipelines, true);
    s.u64(deviceId);
    s.u64(0);                                   // pipelineCache
    s.u32(1); s.arraySize(1);                   // pCreateInfos
    encodeGraphicsInfo(s, {.vertex = true}, {.binaries = true, .binary = binary});
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipelineId);          // pPipelines
}

// What the create info of vkGetPipelineKeyKHR carries.
enum class KeyRequest {
    device,           // no create info: the key of the device
    emptyChain,       // a create info with no chain
    robustnessFirst,  // a chain that begins with VkPipelineRobustnessCreateInfo
};

// vkGetPipelineKeyKHR. The key is written by the host and the request carries
// only its type.
void encodeGetKey(Stream& s, KeyRequest request)
{
    s.command(getPipelineKey, true);
    s.u64(deviceId);
    s.ptr(request != KeyRequest::device);       // pPipelineCreateInfo
    if (request != KeyRequest::device) {
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
        if (request == KeyRequest::robustnessFirst) {
            s.ptr(true);
            s.u32(VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO); s.ptr(false);
            for (int field = 0; field < 4; ++field)
                s.u32(0);                       // every behaviour: the device default
        } else {
            s.ptr(false);
        }
    }
    s.ptr(true);                                // pPipelineKey
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
}

// A command pool of the device, under the guest's id.
void encodeCreatePool(Stream& s, std::uint64_t pool)
{
    s.command(createCommandPool);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO); s.u64(0);
    s.u32(0); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(pool);
}

// One command buffer of a pool, under the guest's id.
void encodeAllocateCommandBuffer(Stream& s, std::uint64_t pool, std::uint64_t commandBuffer)
{
    s.command(allocateCommandBuffers);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO); s.u64(0);
    s.u64(pool); s.u32(VK_COMMAND_BUFFER_LEVEL_PRIMARY); s.u32(1);
    s.arraySize(1); s.u64(commandBuffer);
}

void encodeSetup(Stream& s)
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME};
    extras.pipelineBinaries = true;
    encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId, extras);
}

// The lines the renderer logs when it refuses a request of this extension.
constexpr const char* noSourceLine = "names no source for its binaries";
constexpr const char* keySizeLine = "a key is not 32 bytes";
constexpr const char* chainHeadLine =
    "does not begin with a graphics, compute or ray tracing pipeline's";
// What vkGetPipelineBinaryDataKHR's and vkGetPipelineKeyKHR's replies hold
// beyond the result. A data reply is the command, the result, the key as a
// pointer and a 56-byte structure, then the size's pointer and the size; a key
// reply is the command, the result, the key as a pointer and a structure of
// its type, chain, size and 32 bytes.
std::uint64_t dataSizeOf(const Answer& answer)
{
    std::uint64_t size = 0;
    std::memcpy(&size, answer.raw.data() + 80, 8);
    return size;
}
std::uint32_t keySizeOf(const Answer& answer)
{
    std::uint32_t size = 0;
    std::memcpy(&size, answer.raw.data() + 28, 4);
    return size;
}

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

// A row that imports under a key of one size and is answered with the result.
// The driver refuses a key of the wrong size with the same result, so the row
// also asks the renderer to have refused it before the call was forwarded.
Row keyRow(const char* name, std::uint32_t keySize, std::int32_t result)
{
    return {name, [keySize, result](Session& session) {
        Stream s;
        encodeCreate(s, {.keysAndData = true, .keySize = keySize});
        requireAnswered(session.ask(s), createPipelineBinaries, result,
            result == VK_SUCCESS ? "an import under a key of 32 bytes was refused"
                                 : "an import under a key of the wrong size was not refused");
        require(rendererSaid(keySizeLine) == (result != VK_SUCCESS),
            result == VK_SUCCESS ? "the renderer refused a key of 32 bytes"
                                 : "the renderer did not refuse the key itself");
    }};
}

// A create request that has no source the binaries can come from is refused
// and the context lives on: the next import is served. A request that names
// nothing at all is refused by the renderer; one with only a pipeline create
// info is the driver's to refuse, because a driver with an internal cache
// would look it up.
Row noSourceRow(const char* name, const CreateRequest& request, bool refusedByRenderer)
{
    return {name, [request, refusedByRenderer](Session& session) {
        Stream s;
        encodeCreate(s, request);
        requireAnswered(session.ask(s), createPipelineBinaries, VK_ERROR_VALIDATION_FAILED,
            "a request with no source for its binaries was not refused");
        require(rendererSaid(noSourceLine) == refusedByRenderer,
            refusedByRenderer ? "the renderer did not refuse the request itself"
                              : "the renderer refused a request the driver answers");
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

// vkGetPipelineKeyKHR before which a request that dirties the decoder's temp
// pool has been decoded. The refused request's key lands in memory the pool
// held before, so a reply that is not zeroed carries what was left in it.
void dirtyTheTempPool(Session& session)
{
    require(importsABinary(session, otherBinaryId), "the setup import was refused");
}

bool keyIsZero(const Answer& answer)
{
    return keySizeOf(answer) == 0
        && std::all_of(answer.raw.begin() + 40, answer.raw.begin() + 40 + keyBytes,
            [](std::uint8_t b) { return b == 0; });
}

// A create info that names no pipeline is refused, the renderer says so, the
// reply's key is zero, and the context lives on.
Row keyRefusedRow(const char* name, KeyRequest request)
{
    return {name, [request](Session& session) {
        dirtyTheTempPool(session);
        Stream s;
        encodeGetKey(s, request);
        const Answer answer = session.ask(s);
        require(answer.submitted, "a request that names no pipeline lost the context");
        require(keyIsZero(answer), "a refused key request replied with a key that was never written");
        requireAnswered(answer, getPipelineKey, VK_ERROR_VALIDATION_FAILED,
            "a create info that names no pipeline was not refused");
        require(rendererSaid(chainHeadLine), "the renderer did not refuse the request itself");
        Stream next;
        encodeGetKey(next, KeyRequest::device);
        requireAnswered(session.ask(next), getPipelineKey, VK_SUCCESS,
            "the context did not serve a key after a refused request");
    }};
}

// The pipeline's binary is named by a null handle: a pipeline creation that
// reads it loses the context, as the other commands that need a binary do.
// Each row names the command it creates the pipeline with.

// An object of a pool whose id the context refuses is freed with its pool,
// whichever pool is torn down first, and so is one under id 0. The context is
// lost when the object is made, so the teardown of the session does the rest.
// A leak is the only trace a release renderer leaves, so these rows are also
// run under the leak checker (see runLeakCheck).

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

        // The key of a pipeline, and of the device.
        {"get-the-key-of-the-device", [](Session& session) {
            Stream s;
            encodeGetKey(s, KeyRequest::device);
            const Answer answer = session.ask(s);
            requireAnswered(answer, getPipelineKey, VK_SUCCESS,
                "the key of the device was not served");
            require(keySizeOf(answer) == keyBytes, "the key of the device is not a key's size");
            require(!keyIsZero(answer) && keySizeOf(answer) == keyBytes,
                "the key of the device is zero");
            require(!rendererSaid(chainHeadLine), "the renderer refused the key of the device");
        }},
        keyRefusedRow("get-the-key-of-an-empty-chain", KeyRequest::emptyChain),
        keyRefusedRow("get-the-key-of-a-robustness-chain", KeyRequest::robustnessFirst),

        // The source of a create request.
        noSourceRow("create-from-no-source", {}, true),
        noSourceRow("create-from-a-create-info-only", {.createInfo = true}, false),

        // vkGetPipelineBinaryDataKHR: a binary that is there, and the three
        // ways of naming one that is not.
        {"get-data-of-a-binary", [](Session& session) {
            require(importsABinary(session, binaryId), "the setup import was refused");
            Stream s;
            encodeGetData(s, binaryId);
            const Answer answer = session.ask(s);
            requireAnswered(answer, getPipelineBinaryData, VK_SUCCESS, "a binary's data size was refused");
            require(dataSizeOf(answer) == binaryDataBytes, "the binary's data size is not the imported one");
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

        // A pipeline creation reads the binaries its create info names. The
        // ray tracing form is the same code; this driver has no ray tracing,
        // so there is no row for it.
        contextLostRow("compute-pipeline-from-the-null-binary",
            [](Session&, Stream& s) { encodeComputePipelineFromBinary(s, 0); }),
        contextLostRow("graphics-pipeline-from-the-null-binary",
            [](Session&, Stream& s) { encodeGraphicsPipelineFromBinary(s, 0); }),

        // An object id the guest already used.
        {"create-again-under-a-used-id", [](Session& session) {
            require(importsABinary(session, binaryId), "the setup import was refused");
            Stream s;
            encodeCreate(s, {.keysAndData = true, .binary = binaryId});
            const Answer answer = session.ask(s);
            require(!answer.submitted, "a second object under a used id was accepted");
            // The session's end destroys the context with both objects in it.
        }, true},

        // An object the context refuses is freed whichever owner is torn
        // down first.  Id 0 names no object and finds none in the table.
        {"command-buffer-under-id-0", [](Session& session) {
            Stream pool;
            encodeCreatePool(pool, poolId);
            require(session.send(pool), "a command pool was refused");
            Stream s;
            encodeAllocateCommandBuffer(s, poolId, 0);
            require(!session.send(s), "a command buffer under id 0 was accepted");
        }, true},
        // The older pool holds the duplicate and the newer one the original,
        // and the device tears the newer pool down first: the original leaves
        // the table before the duplicate is removed.
        {"command-buffer-in-another-pool-under-a-used-id", [](Session& session) {
            Stream pools;
            encodeCreatePool(pools, poolId);
            encodeCreatePool(pools, otherPoolId);
            require(session.send(pools), "a command pool was refused");
            Stream original;
            encodeAllocateCommandBuffer(original, otherPoolId, commandBufferId);
            require(session.send(original), "the first command buffer was refused");
            Stream duplicate;
            encodeAllocateCommandBuffer(duplicate, poolId, commandBufferId);
            require(!session.send(duplicate), "a second command buffer under a used id was accepted");
        }, true},
        // The same order in one pool: the original is removed with the pool
        // while the duplicate is still tracked by it.
        {"command-buffer-in-the-same-pool-under-a-used-id", [](Session& session) {
            Stream pool;
            encodeCreatePool(pool, poolId);
            require(session.send(pool), "a command pool was refused");
            Stream original;
            encodeAllocateCommandBuffer(original, poolId, commandBufferId);
            require(session.send(original), "the first command buffer was refused");
            Stream duplicate;
            encodeAllocateCommandBuffer(duplicate, poolId, commandBufferId);
            require(!session.send(duplicate), "a second command buffer under a used id was accepted");
        }, true},
    };
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    const row_runner::Suite suite {rows(), encodeSetup, "pipeline binary request"};
    return row_runner::main(argc, argv, suite);
}
