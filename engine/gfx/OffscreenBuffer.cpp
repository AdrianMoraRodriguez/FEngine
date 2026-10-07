#include "gfx/OffscreenBuffer.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace fe {

OffscreenBuffer::~OffscreenBuffer() { shutdown(); }

bool OffscreenBuffer::init(const VulkanContext& ctx,
                           uint32_t width, uint32_t height) {
    m_ctx = &ctx;
    return create(width, height);
}

void OffscreenBuffer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroy();
    create(width, height);
}

void OffscreenBuffer::shutdown() {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroy();
    m_ctx = nullptr;
}

bool OffscreenBuffer::create(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;

    // ------------------------------------------------------------------
    // 1. Image
    // ------------------------------------------------------------------
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format();
    imageInfo.extent        = {width, height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // COLOR_ATTACHMENT: rendered into. SAMPLED: read by ImGui shader.
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    // GPU_ONLY: fastest memory, not CPU-accessible. Appropriate for a
    // render target that the CPU never reads back.
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create image (%ux%u)", width, height);
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Image view
    // ------------------------------------------------------------------
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format();
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr,
                          &m_imageView) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create image view");
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Sampler
    // ------------------------------------------------------------------
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType      = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter  = VK_FILTER_LINEAR;
    samplerInfo.minFilter  = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = 1.0f;

    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr,
                        &m_sampler) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create sampler");
        return false;
    }

    // ------------------------------------------------------------------
    // 4. ImGui descriptor set
    //
    // ImGui_ImplVulkan_AddTexture creates a VkDescriptorSet that binds
    // the sampler + image view. Passing this set to ImGui::Image() tells
    // ImGui to sample our offscreen image when drawing the Viewport panel.
    // ------------------------------------------------------------------
    m_imguiDescSet = ImGui_ImplVulkan_AddTexture(
        m_sampler,
        m_imageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!m_imguiDescSet) {
        FE_ERROR("OffscreenBuffer: ImGui_ImplVulkan_AddTexture failed");
        return false;
    }

    FE_INFO("OffscreenBuffer created: %ux%u", width, height);
    return true;
}

void OffscreenBuffer::destroy() {
    if (!m_ctx) return;

    if (m_imguiDescSet) {
        ImGui_ImplVulkan_RemoveTexture(m_imguiDescSet);
        m_imguiDescSet = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);
        m_image      = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }
}

} // namespace fe
