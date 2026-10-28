#include "gfx/SdfAtlas.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <vector>

namespace fe {
namespace {

/// Distance stored where no mesh is (far from everything).
constexpr float kEmpty = 1000.0f;

/// Runs @p record in a one-time command buffer and waits for it.
template <typename Record>
void submitOnce(const VulkanContext& ctx, Record record) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = ctx.graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool        = pool;
    cmdInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.device(), &cmdInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(ctx.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.graphicsQueue());

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = srcStage;
    barrier.srcAccessMask       = srcAccess;
    barrier.dstStageMask        = dstStage;
    barrier.dstAccessMask       = dstAccess;
    barrier.oldLayout           = from;
    barrier.newLayout           = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

bool SdfAtlas::init(const VulkanContext& ctx, uint32_t brick) {
    shutdown();
    m_ctx   = &ctx;
    m_brick = brick;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_3D;
    imageInfo.format        = VK_FORMAT_R16_SFLOAT;
    imageInfo.extent        = {m_brick, m_brick, kDepth};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(ctx.allocator(), &imageInfo, &allocInfo, &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("SdfAtlas: failed to create the %ux%ux%u image", m_brick, m_brick, kDepth);
        shutdown();
        return false;
    }

    // Everything starts as "far from everything".
    submitOnce(ctx, [this](VkCommandBuffer cmd) {
        transition(cmd, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkClearColorValue empty{{kEmpty, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &empty, 1, &range);

        transition(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = m_image;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format           = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        FE_ERROR("SdfAtlas: failed to create the image view");
        shutdown();
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        FE_ERROR("SdfAtlas: failed to create the sampler");
        shutdown();
        return false;
    }

    FE_INFO("SdfAtlas: created (%ux%ux%u, %.1f MB)", m_brick, m_brick, kDepth,
            static_cast<double>(m_brick) * m_brick * kDepth * 2.0 / (1024.0 * 1024.0));
    return true;
}

void SdfAtlas::shutdown() {
    if (!m_ctx) return;

    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
    if (m_view    != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), m_view, nullptr);
    if (m_image   != VK_NULL_HANDLE) vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);

    m_sampler    = VK_NULL_HANDLE;
    m_view       = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_nextZ      = 0;
    m_ctx        = nullptr;
}

uint32_t SdfAtlas::add(const SdfVolume& volume) {
    if (!m_ctx || !volume.valid()) return kNoSlot;

    if (volume.dims.x > m_brick || volume.dims.y > m_brick) {
        FE_WARN("SdfAtlas: a %ux%ux%u field does not fit a %ux%u brick", volume.dims.x, volume.dims.y,
                volume.dims.z, m_brick, m_brick);
        return kNoSlot;
    }
    if (m_nextZ + volume.dims.z > kDepth) {
        FE_WARN("SdfAtlas: full (%u of %u cells of depth used)", m_nextZ, kDepth);
        return kNoSlot;
    }

    // ---- Staging buffer with the field as halves ----
    const VkDeviceSize bytes = volume.distances.size() * sizeof(uint16_t);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = bytes;
    bufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingInfo{};
    stagingInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          staging      = VK_NULL_HANDLE;
    VmaAllocation     stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &stagingInfo, &staging, &stagingAlloc, &mapped) != VK_SUCCESS) {
        FE_ERROR("SdfAtlas: failed to create the staging buffer");
        return kNoSlot;
    }

    uint16_t* halves = static_cast<uint16_t*>(mapped.pMappedData);
    for (size_t i = 0; i < volume.distances.size(); ++i) halves[i] = sdfFloatToHalf(volume.distances[i]);
    vmaFlushAllocation(m_ctx->allocator(), stagingAlloc, 0, VK_WHOLE_SIZE);

    const uint32_t z = m_nextZ;

    submitOnce(*m_ctx, [&](VkCommandBuffer cmd) {
        transition(cmd, m_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset      = {0, 0, static_cast<int32_t>(z)};
        region.imageExtent      = {volume.dims.x, volume.dims.y, volume.dims.z};
        vkCmdCopyBufferToImage(cmd, staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        transition(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    m_nextZ += volume.dims.z;
    return z;
}

} // namespace fe
