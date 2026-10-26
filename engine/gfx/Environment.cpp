#include "gfx/Environment.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <cstring>

namespace fe {

VkDescriptorSetLayout Environment::createLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 2;
    info.pBindings    = bindings;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        FE_ERROR("Environment: cannot create the descriptor set layout");
    return layout;
}

bool Environment::init(const VulkanContext& ctx, VkDescriptorSetLayout layout) {
    shutdown();
    m_ctx = &ctx;

    // ---- Descriptor pool and set ----
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot create the descriptor pool");
        shutdown();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;
    if (vkAllocateDescriptorSets(ctx.device(), &allocInfo, &m_set) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot allocate the descriptor set");
        shutdown();
        return false;
    }

    // ---- Sampler: smooth between texels and between mip levels ----
    // Clamp-to-edge, because a cube map has no border: Vulkan filters across
    // the seams between faces on its own.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod       = 0.0f;
    samplerInfo.maxLod       = VK_LOD_CLAMP_NONE;
    samplerInfo.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot create the sampler");
        shutdown();
        return false;
    }

    // ---- Uniform buffer for the diffuse coefficients ----
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = sizeof(glm::vec4) * 9;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocResult{};
    if (vmaCreateBuffer(ctx.allocator(), &bufferInfo, &allocCreate,
                        &m_uniform, &m_uniformAllocation, &allocResult) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot create the uniform buffer");
        shutdown();
        return false;
    }
    m_uniformMapped = allocResult.pMappedData;
    std::memset(m_uniformMapped, 0, sizeof(glm::vec4) * 9);
    vmaFlushAllocation(ctx.allocator(), m_uniformAllocation, 0, VK_WHOLE_SIZE);
    return true;
}

void Environment::destroyImage() {
    if (!m_ctx) return;
    if (m_view)  vkDestroyImageView(m_ctx->device(), m_view, nullptr);
    if (m_image) vmaDestroyImage(m_ctx->allocator(), m_image, m_imageAllocation);
    m_view  = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_imageAllocation = nullptr;
}

void Environment::shutdown() {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());

    destroyImage();
    if (m_uniform) vmaDestroyBuffer(m_ctx->allocator(), m_uniform, m_uniformAllocation);
    if (m_sampler) vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
    if (m_pool)    vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);   // frees the set too

    m_uniform = VK_NULL_HANDLE;
    m_uniformAllocation = nullptr;
    m_uniformMapped = nullptr;
    m_sampler = VK_NULL_HANDLE;
    m_pool    = VK_NULL_HANDLE;
    m_set     = VK_NULL_HANDLE;
    m_ctx     = nullptr;
}

bool Environment::upload(const EnvironmentBake& bake) {
    if (!m_ctx || !bake.valid()) return false;
    const VulkanContext& ctx = *m_ctx;

    // No frame may be using the old image or the set while they are replaced.
    vkDeviceWaitIdle(ctx.device());
    destroyImage();

    // ---- The cube image: 6 layers, one mip chain each ----
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent        = {bake.faceSize, bake.faceSize, 1};
    imageInfo.mipLevels     = bake.mipLevels;
    imageInfo.arrayLayers   = 6;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(ctx.allocator(), &imageInfo, &imageAlloc,
                       &m_image, &m_imageAllocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot create the cube image");
        m_image = VK_NULL_HANDLE;
        return false;
    }

    // ---- Staging buffer with every level, in the order of the bake ----
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(bake.pixels.size()) * sizeof(uint16_t);

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
        FE_ERROR("Environment: cannot create the staging buffer");
        destroyImage();
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, bake.pixels.data(), static_cast<size_t>(byteCount));
    vmaFlushAllocation(ctx.allocator(), stagingAllocation, 0, VK_WHOLE_SIZE);

    VkCommandPool pool  = VK_NULL_HANDLE;
    VkFence       fence = VK_NULL_HANDLE;
    const auto fail = [&](const char* what) {
        FE_ERROR("Environment: %s", what);
        if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
        if (pool)  vkDestroyCommandPool(ctx.device(), pool, nullptr);
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAllocation);
        destroyImage();
        return false;
    };

    // ---- One-time command buffer: copy every face of every level ----
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = ctx.graphicsQueueFamily();
    if (vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return fail("cannot create a command pool");

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool        = pool;
    cmdInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(ctx.device(), &cmdInfo, &cmd) != VK_SUCCESS)
        return fail("cannot allocate a command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, bake.mipLevels, 0, 6};

    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (uint32_t level = 0; level < bake.mipLevels; ++level) {
        const uint32_t size = bake.faceSizeAt(level);
        const VkDeviceSize faceBytes = static_cast<VkDeviceSize>(size) * size * 4 * sizeof(uint16_t);
        for (uint32_t face = 0; face < 6; ++face) {
            VkBufferImageCopy region{};
            region.bufferOffset      = offset;
            region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
            region.imageExtent       = {size, size, 1};
            regions.push_back(region);
            offset += faceBytes;
        }
    }
    vkCmdCopyBufferToImage(cmd, staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
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
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAllocation);
    fence = VK_NULL_HANDLE;
    pool  = VK_NULL_HANDLE;

    // ---- View ----
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = m_image;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format           = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, bake.mipLevels, 0, 6};
    if (vkCreateImageView(ctx.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        FE_ERROR("Environment: cannot create the image view");
        destroyImage();
        return false;
    }

    // ---- Diffuse coefficients and descriptors (the GPU is idle) ----
    std::memcpy(m_uniformMapped, bake.sh.data(), sizeof(glm::vec4) * 9);
    vmaFlushAllocation(ctx.allocator(), m_uniformAllocation, 0, VK_WHOLE_SIZE);

    VkDescriptorImageInfo imageDescriptor{m_sampler, m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bufferDescriptor{m_uniform, 0, sizeof(glm::vec4) * 9};

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo      = &imageDescriptor;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo     = &bufferDescriptor;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
    return true;
}

} // namespace fe
