#include "gfx/OffscreenBuffer.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace fe {

OffscreenBuffer::~OffscreenBuffer() { shutdown(); }

bool OffscreenBuffer::init(const VulkanContext& ctx,
                           uint32_t width, uint32_t height,
                           const OffscreenBuffer* sharedMultisampled) {
    m_ctx    = &ctx;
    m_shared = sharedMultisampled;
    return create(width, height);
}

void OffscreenBuffer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    if (m_shared) {
        FE_ERROR("OffscreenBuffer: a buffer that shares multisampled images cannot be resized");
        return;
    }
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

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    // ------------------------------------------------------------------
    // 1. Colour image (rendered into, then sampled by ImGui)
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
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create colour image (%ux%u)",
                 width, height);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format();
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr,
                          &m_imageView) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create colour image view");
        return false;
    }

    // A buffer that shares the multisampled images uses the other buffer's.
    if (m_shared) {
        if (m_shared->m_width != width || m_shared->m_height != height || !m_shared->m_msaaImage ||
        !m_shared->m_hdrImage) {
            FE_ERROR("OffscreenBuffer: the shared multisampled images do not match (%ux%u)", width, height);
            return false;
        }
        m_msaaImage = m_shared->m_msaaImage;
        m_msaaView  = m_shared->m_msaaView;
        m_depthImage = m_shared->m_depthImage;
        m_depthView  = m_shared->m_depthView;
        m_hdrImage   = m_shared->m_hdrImage;
        m_hdrView    = m_shared->m_hdrView;
        m_hdrDescSet = m_shared->m_hdrDescSet;
    } else {
    // ------------------------------------------------------------------
    // 1b. HDR image: the scene pass averages its samples into it, and
    //     PostProcess reads it. Floating point, so light brighter than 1 is kept.
    // ------------------------------------------------------------------
    VkImageCreateInfo hdrInfo = imageInfo;
    hdrInfo.format = hdrFormat();
    hdrInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // ScreenReflections keeps a copy of it for the next frame

    if (vmaCreateImage(m_ctx->allocator(), &hdrInfo, &allocInfo,
                       &m_hdrImage, &m_hdrAllocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create the HDR image (%ux%u)", width, height);
        return false;
    }

    VkImageViewCreateInfo hdrViewInfo = viewInfo;
    hdrViewInfo.image  = m_hdrImage;
    hdrViewInfo.format = hdrFormat();
    if (vkCreateImageView(m_ctx->device(), &hdrViewInfo, nullptr, &m_hdrView) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create the HDR image view");
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Multisampled colour image: what the scene pass really draws into.
    //    Its contents are averaged into the image above at the end of the
    //    pass and never read again, so the GPU may keep them in tile memory.
    // ------------------------------------------------------------------
    VkImageCreateInfo msaaInfo = imageInfo;
    msaaInfo.format  = hdrFormat();
    msaaInfo.samples = kSampleCount;
    msaaInfo.usage   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                     | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    VmaAllocationCreateInfo transientAlloc{};
    transientAlloc.usage          = VMA_MEMORY_USAGE_GPU_ONLY;
    transientAlloc.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

    if (vmaCreateImage(m_ctx->allocator(), &msaaInfo, &transientAlloc,
                       &m_msaaImage, &m_msaaAllocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create the multisampled colour image (%ux%u)",
                 width, height);
        return false;
    }

    VkImageViewCreateInfo msaaViewInfo = viewInfo;
    msaaViewInfo.image  = m_msaaImage;
    msaaViewInfo.format = hdrFormat();
    if (vkCreateImageView(m_ctx->device(), &msaaViewInfo, nullptr, &m_msaaView) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create the multisampled colour view");
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Depth image (multisampled like the colour; only used during the
    //    scene pass, never sampled)
    // ------------------------------------------------------------------
    VkImageCreateInfo depthInfo = imageInfo;
    depthInfo.format  = depthFormat();
    depthInfo.samples = kSampleCount;
    depthInfo.usage   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    if (vmaCreateImage(m_ctx->allocator(), &depthInfo, &transientAlloc,
                       &m_depthImage, &m_depthAllocation,
                       nullptr) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create depth image (%ux%u)",
                 width, height);
        return false;
    }

    VkImageViewCreateInfo depthViewInfo = viewInfo;
    depthViewInfo.image                       = m_depthImage;
    depthViewInfo.format                      = depthFormat();
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

    if (vkCreateImageView(m_ctx->device(), &depthViewInfo, nullptr,
                          &m_depthView) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create depth image view");
        return false;
    }

    }   // not shared

    // ------------------------------------------------------------------
    // 4. Sampler + ImGui descriptor set (colour image only)
    // ------------------------------------------------------------------
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = 1.0f;

    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr,
                        &m_sampler) != VK_SUCCESS) {
        FE_ERROR("OffscreenBuffer: failed to create sampler");
        return false;
    }

    m_imguiDescSet = ImGui_ImplVulkan_AddTexture(
        m_sampler, m_imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!m_imguiDescSet) {
        FE_ERROR("OffscreenBuffer: ImGui_ImplVulkan_AddTexture failed");
        return false;
    }

    // The set PostProcess reads the HDR image through (borrowed if shared).
    if (!m_shared) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorSetCount = 1;

        if (vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_hdrLayout) != VK_SUCCESS ||
            vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_hdrPool) != VK_SUCCESS) {
            FE_ERROR("OffscreenBuffer: failed to create the HDR descriptor set layout or pool");
            return false;
        }
        allocInfo.descriptorPool = m_hdrPool;
        allocInfo.pSetLayouts    = &m_hdrLayout;
        if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_hdrDescSet) != VK_SUCCESS) {
            FE_ERROR("OffscreenBuffer: failed to allocate the HDR descriptor set");
            return false;
        }

        VkDescriptorImageInfo image{};
        image.sampler     = m_sampler;
        image.imageView   = m_hdrView;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_hdrDescSet;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    }

    FE_INFO("OffscreenBuffer created: %ux%u (HDR colour + depth, %ux multisampled)",
            width, height, static_cast<unsigned>(kSampleCount));
    return true;
}

void OffscreenBuffer::destroy() {
    if (!m_ctx) return;

    if (m_imguiDescSet) {
        ImGui_ImplVulkan_RemoveTexture(m_imguiDescSet);
        m_imguiDescSet = VK_NULL_HANDLE;
    }
    m_hdrDescSet = VK_NULL_HANDLE;   // freed with its pool
    if (m_hdrPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_hdrPool, nullptr);
        m_hdrPool = VK_NULL_HANDLE;
    }
    if (m_hdrLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_hdrLayout, nullptr);
        m_hdrLayout = VK_NULL_HANDLE;
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
    if (m_shared) {   // the multisampled images belong to the other buffer
        m_msaaImage = VK_NULL_HANDLE;  m_msaaView = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE; m_depthView = VK_NULL_HANDLE;
        m_hdrImage   = VK_NULL_HANDLE; m_hdrView   = VK_NULL_HANDLE;
        return;
    }
    if (m_hdrView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_hdrView, nullptr);
        m_hdrView = VK_NULL_HANDLE;
    }
    if (m_hdrImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_hdrImage, m_hdrAllocation);
        m_hdrImage      = VK_NULL_HANDLE;
        m_hdrAllocation = VK_NULL_HANDLE;
    }
    if (m_msaaView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_msaaView, nullptr);
        m_msaaView = VK_NULL_HANDLE;
    }
    if (m_msaaImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_msaaImage, m_msaaAllocation);
        m_msaaImage      = VK_NULL_HANDLE;
        m_msaaAllocation = VK_NULL_HANDLE;
    }
    if (m_depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_depthImage, m_depthAllocation);
        m_depthImage      = VK_NULL_HANDLE;
        m_depthAllocation = VK_NULL_HANDLE;
    }
}

} // namespace fe
