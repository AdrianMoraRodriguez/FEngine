#include "gfx/ShadowCube.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

namespace fe {

ShadowCube::~ShadowCube() { shutdown(); }

bool ShadowCube::init(const VulkanContext& ctx) {
    m_ctx = &ctx;

    // ---- The image: six layers, flagged as usable as a cube ----
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = kFormat;
    imageInfo.extent        = {kResolution, kResolution, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = kFaces;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(ctx.allocator(), &imageInfo, &allocInfo,
                       &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("ShadowCube: failed to create the depth image");
        shutdown();
        return false;
    }

    // ---- Views: one cube view to read, six 2D views to draw ----
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = m_image;
    viewInfo.format                      = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;

    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.subresourceRange.layerCount = kFaces;
    if (vkCreateImageView(ctx.device(), &viewInfo, nullptr, &m_cubeView) != VK_SUCCESS) {
        FE_ERROR("ShadowCube: failed to create the cube view");
        shutdown();
        return false;
    }

    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    for (uint32_t face = 0; face < kFaces; ++face) {
        viewInfo.subresourceRange.baseArrayLayer = face;
        if (vkCreateImageView(ctx.device(), &viewInfo, nullptr, &m_faceViews[face]) != VK_SUCCESS) {
            FE_ERROR("ShadowCube: failed to create the view of face %u", face);
            shutdown();
            return false;
        }
    }

    // ---- The sampler: same comparison sampler as ShadowMap ----
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter     = VK_FILTER_LINEAR;
    samplerInfo.minFilter     = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.maxLod        = 0.0f;

    if (vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        FE_ERROR("ShadowCube: failed to create the sampler");
        shutdown();
        return false;
    }

    // The same image read as plain depth values, for the shadow filter's
    // search for blockers. Nearest filtering: depths must not be blended.
    VkSamplerCreateInfo depthSamplerInfo{};
    depthSamplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    depthSamplerInfo.magFilter    = VK_FILTER_NEAREST;
    depthSamplerInfo.minFilter    = VK_FILTER_NEAREST;
    depthSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    depthSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerInfo.maxLod       = 0.0f;

    if (vkCreateSampler(ctx.device(), &depthSamplerInfo, nullptr, &m_depthSampler) != VK_SUCCESS) {
        FE_ERROR("ShadowCube: failed to create the depth sampler");
        shutdown();
        return false;
    }

    FE_INFO("ShadowCube: created (6 x %ux%u)", kResolution, kResolution);
    return true;
}

void ShadowCube::shutdown() {
    if (!m_ctx) return;

    if (m_depthSampler != VK_NULL_HANDLE)
        vkDestroySampler(m_ctx->device(), m_depthSampler, nullptr);
    if (m_sampler != VK_NULL_HANDLE)
        vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
    for (VkImageView& v : m_faceViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), v, nullptr);
        v = VK_NULL_HANDLE;
    }
    if (m_cubeView != VK_NULL_HANDLE)
        vkDestroyImageView(m_ctx->device(), m_cubeView, nullptr);
    if (m_image != VK_NULL_HANDLE)
        vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);

    m_sampler    = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_cubeView   = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_ctx        = nullptr;
}

void ShadowCube::barrier(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask  = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout     = from;
    b.newLayout     = to;
    b.image         = m_image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = kFaces;   // all six faces at once

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void ShadowCube::begin(VkCommandBuffer cmd) {
    // Same reasoning as ShadowMap::begin(): the old contents are not needed,
    // but the previous frame's reads must finish before the new writes.
    barrier(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void ShadowCube::beginFace(VkCommandBuffer cmd, uint32_t face) {
    VkRenderingAttachmentInfo depth{};
    depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView   = m_faceViews[face];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType             = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {kResolution, kResolution};
    rendering.layerCount        = 1;
    rendering.pDepthAttachment  = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(kResolution);
    viewport.height   = static_cast<float>(kResolution);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {kResolution, kResolution};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void ShadowCube::endFace(VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);
}

void ShadowCube::end(VkCommandBuffer cmd) {
    barrier(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace fe
