#include "gfx/Texture.h"

#include "core/Log.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace fe {
namespace {

/// Records an image layout transition for the given mip range.
void transition(VkCommandBuffer cmd, VkImage image,
                uint32_t baseMip, uint32_t mipCount,
                VkImageLayout oldLayout, VkImageLayout newLayout,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = baseMip;
    barrier.subresourceRange.levelCount     = mipCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.srcAccessMask                   = srcAccess;
    barrier.dstAccessMask                   = dstAccess;

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

bool Texture::create(const VulkanContext& ctx, const ImageData& image,
                     VkFormat format, bool generateMips, float maxAnisotropy) {
    destroy();

    const size_t byteCount = static_cast<size_t>(image.width) * image.height * 4;
    if (image.width == 0 || image.height == 0 || image.pixels.size() < byteCount) {
        FE_ERROR("Texture: invalid image data (%u x %u, %zu bytes)",
                 image.width, image.height, image.pixels.size());
        return false;
    }

    m_ctx    = &ctx;
    m_width  = image.width;
    m_height = image.height;

    // ---- Mip count ----
    // Levels needed to reach 1x1: floor(log2(largest side)) + 1. Generating
    // them with blits needs the format to support linear filtering as both
    // blit source and destination.
    m_mipLevels = 1;
    if (generateMips) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), format, &props);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                            VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((props.optimalTilingFeatures & needed) == needed)
            m_mipLevels = std::bit_width(std::max(m_width, m_height));
        else
            FE_WARN("Texture: format %d cannot be blitted with filtering; no mipmaps", int(format));
    }

    // ---- Staging buffer (CPU-visible) ----
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          staging           = VK_NULL_HANDLE;
    VmaAllocation     stagingAllocation = nullptr;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(ctx.allocator(), &bufferInfo, &stagingAlloc,
                        &staging, &stagingAllocation, &stagingInfo) != VK_SUCCESS) {
        FE_ERROR("Texture: cannot create the staging buffer");
        destroy();
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, image.pixels.data(), byteCount);
    vmaFlushAllocation(ctx.allocator(), stagingAllocation, 0, VK_WHOLE_SIZE);

    // Everything from here on must free the staging buffer; one lambda keeps
    // the failure paths short.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence       fence = VK_NULL_HANDLE;
    const auto fail = [&](const char* what) {
        FE_ERROR("Texture: %s", what);
        if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
        if (pool)  vkDestroyCommandPool(ctx.device(), pool, nullptr);
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAllocation);
        destroy();
        return false;
    };

    // ---- The GPU image ----
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = {m_width, m_height, 1};
    imageInfo.mipLevels     = m_mipLevels;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // SRC as well as DST: each mip level is read (blit source) to build the next.
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(ctx.allocator(), &imageInfo, &imageAlloc,
                       &m_image, &m_allocation, nullptr) != VK_SUCCESS)
        return fail("cannot create the image");

    // ---- One-time command buffer ----
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = ctx.graphicsQueueFamily();
    if (vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return fail("cannot create a command pool");

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(ctx.device(), &allocInfo, &cmd) != VK_SUCCESS)
        return fail("cannot allocate a command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // All levels: UNDEFINED -> TRANSFER_DST, so level 0 can receive the pixels.
    transition(cmd, m_image, 0, m_mipLevels,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(cmd, staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Build the chain. Before blitting level i, level i-1 must be a source
    // (TRANSFER_SRC); once it has been read, it is finished and goes to
    // SHADER_READ_ONLY. The last level is only ever written, never read.
    int32_t mipW = static_cast<int32_t>(m_width);
    int32_t mipH = static_cast<int32_t>(m_height);

    for (uint32_t level = 1; level < m_mipLevels; ++level) {
        transition(cmd, m_image, level - 1, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        const int32_t nextW = std::max(mipW / 2, 1);
        const int32_t nextH = std::max(mipH / 2, 1);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1]  = {mipW, mipH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1]  = {nextW, nextH, 1};
        vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        transition(cmd, m_image, level - 1, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        mipW = nextW;
        mipH = nextH;
    }

    // The last level (the only one, without mipmaps) is still TRANSFER_DST.
    transition(cmd, m_image, m_mipLevels - 1, 1,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(ctx.device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        return fail("cannot create a fence");

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    if (vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, fence) != VK_SUCCESS)
        return fail("cannot submit the upload");
    vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(ctx.device(), fence, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);   // frees cmd too
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAllocation);
    fence = VK_NULL_HANDLE;
    pool  = VK_NULL_HANDLE;

    // ---- View ----
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                         = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                         = m_image;
    viewInfo.viewType                      = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                        = format;
    viewInfo.subresourceRange.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount   = m_mipLevels;
    viewInfo.subresourceRange.layerCount   = 1;
    if (vkCreateImageView(ctx.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        FE_ERROR("Texture: cannot create the image view");
        destroy();
        return false;
    }

    // ---- Sampler ----
    // Linear filtering between texels and between mip levels (trilinear);
    // repeat wrapping, because most model textures tile.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod       = 0.0f;
    samplerInfo.maxLod       = static_cast<float>(m_mipLevels);
    samplerInfo.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

    if (maxAnisotropy > 1.0f) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy    = std::min(maxAnisotropy, props.limits.maxSamplerAnisotropy);
    }

    if (vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        FE_ERROR("Texture: cannot create the sampler");
        destroy();
        return false;
    }

    return true;
}

void Texture::destroy() {
    if (!m_ctx) return;

    if (m_sampler) vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
    if (m_view)    vkDestroyImageView(m_ctx->device(), m_view, nullptr);
    if (m_image)   vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);

    m_sampler    = VK_NULL_HANDLE;
    m_view       = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_width = m_height = 0;
    m_mipLevels = 1;
    m_ctx = nullptr;
}

void Texture::moveFrom(Texture& other) noexcept {
    m_ctx        = std::exchange(other.m_ctx, nullptr);
    m_image      = std::exchange(other.m_image, VK_NULL_HANDLE);
    m_allocation = std::exchange(other.m_allocation, nullptr);
    m_view       = std::exchange(other.m_view, VK_NULL_HANDLE);
    m_sampler    = std::exchange(other.m_sampler, VK_NULL_HANDLE);
    m_width      = std::exchange(other.m_width, 0u);
    m_height     = std::exchange(other.m_height, 0u);
    m_mipLevels  = std::exchange(other.m_mipLevels, 1u);
}

} // namespace fe
