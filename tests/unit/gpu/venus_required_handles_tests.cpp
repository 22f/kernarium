// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, and in the product the renderer runs in
// the app's own process, so no command stream may make the host dereference a
// pointer the guest left null or allocate what the guest chose without bound.
// The generated codec looks a handle up and yields NULL for object id 0, and
// refuses an id that names no object of the type; it refuses id 0 only for
// the first, dispatchable argument of a command. A handle the specification
// does not allow to be VK_NULL_HANDLE therefore reaches its handler as a null
// pointer, which the renderer reads or the driver dereferences. Each row
// sends such a command the way a guest does:
//
//  - the memory requirements and the binds of buffers and images, a
//    framebuffer's render pass, an image view's conversion, the pools of the
//    commands that free what they made, the commands that copy to and from
//    device memory (VK_KHR_device_address_commands), the semaphore of a
//    semaphore import, and a device group's physical devices, each named by
//    the null handle. The renderer logs which one and ends the context, as it
//    does for an id that names no object, and the host lives on: a context of
//    its own is served afterwards.
//  - a shader stage with no module and nothing chained to name its shader, or
//    with no entry point name, in a pipeline creation or a key request. The
//    registry lets the module be null, so the codec cannot refuse it, and the
//    runtime hashed the stage by reading the identifier it does not have. A
//    creation ends the context; a key request answers
//    VK_ERROR_VALIDATION_FAILED and the context lives. A stage with a
//    chained module is served, and so is one whose pipeline is made from the
//    binaries it captured, which the specification says ignore the stage's
//    shaders.
//  - a pipeline create info that chains a library which is the null handle.
//  - a host image copy (VK_EXT_host_image_copy) of a region the image cannot
//    hold: a mip level, an aspect, an offset or an extent the image does not
//    have, or a row length or image height that makes the region reach
//    further than the memory the guest sent. The command is answered with
//    VK_ERROR_OUT_OF_HOST_MEMORY, as for a region whose footprint cannot be
//    stated, and the renderer logs why. A region the image holds is copied,
//    and read back as it was written.
//
// A row that expects the renderer to refuse names the line it logs, so that a
// driver that refuses the same request is not mistaken for the renderer.
//
// The test drives the real vkr renderer in-process against the real host
// Vulkan driver, with hand-encoded Venus command streams. Each row runs in a
// child process under Guard Malloc, as venus_row_runner.hpp describes.

#include "require.hpp"
#include "venus_pipeline_wire.hpp"
#include "venus_row_runner.hpp"

#include <algorithm>
#include <array>
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
    instanceId = 1, physDevId = 2, deviceId = 3, otherDeviceId = 4,
    imageId = 10, bufferId = 20, memoryId = 30, imageMemoryId = 31,
    poolId = 40, commandBufferId = 41, moduleId = 50, layoutId = 51,
    pipelineId = 60, otherPipelineId = 61, binaryId = 70,
};
constexpr std::uint32_t extent = 16;
constexpr std::uint64_t memorySize = 64 * 1024;
constexpr std::uint32_t texelBytes = 4;
// A row length that no memory of a test could hold a row of.
constexpr std::uint32_t hugeRowLength = 0x10000000;

// VK_STRUCTURE_TYPE_ values of the Venus protocol's own structures.
constexpr std::uint32_t importSemaphoreResourceInfoMesa = 1000384004;
constexpr std::uint32_t copyImageToMemoryInfoMesa = 1000384008;
constexpr std::uint32_t memoryToImageCopyMesa = 1000384009;
constexpr std::uint32_t copyMemoryToImageInfoMesa = 1000384010;

// ---------------------------------------------------------------- devices

DeviceExtras plainDevice() { return {}; }

DeviceExtras hostCopyDevice()
{
    DeviceExtras extras;
    extras.extensions = {VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME};
    extras.features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES};
    return extras;
}

DeviceExtras addressCommandsDevice()
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME};
    extras.features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    return extras;
}

DeviceExtras semaphoreFdDevice()
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    return extras;
}

DeviceExtras pipelineBinaryDevice()
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME};
    extras.pipelineBinaries = true;
    return extras;
}

std::function<void(Stream&)> setupOf(const DeviceExtras& extras)
{
    return [extras](Stream& s) {
        encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId, extras);
    };
}

// ---------------------------------------------------------------- encoders

void encodeBuffer(Stream& s, std::uint64_t id, bool reply = false)
{
    s.command(createBuffer, reply);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO); s.u64(0);
    s.u32(0); s.u64(4096);
    s.u32(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.u32(VK_SHARING_MODE_EXCLUSIVE); s.u32(0); s.arraySize(0);
    s.ptr(false);
    s.ptr(true); s.u64(id);
}

void encodeImage(Stream& s, std::uint64_t id, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
    std::uint32_t mipLevels = 1, VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    std::uint32_t arrayLayers = 1)
{
    s.command(createImage);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(VK_IMAGE_TYPE_2D); s.u32(format);
    s.u32(extent); s.u32(extent); s.u32(1);
    s.u32(mipLevels); s.u32(arrayLayers);
    s.u32(VK_SAMPLE_COUNT_1_BIT); s.u32(VK_IMAGE_TILING_OPTIMAL);
    s.u32(usage);
    s.u32(VK_SHARING_MODE_EXCLUSIVE); s.u32(0); s.arraySize(0);
    s.u32(VK_IMAGE_LAYOUT_UNDEFINED);
    s.ptr(false);
    s.ptr(true); s.u64(id);
}

void encodeMemory(Stream& s, std::uint64_t id)
{
    s.command(allocateMemory);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO); s.u64(0);
    s.u64(memorySize); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(id);
}

void encodePool(Stream& s, std::uint64_t pool)
{
    s.command(createCommandPool);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO); s.u64(0);
    s.u32(0); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(pool);
}

// A command buffer of the pool, in the recording state.
void encodeRecordingCommandBuffer(Stream& s, std::uint64_t pool, std::uint64_t commandBuffer)
{
    s.command(allocateCommandBuffers);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO); s.u64(0);
    s.u64(pool); s.u32(VK_COMMAND_BUFFER_LEVEL_PRIMARY); s.u32(1);
    s.arraySize(1); s.u64(commandBuffer);

    s.command(beginCommandBuffer);
    s.u64(commandBuffer);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO); s.u64(0);
    s.u32(0); s.ptr(false);
}

void encodePipelineLayout(Stream& s, std::uint64_t layout)
{
    s.command(createPipelineLayout);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(0); s.arraySize(0);
    s.u32(0); s.arraySize(0);
    s.ptr(false);
    s.ptr(true); s.u64(layout);
}

void encodeGetBufferRequirements(Stream& s, std::uint64_t buffer)
{
    s.command(getBufferMemoryRequirements);
    s.u64(deviceId); s.u64(buffer); s.ptr(true);
}

void encodeGetBufferRequirements2(Stream& s, std::uint64_t buffer)
{
    s.command(getBufferMemoryRequirements2);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2); s.u64(0);
    s.u64(buffer);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2); s.u64(0);
}

void encodeGetImageRequirements(Stream& s, std::uint64_t image)
{
    s.command(getImageMemoryRequirements);
    s.u64(deviceId); s.u64(image); s.ptr(true);
}

void encodeGetImageRequirements2(Stream& s, std::uint64_t image)
{
    s.command(getImageMemoryRequirements2);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2); s.u64(0);
    s.u64(image);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2); s.u64(0);
}

void encodeBindBuffer(Stream& s, std::uint64_t buffer, std::uint64_t memory)
{
    s.command(bindBufferMemory);
    s.u64(deviceId); s.u64(buffer); s.u64(memory); s.u64(0);
}

void encodeBindBuffer2(Stream& s, std::uint64_t buffer, std::uint64_t memory)
{
    s.command(bindBufferMemory2);
    s.u64(deviceId);
    s.u32(1); s.arraySize(1);
    s.u32(VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO); s.u64(0);
    s.u64(buffer); s.u64(memory); s.u64(0);
}

void encodeBindImage(Stream& s, std::uint64_t image, std::uint64_t memory)
{
    s.command(bindImageMemory);
    s.u64(deviceId); s.u64(image); s.u64(memory); s.u64(0);
}

void encodeBindImage2(Stream& s, std::uint64_t image, std::uint64_t memory)
{
    s.command(bindImageMemory2);
    s.u64(deviceId);
    s.u32(1); s.arraySize(1);
    s.u32(VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO); s.u64(0);
    s.u64(image); s.u64(memory); s.u64(0);
}

void encodeFreeCommandBuffers(Stream& s, std::uint64_t pool)
{
    s.command(freeCommandBuffers);
    s.u64(deviceId); s.u64(pool);
    s.u32(0); s.arraySize(0);
}

void encodeFreeDescriptorSets(Stream& s, std::uint64_t pool)
{
    s.command(freeDescriptorSets);
    s.u64(deviceId); s.u64(pool);
    s.u32(0); s.arraySize(0);
}

void encodeFramebuffer(Stream& s, std::uint64_t renderPass)
{
    s.command(createFramebuffer);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO); s.u64(0);
    s.u32(0); s.u64(renderPass);
    s.u32(0); s.arraySize(0);
    s.u32(extent); s.u32(extent); s.u32(1);
    s.ptr(false);
    s.ptr(true); s.u64(80);
}

// An image view of the image with a Y'CbCr conversion chained, the null handle.
void encodeViewWithConversion(Stream& s, std::uint64_t image, std::uint64_t view)
{
    s.command(createImageView);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO); s.ptr(false);
    s.u64(0);                                   // the conversion
    s.u32(0); s.u64(image);
    s.u32(VK_IMAGE_VIEW_TYPE_2D); s.u32(VK_FORMAT_R8G8B8A8_UNORM);
    for (int component = 0; component < 4; ++component)
        s.u32(VK_COMPONENT_SWIZZLE_IDENTITY);
    s.u32(VK_IMAGE_ASPECT_COLOR_BIT); s.u32(0); s.u32(1); s.u32(0); s.u32(1);
    s.ptr(false);
    s.ptr(true); s.u64(view);
}

void encodeCopyQueryPoolResultsToMemory(Stream& s, std::uint64_t queryPool)
{
    s.command(cmdCopyQueryPoolResultsToMemory);
    s.u64(commandBufferId); s.u64(queryPool);
    s.u32(0); s.u32(1);
    s.ptr(true); s.u64(0); s.u64(0); s.u64(0);  // pDstRange
    s.u32(0); s.u32(0);
}

void encodeCopyDeviceMemoryImage(Stream& s, Cmd command, std::uint64_t image)
{
    s.command(command);
    s.u64(commandBufferId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR); s.ptr(false);
    s.u64(image);
    s.u32(0); s.arraySize(0);
}

void encodeImportSemaphore(Stream& s, std::uint64_t semaphore)
{
    s.command(importSemaphoreResource);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(importSemaphoreResourceInfoMesa); s.ptr(false);
    s.u64(semaphore); s.u32(0);
}

// A pipeline from one of the compute creation forms the rows name.
void encodeComputePipeline(Stream& s, std::uint64_t pipeline, const StageSource& source,
    const PipelineChain& chain = {}, std::uint64_t layout = 0, bool reply = true)
{
    s.command(createComputePipelines, reply);
    s.u64(deviceId);
    s.u64(0);                                   // pipelineCache
    s.u32(1); s.arraySize(1);
    encodeComputeInfo(s, source, layout, chain);
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipeline);
}

void encodeGraphicsPipeline(Stream& s, std::uint64_t pipeline, const GraphicsStages& stages,
    const PipelineChain& chain = {})
{
    s.command(createGraphicsPipelines, true);
    s.u64(deviceId);
    s.u64(0);
    s.u32(1); s.arraySize(1);
    encodeGraphicsInfo(s, stages, chain);
    s.ptr(false);
    s.arraySize(1); s.u64(pipeline);
}

// vkGetPipelineKeyKHR of a compute or a graphics pipeline create info chained
// to a VkPipelineCreateInfoKHR, as the driver reads it.
void encodeKeyOfCompute(Stream& s, const StageSource& source, const PipelineChain& chain = {})
{
    s.command(getPipelineKey, true);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    encodePipelineChain(s, chain);
    encodeComputeSelf(s, source, 0);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
}

void encodeKeyOfGraphics(Stream& s, const GraphicsStages& stages, const PipelineChain& chain = {})
{
    s.command(getPipelineKey, true);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    encodePipelineChain(s, chain);
    encodeGraphicsSelf(s, stages);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
}

void encodeKeyOfDevice(Stream& s)
{
    s.command(getPipelineKey, true);
    s.u64(deviceId);
    s.ptr(false);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
}

// vkCreatePipelineBinariesKHR of a pipeline that was made to capture them: the
// binaries land under the guest's id `binaryId`, one of them.
void encodeBinariesOfPipeline(Stream& s, std::uint64_t pipeline, std::uint64_t binary)
{
    s.command(createPipelineBinaries, true);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR); s.ptr(false);
    s.ptr(false);                               // pKeysAndDataInfo
    s.u64(pipeline);
    s.ptr(false);                               // pPipelineCreateInfo
    s.ptr(false);                               // pAllocator
    s.ptr(true);                                // pBinaries
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR); s.ptr(false);
    s.u32(1);
    s.arraySize(1); s.u64(binary);
}

// vkCreateComputePipelines of the empty shader that is to capture its binaries.
void encodeCapturingComputePipeline(Stream& s, std::uint64_t pipeline, std::uint64_t layout)
{
    s.command(createComputePipelines, true);
    s.u64(deviceId);
    s.u64(0);
    s.u32(1); s.arraySize(1);
    s.u32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    s.ptr(true);                                // pNext: the flags that ask for the data
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO); s.ptr(false);
    s.u64(VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR);
    encodeComputeSelf(s, {.moduleInfo = true}, layout);
    s.ptr(false);
    s.arraySize(1); s.u64(pipeline);
}

// vkCopyImageToMemoryMESA of a region of the image, into a reply blob of
// dataSize bytes.
struct HostCopy {
    std::uint32_t layers = 1;
    std::uint32_t mipLevel = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t rowLength = 0;
    std::uint32_t imageHeight = 0;
    std::int32_t offset = 0;
    std::uint32_t width = 4;
    std::uint32_t height = 4;
    std::uint32_t memoryBytes = 4 * 4 * texelBytes;
};

void encodeSubresourceRegion(Stream& s, const HostCopy& copy)
{
    s.u32(copy.aspect); s.u32(copy.mipLevel); s.u32(0); s.u32(copy.layers);
    s.u32(static_cast<std::uint32_t>(copy.offset)); s.u32(0); s.u32(0);
    s.u32(copy.width); s.u32(copy.height); s.u32(1);
}

void encodeCopyImageToMemory(Stream& s, std::uint64_t image, const HostCopy& copy)
{
    s.command(copyImageToMemory, true);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(copyImageToMemoryInfoMesa); s.ptr(false);
    s.u32(0);                                   // flags
    s.u64(image); s.u32(VK_IMAGE_LAYOUT_GENERAL);
    s.u32(copy.rowLength); s.u32(copy.imageHeight);
    encodeSubresourceRegion(s, copy);
    s.u64(copy.memoryBytes);                    // dataSize
    s.arraySize(copy.memoryBytes);              // pData, an out array
}

void encodeCopyMemoryToImage(Stream& s, std::uint64_t image, const HostCopy& copy,
    const std::vector<std::uint8_t>& data, bool reply = true)
{
    s.command(copyMemoryToImage, reply);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(copyMemoryToImageInfoMesa); s.ptr(false);
    s.u32(0);                                   // flags
    s.u64(image); s.u32(VK_IMAGE_LAYOUT_GENERAL);
    s.u32(1); s.arraySize(1);                   // one region
    s.u32(memoryToImageCopyMesa); s.ptr(false);
    s.u64(data.size());
    s.arraySize(data.size());
    s.blob(data.data(), data.size());
    s.u32(copy.rowLength); s.u32(copy.imageHeight);
    encodeSubresourceRegion(s, copy);
}

// The image, made ready for a host copy: its memory bound and its layout
// general.
void encodeHostCopyImage(Stream& s, std::uint64_t image, std::uint64_t memory,
    std::uint32_t arrayLayers = 1)
{
    constexpr std::uint32_t mipLevels = 1;
    encodeImage(s, image, VK_FORMAT_R8G8B8A8_UNORM, mipLevels,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT, arrayLayers);
    s.command(getImageMemoryRequirements);
    s.u64(deviceId); s.u64(image); s.ptr(true);
    encodeMemory(s, memory);
    encodeBindImage(s, image, memory);
    s.command(transitionImageLayout);
    s.u64(deviceId);
    s.u32(1); s.arraySize(1);
    s.u32(VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO); s.ptr(false);
    s.u64(image);
    s.u32(VK_IMAGE_LAYOUT_UNDEFINED); s.u32(VK_IMAGE_LAYOUT_GENERAL);
    s.u32(VK_IMAGE_ASPECT_COLOR_BIT); s.u32(0); s.u32(mipLevels); s.u32(0); s.u32(arrayLayers);
}

// ---------------------------------------------------------------- rows

// The line the renderer logs when a handle is the null handle, and the one it
// logs when a shader stage names no shader.
constexpr const char* nullHandleLine = "is the null handle";
constexpr const char* noShaderLine = "a pipeline stage names no shader";
constexpr const char* noEntryPointLine = "a pipeline stage names no entry point";

// The row names the handler that refused, which the log line begins with.
bool refusedAsNull(const char* handler)
{
    return rendererSaid((std::string(handler) + ": ").c_str()) && rendererSaid(nullHandleLine);
}

// A context of its own is still served: a host that lives on answers a buffer.
void requireFreshContextServed(const DeviceExtras& extras)
{
    Session fresh(2, setupOf(extras), replyResourceId + 1);
    Stream s;
    encodeBuffer(s, bufferId, true);
    requireAnswered(fresh.ask(s), createBuffer, VK_SUCCESS,
        "a context of its own was not served after another was lost");
}

// A command that names a required handle as the null handle ends the context
// and the renderer says which; the host lives on. `fixture` makes the objects
// the command names besides, `command` encodes it.
Row nullHandleRow(const char* name, const DeviceExtras& extras, const char* handler,
    std::function<void(Session&)> fixture, std::function<void(Stream&)> command)
{
    return {name, [=](Session& session) {
        if (fixture)
            fixture(session);
        Stream s;
        command(s);
        require(!session.send(s),
            "a command that names the null handle where a handle is required was served");
        require(refusedAsNull(handler), "the renderer did not refuse the null handle itself");
        requireFreshContextServed(extras);
    }, false, setupOf(extras)};
}

std::function<void(Session&)> makes(std::function<void(Stream&)> encode)
{
    return [encode = std::move(encode)](Session& session) {
        Stream s;
        encode(s);
        require(session.send(s), "a fixture object was refused");
    };
}

std::function<void(Session&)> recordingCommandBuffer()
{
    return makes([](Stream& s) {
        encodePool(s, poolId);
        encodeRecordingCommandBuffer(s, poolId, commandBufferId);
    });
}

// A shader stage that names no shader: the creation ends the context and the
// renderer says so.
Row stageRefusedRow(const char* name, const char* line,
    std::function<void(Stream&)> command, const DeviceExtras& extras = plainDevice())
{
    return {name, [=](Session& session) {
        Stream s;
        command(s);
        const Answer answer = session.ask(s);
        require(!answer.submitted, "a stage that names no shader was served");
        require(rendererSaid(line), "the renderer did not refuse the stage itself");
        requireFreshContextServed(extras);
    }, false, setupOf(extras)};
}

// A key request for a pipeline whose stage names no shader is refused, the
// context lives and serves the next request.
Row keyStageRefusedRow(const char* name, const char* line, std::function<void(Stream&)> command)
{
    return {name, [=](Session& session) {
        Stream s;
        command(s);
        requireAnswered(session.ask(s), getPipelineKey, VK_ERROR_VALIDATION_FAILED,
            "the key of a pipeline whose stage names no shader was not refused");
        require(rendererSaid(line), "the renderer did not refuse the stage itself");
        Stream next;
        encodeKeyOfDevice(next);
        requireAnswered(session.ask(next), getPipelineKey, VK_SUCCESS,
            "the context did not serve a key after a refused request");
    }, false, setupOf(pipelineBinaryDevice())};
}

// The region of a host copy, from memory the test chooses, with the image
// already made ready.
std::function<void(Session&)> hostCopyImageReady(std::uint32_t arrayLayers = 1)
{
    return makes([arrayLayers](Stream& s) { encodeHostCopyImage(s, imageId, imageMemoryId, arrayLayers); });
}

// The data a copy of this region has: its bytes, which are not what the
// image holds beforehand.
std::vector<std::uint8_t> patternOf(std::size_t bytes)
{
    std::vector<std::uint8_t> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    return data;
}

// A host image copy of a region the image cannot hold is answered with
// VK_ERROR_OUT_OF_HOST_MEMORY, the renderer says why, and the context lives on:
// the next copy, of a region the image holds, is served.
Row hostCopyRefusedRow(const char* name, const char* line, bool toMemory, const HostCopy& copy,
    std::uint32_t arrayLayers = 1)
{
    return {name, [=](Session& session) {
        hostCopyImageReady(arrayLayers)(session);
        Stream s;
        if (toMemory)
            encodeCopyImageToMemory(s, imageId, copy);
        else
            encodeCopyMemoryToImage(s, imageId, copy, patternOf(copy.memoryBytes));
        requireAnswered(session.ask(s), toMemory ? copyImageToMemory : copyMemoryToImage,
            VK_ERROR_OUT_OF_HOST_MEMORY, "a region the image cannot hold was not refused");
        require(rendererSaid(line), "the renderer did not refuse the region itself");

        Stream next;
        encodeCopyMemoryToImage(next, imageId, {}, patternOf(HostCopy{}.memoryBytes));
        requireAnswered(session.ask(next), copyMemoryToImage, VK_SUCCESS,
            "the context did not serve a copy after a refused region");
    }, false, setupOf(hostCopyDevice())};
}

constexpr const char* mipLine = "is past the image's";
constexpr const char* aspectLine = "is not one aspect of the image";
constexpr const char* outsideLine = "lies outside the image's extent";
constexpr const char* reachLine = "past what its";

const std::vector<Row>& rows()
{
    static const std::vector<Row> all = {
        // The memory requirements and the binds of buffers and images.
        nullHandleRow("get-buffer-memory-requirements-of-the-null-buffer", plainDevice(),
            "vkr_dispatch_vkGetBufferMemoryRequirements", nullptr,
            [](Stream& s) { encodeGetBufferRequirements(s, 0); }),
        nullHandleRow("get-buffer-memory-requirements2-of-the-null-buffer", plainDevice(),
            "vkr_dispatch_vkGetBufferMemoryRequirements2", nullptr,
            [](Stream& s) { encodeGetBufferRequirements2(s, 0); }),
        nullHandleRow("bind-buffer-memory-of-the-null-buffer", plainDevice(),
            "vkr_dispatch_vkBindBufferMemory", makes([](Stream& s) { encodeMemory(s, memoryId); }),
            [](Stream& s) { encodeBindBuffer(s, 0, memoryId); }),
        nullHandleRow("bind-buffer-memory-to-the-null-memory", plainDevice(),
            "vkr_dispatch_vkBindBufferMemory", makes([](Stream& s) { encodeBuffer(s, bufferId); }),
            [](Stream& s) { encodeBindBuffer(s, bufferId, 0); }),
        nullHandleRow("bind-buffer-memory2-of-the-null-buffer", plainDevice(),
            "vkr_dispatch_vkBindBufferMemory2", makes([](Stream& s) { encodeMemory(s, memoryId); }),
            [](Stream& s) { encodeBindBuffer2(s, 0, memoryId); }),
        nullHandleRow("bind-buffer-memory2-to-the-null-memory", plainDevice(),
            "vkr_dispatch_vkBindBufferMemory2", makes([](Stream& s) { encodeBuffer(s, bufferId); }),
            [](Stream& s) { encodeBindBuffer2(s, bufferId, 0); }),
        nullHandleRow("get-image-memory-requirements-of-the-null-image", plainDevice(),
            "vkr_dispatch_vkGetImageMemoryRequirements", nullptr,
            [](Stream& s) { encodeGetImageRequirements(s, 0); }),
        nullHandleRow("get-image-memory-requirements2-of-the-null-image", plainDevice(),
            "vkr_dispatch_vkGetImageMemoryRequirements2", nullptr,
            [](Stream& s) { encodeGetImageRequirements2(s, 0); }),
        nullHandleRow("bind-image-memory-of-the-null-image", plainDevice(),
            "vkr_dispatch_vkBindImageMemory", makes([](Stream& s) { encodeMemory(s, memoryId); }),
            [](Stream& s) { encodeBindImage(s, 0, memoryId); }),
        nullHandleRow("bind-image-memory-to-the-null-memory", plainDevice(),
            "vkr_dispatch_vkBindImageMemory", makes([](Stream& s) { encodeImage(s, imageId); }),
            [](Stream& s) { encodeBindImage(s, imageId, 0); }),
        nullHandleRow("bind-image-memory2-of-the-null-image", plainDevice(),
            "vkr_dispatch_vkBindImageMemory2", makes([](Stream& s) { encodeMemory(s, memoryId); }),
            [](Stream& s) { encodeBindImage2(s, 0, memoryId); }),
        nullHandleRow("bind-image-memory2-to-the-null-memory", plainDevice(),
            "vkr_dispatch_vkBindImageMemory2", makes([](Stream& s) { encodeImage(s, imageId); }),
            [](Stream& s) { encodeBindImage2(s, imageId, 0); }),

        // The objects the other commands name.
        nullHandleRow("free-command-buffers-of-the-null-pool", plainDevice(),
            "vkr_dispatch_vkFreeCommandBuffers", nullptr,
            [](Stream& s) { encodeFreeCommandBuffers(s, 0); }),
        nullHandleRow("free-descriptor-sets-of-the-null-pool", plainDevice(),
            "vkr_dispatch_vkFreeDescriptorSets", nullptr,
            [](Stream& s) { encodeFreeDescriptorSets(s, 0); }),
        nullHandleRow("create-framebuffer-with-the-null-render-pass", plainDevice(),
            "vkr_dispatch_vkCreateFramebuffer", nullptr,
            [](Stream& s) { encodeFramebuffer(s, 0); }),
        nullHandleRow("create-image-view-with-the-null-conversion", plainDevice(),
            "vkr_dispatch_vkCreateImageView",
            makes([](Stream& s) { encodeImage(s, imageId, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_USAGE_SAMPLED_BIT); }),
            [](Stream& s) { encodeViewWithConversion(s, imageId, 90); }),
        nullHandleRow("create-device-of-a-group-with-the-null-physical-device", plainDevice(),
            "vkr_dispatch_vkCreateDevice", nullptr,
            [](Stream& s) {
                DeviceExtras group;
                group.groupOfTheNullPhysicalDevice = true;
                encodeCreateDevice(s, physDevId, otherDeviceId, group);
            }),
        nullHandleRow("import-semaphore-resource-of-the-null-semaphore", semaphoreFdDevice(),
            "vkr_dispatch_vkImportSemaphoreResourceMESA", nullptr,
            [](Stream& s) { encodeImportSemaphore(s, 0); }),

        // The commands that copy to and from device memory.
        nullHandleRow("copy-query-pool-results-to-memory-of-the-null-pool", addressCommandsDevice(),
            "vkr_dispatch_vkCmdCopyQueryPoolResultsToMemoryKHR", recordingCommandBuffer(),
            [](Stream& s) { encodeCopyQueryPoolResultsToMemory(s, 0); }),
        nullHandleRow("copy-memory-to-image-of-the-null-image", addressCommandsDevice(),
            "vkr_dispatch_vkCmdCopyMemoryToImageKHR", recordingCommandBuffer(),
            [](Stream& s) { encodeCopyDeviceMemoryImage(s, cmdCopyMemoryToImage, 0); }),
        nullHandleRow("copy-image-to-memory-of-the-null-image", addressCommandsDevice(),
            "vkr_dispatch_vkCmdCopyImageToMemoryKHR", recordingCommandBuffer(),
            [](Stream& s) { encodeCopyDeviceMemoryImage(s, cmdCopyImageToMemory, 0); }),

        // A shader stage that names no shader.
        stageRefusedRow("compute-stage-with-no-module", noShaderLine,
            [](Stream& s) { encodeComputePipeline(s, pipelineId, {}); }),
        stageRefusedRow("compute-stage-with-no-entry-point-name", noEntryPointLine,
            [](Stream& s) { encodeComputePipeline(s, pipelineId, {.moduleInfo = true, .name = false}); }),
        stageRefusedRow("graphics-stage-with-no-module", noShaderLine,
            [](Stream& s) { encodeGraphicsPipeline(s, pipelineId, {.vertex = true}); }),
        stageRefusedRow("graphics-stage-with-no-entry-point-name", noEntryPointLine,
            [](Stream& s) {
                encodeGraphicsPipeline(s, pipelineId, {.vertex = true, .source = {.moduleInfo = true, .name = false}});
            }),
        keyStageRefusedRow("get-the-key-of-a-compute-stage-with-no-module", noShaderLine,
            [](Stream& s) { encodeKeyOfCompute(s, {}); }),
        keyStageRefusedRow("get-the-key-of-a-compute-stage-with-no-entry-point-name", noEntryPointLine,
            [](Stream& s) { encodeKeyOfCompute(s, {.moduleInfo = true, .name = false}); }),
        keyStageRefusedRow("get-the-key-of-a-graphics-stage-with-no-module", noShaderLine,
            [](Stream& s) { encodeKeyOfGraphics(s, {.vertex = true}); }),

        // A pipeline that chains a library which is the null handle.
        stageRefusedRow("graphics-pipeline-from-the-null-library", "a pipeline library is the null handle",
            [](Stream& s) {
                encodeGraphicsPipeline(s, pipelineId, {.vertex = true, .source = {.moduleInfo = true}},
                    {.library = true});
            }),
        keyStageRefusedRow("get-the-key-of-a-pipeline-with-the-null-library", "names a null handle",
            [](Stream& s) {
                encodeKeyOfGraphics(s, {.vertex = true, .source = {.moduleInfo = true}}, {.library = true});
            }),

        // A stage with a chained module is served, and so is a pipeline made
        // from the binaries it captured, which ignore the stage's shaders.
        {"compute-stage-with-a-chained-module-is-served", [](Session& session) {
            Stream layout;
            encodePipelineLayout(layout, layoutId);
            require(session.send(layout), "the pipeline layout was refused");
            Stream s;
            encodeComputePipeline(s, pipelineId, {.moduleInfo = true}, {}, layoutId);
            requireAnswered(session.ask(s), createComputePipelines, VK_SUCCESS,
                "a stage with a chained module was refused");
            require(!rendererSaid(noShaderLine), "the renderer refused a stage that names its shader");
        }, false, setupOf(plainDevice())},
        {"compute-pipeline-from-captured-binaries-needs-no-module", [](Session& session) {
            Stream layout;
            encodePipelineLayout(layout, layoutId);
            require(session.send(layout), "the pipeline layout was refused");
            Stream capture;
            encodeCapturingComputePipeline(capture, pipelineId, layoutId);
            requireAnswered(session.ask(capture), createComputePipelines, VK_SUCCESS,
                "the pipeline that captures its binaries was refused");
            Stream binaries;
            encodeBinariesOfPipeline(binaries, pipelineId, binaryId);
            const Answer made = session.ask(binaries);
            require(made.submitted && made.command == static_cast<std::int32_t>(createPipelineBinaries) &&
                        (made.result == VK_SUCCESS || made.result == VK_INCOMPLETE),
                "the binaries of the pipeline were not made");
            Stream s;
            encodeComputePipeline(s, otherPipelineId, {}, {.binaries = true, .binary = binaryId}, layoutId);
            requireAnswered(session.ask(s), createComputePipelines, VK_SUCCESS,
                "a pipeline made from captured binaries was refused for its stage's missing module");
            require(!rendererSaid(noShaderLine), "the renderer refused a stage a binary ignores");
        }, false, setupOf(pipelineBinaryDevice())},
        {"get-the-key-of-a-compute-stage-with-a-chained-module-is-served", [](Session& session) {
            Stream s;
            encodeKeyOfCompute(s, {.moduleInfo = true});
            requireAnswered(session.ask(s), getPipelineKey, VK_SUCCESS,
                "the key of a stage with a chained module was refused");
        }, false, setupOf(pipelineBinaryDevice())},

        // A host image copy of a region the image cannot hold.
        hostCopyRefusedRow("host-copy-to-memory-past-the-last-mip-level", mipLine, true, {.mipLevel = 5}),
        hostCopyRefusedRow("host-copy-to-memory-of-an-aspect-the-image-lacks", aspectLine, true,
            {.aspect = VK_IMAGE_ASPECT_DEPTH_BIT}),
        hostCopyRefusedRow("host-copy-to-memory-of-two-aspects", aspectLine, true,
            {.aspect = VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT}),
        hostCopyRefusedRow("host-copy-to-memory-past-the-width", outsideLine, true,
            {.width = 2 * extent, .memoryBytes = 2 * extent * 4 * texelBytes}),
        hostCopyRefusedRow("host-copy-to-memory-from-a-negative-offset", outsideLine, true, {.offset = -1}),
        hostCopyRefusedRow("host-copy-to-memory-from-an-offset-past-the-edge", outsideLine, true,
            {.offset = extent - 2}),
        hostCopyRefusedRow("host-copy-to-memory-with-an-oversized-row-length", reachLine, true,
            {.rowLength = hugeRowLength}),
        hostCopyRefusedRow("host-copy-to-memory-with-an-oversized-image-height", reachLine, true,
            {.layers = 2, .imageHeight = hugeRowLength, .memoryBytes = 4 * 4 * texelBytes * 2}, 2),
        hostCopyRefusedRow("host-copy-from-memory-past-the-last-mip-level", mipLine, false, {.mipLevel = 5}),
        hostCopyRefusedRow("host-copy-from-memory-of-an-aspect-the-image-lacks", aspectLine, false,
            {.aspect = VK_IMAGE_ASPECT_STENCIL_BIT}),
        hostCopyRefusedRow("host-copy-from-memory-past-the-height", outsideLine, false,
            {.height = 2 * extent, .memoryBytes = 4 * 2 * extent * texelBytes}),
        hostCopyRefusedRow("host-copy-from-memory-with-an-oversized-row-length", reachLine, false,
            {.rowLength = hugeRowLength}),
        hostCopyRefusedRow("host-copy-from-memory-with-an-oversized-image-height", reachLine, false,
            {.layers = 2, .imageHeight = hugeRowLength, .memoryBytes = 4 * 4 * texelBytes * 2}, 2),

        // A region the image holds is copied, and what was written is read
        // back, from memory padded to a longer row as well as packed.
        {"host-copy-round-trip", [](Session& session) {
            hostCopyImageReady()(session);
            const HostCopy copy;
            const std::vector<std::uint8_t> data = patternOf(copy.memoryBytes);
            Stream write;
            encodeCopyMemoryToImage(write, imageId, copy, data);
            requireAnswered(session.ask(write), copyMemoryToImage, VK_SUCCESS, "a copy into the image was refused");
            Stream read;
            encodeCopyImageToMemory(read, imageId, copy);
            const Answer answer = session.ask(read);
            requireAnswered(answer, copyImageToMemory, VK_SUCCESS, "a copy out of the image was refused");
            // The reply: the command, the result, the size of the array, the bytes.
            require(std::equal(data.begin(), data.end(), answer.raw.begin() + 16),
                "what a copy out of the image returned is not what was copied into it");
            require(!rendererSaid("host image copy:"), "the renderer refused a region the image holds");
        }, false, setupOf(hostCopyDevice())},
        {"host-copy-from-memory-padded-to-a-longer-row", [](Session& session) {
            hostCopyImageReady()(session);
            HostCopy copy;
            copy.rowLength = 8;                         // texels, for a region 4 wide
            copy.memoryBytes = (3 * 8 + 4) * texelBytes;  // the last row ends after its 4 texels
            Stream write;
            encodeCopyMemoryToImage(write, imageId, copy, patternOf(copy.memoryBytes));
            requireAnswered(session.ask(write), copyMemoryToImage, VK_SUCCESS,
                "a copy from memory padded to a longer row was refused");
            require(!rendererSaid("host image copy:"), "the renderer refused a padded region");
        }, false, setupOf(hostCopyDevice())},
    };
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    const row_runner::Suite suite {rows(), setupOf(plainDevice()), "required handle and image copy"};
    return row_runner::main(argc, argv, suite);
}
