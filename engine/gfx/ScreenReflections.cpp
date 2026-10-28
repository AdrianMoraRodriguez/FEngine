#include "gfx/ScreenReflections.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <algorithm>
#include <bit>

namespace fe {

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

bool ScreenReflections::createImage(Image& out, VkFormat format, VkExtent2D extent, uint32_t levels,
                                    VkImageUsageFlags usage) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = {extent.width, extent.height, 1};
    imageInfo.mipLevels     = levels;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = usage;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create an image (%ux%u)", extent.width, extent.height);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = out.image;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = levels;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create an image view");
        return false;
    }
    return true;
}

void ScreenReflections::destroyImage(Image& image) {
    if (image.view != VK_NULL_HANDLE)
        vkDestroyImageView(m_ctx->device(), image.view, nullptr);
    if (image.image != VK_NULL_HANDLE)
        vmaDestroyImage(m_ctx->allocator(), image.image, image.allocation);
    image = Image{};
}

void ScreenReflections::barrier(VkCommandBuffer cmd, VkImage image, uint32_t firstLevel, uint32_t levelCount,
                                VkImageLayout from, VkImageLayout to) {
    // Everything before waits for everything after: simple, and nothing next
    // to the passes themselves. It also orders this frame after the last one.
    VkImageMemoryBarrier2 b{};
    b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout     = from;
    b.newLayout     = to;
    b.image         = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = firstLevel;
    b.subresourceRange.levelCount   = levelCount;
    b.subresourceRange.layerCount   = 1;

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool ScreenReflections::createEmptyImages() {
    constexpr VkExtent2D one{1, 1};
    if (!createImage(m_emptyDepth,  VK_FORMAT_R32_SFLOAT, one, 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !createImage(m_emptyColour, kFormat, one, 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;

    // Fill them once and leave them readable, with a command buffer of their
    // own: this runs at start-up, not in the middle of a frame.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create a command pool");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(m_ctx->device(), &allocInfo, &cmd) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue far{};
        far.float32[0] = far.float32[1] = far.float32[2] = far.float32[3] = 1.0f;
        VkClearColorValue black{};

        barrier(cmd, m_emptyDepth.image,  0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier(cmd, m_emptyColour.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, m_emptyDepth.image,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far,   1, &range);
        vkCmdClearColorImage(cmd, m_emptyColour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(cmd, m_emptyDepth.image,  0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, m_emptyColour.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        ok = vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) vkQueueWaitIdle(m_ctx->graphicsQueue());
    }
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);   // frees cmd too

    if (!ok) FE_ERROR("ScreenReflections: failed to fill the empty images");
    return ok;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool ScreenReflections::init(const VulkanContext& ctx) {
    m_ctx = &ctx;
    VkDevice device = ctx.device();

    // The depth is read exactly (nearest), the picture smoothly (linear, between its levels too).
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = static_cast<float>(kMaxLevels);

    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_nearest) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create the depth sampler");
        shutdown();
        return false;
    }
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_linear) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create the picture sampler");
        shutdown();
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create the descriptor set layout");
        shutdown();
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to create the descriptor pool");
        shutdown();
        return false;
    }

    const VkDescriptorSetLayout layouts[2] = {m_layout, m_layout};
    VkDescriptorSet sets[2] = {};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        FE_ERROR("ScreenReflections: failed to allocate the descriptor sets");
        shutdown();
        return false;
    }
    m_liveSet = sets[0];
    m_emptySet = sets[1];

    if (!createEmptyImages()) { shutdown(); return false; }

    // The empty set never changes.
    const VkDescriptorImageInfo depth {m_nearest, m_emptyDepth.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo colour{m_linear,  m_emptyColour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_emptySet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = (i == 0) ? &depth : &colour;
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // The live set points at the empty images until the first resize(), so it
    // is always complete.
    writes[0].dstSet = m_liveSet;
    writes[1].dstSet = m_liveSet;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    return true;
}

void ScreenReflections::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    vkDeviceWaitIdle(device);

    destroyImage(m_history);
    destroyImage(m_emptyDepth);
    destroyImage(m_emptyColour);
    if (m_pool)    vkDestroyDescriptorPool(device, m_pool, nullptr);   // frees the sets
    if (m_layout)  vkDestroyDescriptorSetLayout(device, m_layout, nullptr);
    if (m_nearest) vkDestroySampler(device, m_nearest, nullptr);
    if (m_linear)  vkDestroySampler(device, m_linear, nullptr);

    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_nearest = m_linear = VK_NULL_HANDLE;
    m_liveSet = m_emptySet = VK_NULL_HANDLE;
    m_extent = {0, 0};
    m_levels = 0;
    m_valid = false;
    m_depthView = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void ScreenReflections::writeLiveSet(VkImageView depthView) {
    const VkDescriptorImageInfo depth {m_nearest, depthView,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo colour{m_linear,  m_history.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_liveSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = (i == 0) ? &depth : &colour;
    }
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
    m_depthView = depthView;
}

bool ScreenReflections::resize(VkExtent2D extent, VkImageView depthView) {
    if (!m_ctx || extent.width == 0 || extent.height == 0 || depthView == VK_NULL_HANDLE) return false;

    const bool sameSize = (extent.width == m_extent.width && extent.height == m_extent.height && m_history.image);
    if (sameSize && depthView == m_depthView) return true;

    // The sets may be in use by frames in flight, and so may the old images.
    vkDeviceWaitIdle(m_ctx->device());

    if (!sameSize) {
        destroyImage(m_history);
        m_extent = {0, 0};
        m_valid  = false;

        // Down to a small level: past that a reflection is a flat colour anyway.
        const uint32_t smallest = std::min(extent.width, extent.height);
        m_levels = std::clamp<uint32_t>(static_cast<uint32_t>(std::bit_width(smallest)) - 2u, 1u, kMaxLevels);

        if (!createImage(m_history, kFormat, extent, m_levels,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            destroyImage(m_history);
            m_levels = 0;
            return false;
        }
        m_extent = extent;
    }

    writeLiveSet(depthView);
    return true;
}

void ScreenReflections::capture(VkCommandBuffer cmd, VkImage hdrImage, VkExtent2D extent) {
    if (!m_history.image || extent.width != m_extent.width || extent.height != m_extent.height) return;

    // The previous contents are not needed: UNDEFINED discards them.
    barrier(cmd, hdrImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, m_history.image, 0, m_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const auto blit = [&](VkImage src, uint32_t srcLevel, VkExtent2D srcSize,
                          VkImage dst, uint32_t dstLevel, VkExtent2D dstSize) {
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, srcLevel, 0, 1};
        region.srcOffsets[1]  = {static_cast<int32_t>(srcSize.width), static_cast<int32_t>(srcSize.height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dstLevel, 0, 1};
        region.dstOffsets[1]  = {static_cast<int32_t>(dstSize.width), static_cast<int32_t>(dstSize.height), 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_LINEAR);
    };

    // Level 0 is the picture; each level after it is the last one at half size.
    blit(hdrImage, 0, extent, m_history.image, 0, extent);
    VkExtent2D size = extent;
    for (uint32_t level = 1; level < m_levels; ++level) {
        const VkExtent2D next{std::max(1u, size.width / 2), std::max(1u, size.height / 2)};
        barrier(cmd, m_history.image, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        blit(m_history.image, level - 1, size, m_history.image, level, next);
        size = next;
    }

    // Levels 0 .. n-2 were turned into sources on the way; the last is still a destination.
    if (m_levels > 1)
        barrier(cmd, m_history.image, 0, m_levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd, m_history.image, m_levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    barrier(cmd, hdrImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_valid = true;
}

} // namespace fe
