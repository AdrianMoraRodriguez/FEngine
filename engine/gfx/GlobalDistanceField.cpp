#include "gfx/GlobalDistanceField.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/Renderer.h"
#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace fe {
namespace {

static_assert(GlobalDistanceField::kBaseResolution % 4 == 0, "the compute shader works in groups of 4 cells");

/// What sdf_global.comp reads as its push constants.
struct ComputePush {
    glm::vec4  origin;   ///< xyz: corner of the level. w: cell size.
    glm::uvec4 counts;   ///< x: objects. y: cells per side. z: the level.
    glm::vec4  range;    ///< x: the farthest distance stored. y: how much surfaces are inflated.
};

/// What sdf_debug.frag reads as its push constants.
struct DebugPush {
    glm::mat4 inverseViewProj;
    glm::vec4 origin[GlobalDistanceField::kLevels];   ///< Per level. xyz: corner. w: cell size.
    glm::vec4 params;                                 ///< x: cells per side.
};
static_assert(sizeof(DebugPush) <= 128, "push constants are only guaranteed up to 128 bytes");

/// The corner of the volume moves in steps of this many cells, so that a
/// moving camera does not rebuild it every frame.
constexpr float kSnapCells = 4.0f;

uint64_t fnv(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

} // namespace

VkShaderModule GlobalDistanceField::loadShader(const char* path) const {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("GlobalDistanceField: cannot open shader file '%s'", path);
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &info, nullptr, &module) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create shader module '%s'", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

bool GlobalDistanceField::init(const VulkanContext& ctx, const char* shaderDir, const SdfAtlas& atlas,
                               VkFormat colorFormat, VkFormat depthFormat) {
    shutdown();
    m_ctx   = &ctx;
    m_atlas = &atlas;

    // Without the volume and its descriptors nothing can sample the field, and
    // the mesh pipeline needs their layout: that is a failure.
    if (!createVolume() || !createColourVolume() || !createDescriptors(atlas)) {
        shutdown();
        return false;
    }

    // Without the shaders the volume is never built (ready() stays false) but
    // the descriptors stay valid, so the mesh pipeline can still be created
    // and simply has no distance field to trace.
    if (!createComputePipeline(shaderDir) || !createDebugPipeline(shaderDir, colorFormat, depthFormat))
        return false;

    FE_INFO("GlobalDistanceField: created (%u levels of %u^3 cells: %.3f, %.3f, %.3f m)",
            kLevels, m_resolution, cellSize(0), cellSize(1), cellSize(2));
    return true;
}

bool GlobalDistanceField::createVolume() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_3D;
    imageInfo.format        = VK_FORMAT_R16_SFLOAT;
    imageInfo.extent        = {m_resolution, m_resolution, m_resolution * kLevels};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the volume");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = m_image;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format           = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the volume's view");
        return false;
    }

    if (m_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            FE_ERROR("GlobalDistanceField: failed to create the volume's sampler");
            return false;
        }
    }

    // The volume stays in the general layout for good (written and sampled).
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool        = pool;
    cmdInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_ctx->device(), &cmdInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx->graphicsQueue());
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);

    // The objects: host-visible, one copy per frame in flight. Created once:
    // when the volumes are recreated the buffers and the sets that read them stay.
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (m_instanceBuffer[i] != VK_NULL_HANDLE) continue;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size        = sizeof(SdfInstance) * kMaxInstances;
        bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo bufferAlloc{};
        bufferAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        bufferAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &bufferAlloc, &m_instanceBuffer[i],
                            &m_instanceAlloc[i], &mapped) != VK_SUCCESS) {
            FE_ERROR("GlobalDistanceField: failed to create the object buffer");
            return false;
        }
        m_instanceMapped[i] = mapped.pMappedData;
    }

    if (m_instanceGpu == VK_NULL_HANDLE) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size        = sizeof(SdfInstance) * kMaxInstances;
        bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo bufferAlloc{};
        bufferAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &bufferAlloc, &m_instanceGpu,
                            &m_instanceGpuAlloc, nullptr) != VK_SUCCESS) {
            FE_ERROR("GlobalDistanceField: failed to create the shader's object buffer");
            return false;
        }
    }
    return true;
}

void GlobalDistanceField::destroyVolumes() {
    VkDevice device = m_ctx->device();
    if (m_view)         vkDestroyImageView(device, m_view, nullptr);
    if (m_image)        vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);
    if (m_colourView)   vkDestroyImageView(device, m_colourView, nullptr);
    if (m_colourImage)  vmaDestroyImage(m_ctx->allocator(), m_colourImage, m_colourAllocation);
    m_view = VK_NULL_HANDLE; m_image = VK_NULL_HANDLE; m_allocation = VK_NULL_HANDLE;
    m_colourView = VK_NULL_HANDLE; m_colourImage = VK_NULL_HANDLE; m_colourAllocation = VK_NULL_HANDLE;
}

void GlobalDistanceField::updateVolumeDescriptors() {
    VkDescriptorImageInfo volumeStorage{VK_NULL_HANDLE, m_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo colourStorage{VK_NULL_HANDLE, m_colourView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo volumeSampled{m_sampler, m_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo colourSampled{m_sampler, m_colourView, VK_IMAGE_LAYOUT_GENERAL};

    std::vector<VkWriteDescriptorSet> writes;
    auto add = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.descriptorCount = 1;
        write.descriptorType  = type;
        write.pImageInfo      = info;
        writes.push_back(write);
    };
    for (uint32_t i = 0; i < kFrames; ++i) {
        add(m_computeSet[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &volumeStorage);
        add(m_computeSet[i], 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &colourStorage);
    }
    add(m_sampleSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &volumeSampled);
    add(m_sampleSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colourSampled);
    vkUpdateDescriptorSets(m_ctx->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool GlobalDistanceField::setResolution(uint32_t cells) {
    cells = std::clamp((cells / 4) * 4, 64u, 256u);   // the compute shader works in groups of 4
    if (!m_ctx || cells == m_resolution) return true;

    // Nothing may still be using the old volumes or the sets that point at them.
    vkDeviceWaitIdle(m_ctx->device());
    destroyVolumes();
    m_resolution = cells;
    for (uint32_t l = 0; l < kLevels; ++l) m_levelBuilt[l] = false;
    m_built = false;

    if (!createVolume() || !createColourVolume()) {
        FE_ERROR("GlobalDistanceField: could not recreate the volumes at %u cells", cells);
        destroyVolumes();
        return false;
    }
    updateVolumeDescriptors();
    FE_INFO("GlobalDistanceField: %u^3 cells per level (%.3f, %.3f, %.3f m)",
            m_resolution, cellSize(0), cellSize(1), cellSize(2));
    return true;
}

bool GlobalDistanceField::createColourVolume() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_3D;
    imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent        = {m_resolution, m_resolution, m_resolution * kLevels};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_colourImage, &m_colourAllocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the colour volume");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = m_colourImage;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_colourView) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the colour volume's view");
        return false;
    }

    // Like the distances, it stays in the general layout for good.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool        = pool;
    cmdInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_ctx->device(), &cmdInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_colourImage;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx->graphicsQueue());
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
    return true;
}

bool GlobalDistanceField::createDescriptors(const SdfAtlas& atlas) {
    VkDevice device = m_ctx->device();

    // ---- Layouts ----
    VkDescriptorSetLayoutBinding computeBindings[4]{};
    computeBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo computeInfo{};
    computeInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    computeInfo.bindingCount = 4;
    computeInfo.pBindings    = computeBindings;
    if (vkCreateDescriptorSetLayout(device, &computeInfo, nullptr, &m_computeLayout) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the compute set layout");
        return false;
    }

    VkDescriptorSetLayoutBinding sampleBindings[4]{};
    for (uint32_t b = 0; b < 3; ++b) {   // the two volumes and the atlas
        sampleBindings[b].binding         = b;
        sampleBindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampleBindings[b].descriptorCount = 1;
        sampleBindings[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    sampleBindings[3].binding         = 3;   // the objects
    sampleBindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sampleBindings[3].descriptorCount = 1;
    sampleBindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sampleInfo{};
    sampleInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sampleInfo.bindingCount = 4;
    sampleInfo.pBindings    = sampleBindings;
    if (vkCreateDescriptorSetLayout(device, &sampleInfo, nullptr, &m_sampleLayout) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the sample set layout");
        return false;
    }

    // ---- Pool and sets ----
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          2 * kFrames},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames + 3},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kFrames + 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = kFrames + 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the descriptor pool");
        return false;
    }

    VkDescriptorSetLayout layouts[kFrames + 1];
    for (uint32_t i = 0; i < kFrames; ++i) layouts[i] = m_computeLayout;
    layouts[kFrames] = m_sampleLayout;

    VkDescriptorSet sets[kFrames + 1]{};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = kFrames + 1;
    allocInfo.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to allocate the descriptor sets");
        return false;
    }
    for (uint32_t i = 0; i < kFrames; ++i) m_computeSet[i] = sets[i];
    m_sampleSet = sets[kFrames];

    // ---- What they point at ----
    VkDescriptorImageInfo volumeStorage{VK_NULL_HANDLE, m_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo atlasImage{atlas.sampler(), atlas.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo volumeSampled{m_sampler, m_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo colourStorage{VK_NULL_HANDLE, m_colourView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo colourSampled{m_sampler, m_colourView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buffers[kFrames]{};

    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < kFrames; ++i) {
        buffers[i] = {m_instanceBuffer[i], 0, sizeof(SdfInstance) * kMaxInstances};

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_computeSet[i];
        write.descriptorCount = 1;

        write.dstBinding = 0; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          write.pImageInfo  = &volumeStorage; writes.push_back(write);
        write.dstBinding = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo  = &atlasImage;     writes.push_back(write);
        write.dstBinding = 2; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         write.pImageInfo  = nullptr;
                                                                                                 write.pBufferInfo = &buffers[i];    writes.push_back(write);
        write.dstBinding = 3; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          write.pBufferInfo = nullptr;
                                                                                                 write.pImageInfo  = &colourStorage; writes.push_back(write);
    }
    VkWriteDescriptorSet sampleWrite{};
    sampleWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    sampleWrite.dstSet          = m_sampleSet;
    sampleWrite.dstBinding      = 0;
    sampleWrite.descriptorCount = 1;
    sampleWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampleWrite.pImageInfo      = &volumeSampled;
    writes.push_back(sampleWrite);
    sampleWrite.dstBinding = 1;
    sampleWrite.pImageInfo = &colourSampled;
    writes.push_back(sampleWrite);
    sampleWrite.dstBinding = 2;
    sampleWrite.pImageInfo = &atlasImage;
    writes.push_back(sampleWrite);

    VkDescriptorBufferInfo gpuBuffer{m_instanceGpu, 0, sizeof(SdfInstance) * kMaxInstances};
    sampleWrite.dstBinding      = 3;
    sampleWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sampleWrite.pImageInfo      = nullptr;
    sampleWrite.pBufferInfo     = &gpuBuffer;
    writes.push_back(sampleWrite);

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool GlobalDistanceField::createComputePipeline(const char* shaderDir) {
    VkShaderModule shader = loadShader((std::string(shaderDir) + "/sdf_global.comp.spv").c_str());
    if (!shader) return false;

    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_computeLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_computeLayoutPipeline) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the compute pipeline layout");
        vkDestroyShaderModule(m_ctx->device(), shader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName  = "main";
    info.layout       = m_computeLayoutPipeline;

    const VkResult result = vkCreateComputePipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &info, nullptr, &m_computePipeline);
    vkDestroyShaderModule(m_ctx->device(), shader, nullptr);
    if (result != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the compute pipeline");
        return false;
    }
    return true;
}

bool GlobalDistanceField::createDebugPipeline(const char* shaderDir, VkFormat colorFormat, VkFormat depthFormat) {
    const std::string dir = shaderDir;
    VkShaderModule vert = loadShader((dir + "/sky.vert.spv").c_str());     // the same full-screen triangle
    VkShaderModule frag = loadShader((dir + "/sdf_debug.frag.spv").c_str());
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode    = VK_CULL_MODE_NONE;
    rasteriser.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = OffscreenBuffer::kSampleCount;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DebugPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_sampleLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_debugLayout) != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the debug pipeline layout");
        vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
        return false;
    }

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasteriser;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &blending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_debugLayout;

    const VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_debugPipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    if (result != VK_SUCCESS) {
        FE_ERROR("GlobalDistanceField: failed to create the debug pipeline");
        return false;
    }
    return true;
}

void GlobalDistanceField::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    vkDeviceWaitIdle(device);

    if (m_debugPipeline)          vkDestroyPipeline(device, m_debugPipeline, nullptr);
    if (m_debugLayout)            vkDestroyPipelineLayout(device, m_debugLayout, nullptr);
    if (m_computePipeline)        vkDestroyPipeline(device, m_computePipeline, nullptr);
    if (m_computeLayoutPipeline)  vkDestroyPipelineLayout(device, m_computeLayoutPipeline, nullptr);
    if (m_pool)                   vkDestroyDescriptorPool(device, m_pool, nullptr);
    if (m_computeLayout)          vkDestroyDescriptorSetLayout(device, m_computeLayout, nullptr);
    if (m_sampleLayout)           vkDestroyDescriptorSetLayout(device, m_sampleLayout, nullptr);
    for (uint32_t i = 0; i < kFrames; ++i)
        if (m_instanceBuffer[i])  vmaDestroyBuffer(m_ctx->allocator(), m_instanceBuffer[i], m_instanceAlloc[i]);
    if (m_instanceGpu)            vmaDestroyBuffer(m_ctx->allocator(), m_instanceGpu, m_instanceGpuAlloc);
    if (m_sampler)                vkDestroySampler(device, m_sampler, nullptr);
    if (m_view)                   vkDestroyImageView(device, m_view, nullptr);
    if (m_image)                  vmaDestroyImage(m_ctx->allocator(), m_image, m_allocation);
    if (m_colourView)             vkDestroyImageView(device, m_colourView, nullptr);
    if (m_colourImage)            vmaDestroyImage(m_ctx->allocator(), m_colourImage, m_colourAllocation);

    m_debugPipeline = VK_NULL_HANDLE; m_debugLayout = VK_NULL_HANDLE;
    m_computePipeline = VK_NULL_HANDLE; m_computeLayoutPipeline = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE; m_computeLayout = VK_NULL_HANDLE; m_sampleLayout = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kFrames; ++i) {
        m_instanceBuffer[i] = VK_NULL_HANDLE; m_instanceAlloc[i] = VK_NULL_HANDLE;
        m_instanceMapped[i] = nullptr; m_computeSet[i] = VK_NULL_HANDLE;
    }
    m_sampleSet = VK_NULL_HANDLE;
    m_instanceGpu = VK_NULL_HANDLE; m_instanceGpuAlloc = VK_NULL_HANDLE; m_instanceCount = 0;
    m_sampler = VK_NULL_HANDLE; m_view = VK_NULL_HANDLE; m_image = VK_NULL_HANDLE; m_allocation = VK_NULL_HANDLE;
    m_colourView = VK_NULL_HANDLE; m_colourImage = VK_NULL_HANDLE; m_colourAllocation = VK_NULL_HANDLE;
    m_built = false;
    m_instanceHash = 0;
    for (uint32_t l = 0; l < kLevels; ++l) m_levelBuilt[l] = false;
    m_ctx   = nullptr;
}

void GlobalDistanceField::setDetail(float scale) {
    scale = std::clamp(scale, 0.0625f, 8.0f);
    if (scale == m_detail) return;

    m_detail = scale;
    for (uint32_t l = 0; l < kLevels; ++l) m_levelBuilt[l] = false;   // every cell changes size
}

bool GlobalDistanceField::update(VkCommandBuffer cmd, uint32_t frameIndex,
                                 const std::vector<SdfInstance>& instances, const glm::vec3& eye) {
    if (!m_ctx || !m_computePipeline) return false;

    uint32_t count = static_cast<uint32_t>(instances.size());
    if (count > kMaxInstances) {
        if (!m_warnedFull) {
            FE_WARN("GlobalDistanceField: %u objects, only the first %u are in the distance field",
                    count, kMaxInstances);
            m_warnedFull = true;
        }
        count = kMaxInstances;
    }

    // ---- What changed? ----
    // The objects change every level; the camera changes the levels it has
    // moved away from the centre of (the big ones rarely).
    uint64_t instanceHash = 1469598103934665603ull;
    instanceHash = fnv(instanceHash, &count, sizeof(count));
    if (count > 0) instanceHash = fnv(instanceHash, instances.data(), sizeof(SdfInstance) * count);
    const bool instancesChanged = !m_built || instanceHash != m_instanceHash;

    glm::vec3 origins[kLevels];
    bool      dirty[kLevels];
    bool      any = false;
    for (uint32_t l = 0; l < kLevels; ++l) {
        const float step = kSnapCells * cellSize(l);
        origins[l] = glm::floor((eye - 0.5f * extent(l)) / step) * step;
        dirty[l]   = instancesChanged || !m_levelBuilt[l] || origins[l] != m_origin[l];
        any        = any || dirty[l];
    }
    if (!any) return false;

    // ---- Write the objects (this frame's copy) ----
    const uint32_t slot = frameIndex % kFrames;
    if (count > 0) {
        std::memcpy(m_instanceMapped[slot], instances.data(), sizeof(SdfInstance) * count);
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc[slot], 0, sizeof(SdfInstance) * count);
    }

    // ---- The scene shader's copy of the objects ----
    // Made by the GPU, in order with the frames: the host buffers are written
    // by the CPU for one frame in flight at a time, and the shader may read the
    // objects in frames where nothing changed.
    m_instanceCount = count;
    if (instancesChanged && count > 0) {
        VkMemoryBarrier2 beforeCopy{};
        beforeCopy.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        beforeCopy.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        beforeCopy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        beforeCopy.dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        beforeCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo beforeCopyDependency{};
        beforeCopyDependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        beforeCopyDependency.memoryBarrierCount = 1;
        beforeCopyDependency.pMemoryBarriers    = &beforeCopy;
        vkCmdPipelineBarrier2(cmd, &beforeCopyDependency);

        VkBufferCopy region{0, 0, sizeof(SdfInstance) * count};
        vkCmdCopyBuffer(cmd, m_instanceBuffer[slot], m_instanceGpu, 1, &region);

        VkMemoryBarrier2 afterCopy{};
        afterCopy.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        afterCopy.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        afterCopy.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        afterCopy.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        afterCopy.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo afterCopyDependency{};
        afterCopyDependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        afterCopyDependency.memoryBarrierCount = 1;
        afterCopyDependency.pMemoryBarriers    = &afterCopy;
        vkCmdPipelineBarrier2(cmd, &afterCopyDependency);
    }

    // ---- Wait for whatever still reads the field, then build the levels that need it ----
    VkMemoryBarrier2 before{};
    before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    before.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    before.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    before.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo beforeDependency{};
    beforeDependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    beforeDependency.memoryBarrierCount = 1;
    beforeDependency.pMemoryBarriers    = &before;
    vkCmdPipelineBarrier2(cmd, &beforeDependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computeLayoutPipeline, 0, 1, &m_computeSet[slot], 0, nullptr);

    for (uint32_t l = 0; l < kLevels; ++l) {
        if (!dirty[l]) continue;

        ComputePush push{};
        push.origin = glm::vec4(origins[l], cellSize(l));
        push.counts = glm::uvec4(count, m_resolution, l, 0);
        push.range  = glm::vec4(0.5f * extent(l), 0.5f * cellSize(l), 0.0f, 0.0f);
        vkCmdPushConstants(cmd, m_computeLayoutPipeline, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        vkCmdDispatch(cmd, m_resolution / 4, m_resolution / 4, m_resolution / 4);

        m_origin[l]     = origins[l];
        m_levelBuilt[l] = true;
    }

    VkMemoryBarrier2 after{};
    after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    after.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    after.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo afterDependency{};
    afterDependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    afterDependency.memoryBarrierCount = 1;
    afterDependency.pMemoryBarriers    = &after;
    vkCmdPipelineBarrier2(cmd, &afterDependency);

    m_instanceHash = instanceHash;
    m_built        = true;
    return true;
}

void GlobalDistanceField::drawDebug(VkCommandBuffer cmd, VkExtent2D extent, const glm::mat4& inverseViewProj) const {
    if (!m_built || !m_debugPipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipeline);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugLayout, 0, 1, &m_sampleSet, 0, nullptr);

    DebugPush push{};
    push.inverseViewProj = inverseViewProj;
    for (uint32_t l = 0; l < kLevels; ++l) push.origin[l] = glm::vec4(m_origin[l], cellSize(l));
    push.params          = glm::vec4(static_cast<float>(m_resolution), 0.0f, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, m_debugLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace fe
