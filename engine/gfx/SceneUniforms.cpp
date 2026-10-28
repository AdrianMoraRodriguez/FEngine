#include "gfx/SceneUniforms.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <cstring>

namespace fe {

SceneUniforms::~SceneUniforms() { shutdown(); }

bool SceneUniforms::init(const VulkanContext& ctx, const ShadowBindings& shadows) {
    m_ctx = &ctx;
    constexpr uint32_t kFrames = Renderer::MAX_FRAMES_IN_FLIGHT + 2;   // + the preview and probe slots

    // ------------------------------------------------------------------
    // 1. Layout: the contract.
    //      binding 0: uniform buffer with the lights (both stages can read it)
    //      binding 1: the flat shadow maps, an array of sampled images read by fragment shaders
    //      binding 2: the shadow cubes of the point lights, likewise
    //      binding 3: the sun's shadow cascades, one array image
    //      bindings 4, 5, 6: the same three, through plain samplers (raw depth)
    // ------------------------------------------------------------------
    VkDescriptorSetLayoutBinding bindings[7]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = kMaxShadowCasters;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = kMaxPointShadowCasters;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = kMaxShadowCasters;
    bindings[4].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[5].binding         = 5;
    bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = kMaxPointShadowCasters;
    bindings[5].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[6].binding         = 6;
    bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(ctx.device(), &layoutInfo, nullptr,
                                    &m_layout) != VK_SUCCESS) {
        FE_ERROR("SceneUniforms: failed to create descriptor set layout");
        shutdown();
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Pool: memory from which the sets are allocated. One set per frame slot.
    // ------------------------------------------------------------------
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFrames;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFrames * 2 * (kMaxShadowCasters + kMaxPointShadowCasters + 1);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = kFrames;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("SceneUniforms: failed to create descriptor pool");
        shutdown();
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Sets: one per frame slot, all with the same layout.
    // ------------------------------------------------------------------
    VkDescriptorSetLayout layouts[kFrames];
    for (VkDescriptorSetLayout& l : layouts) l = m_layout;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = kFrames;
    allocInfo.pSetLayouts        = layouts;

    VkDescriptorSet sets[kFrames];
    if (vkAllocateDescriptorSets(ctx.device(), &allocInfo, sets) != VK_SUCCESS) {
        FE_ERROR("SceneUniforms: failed to allocate descriptor sets");
        shutdown();
        return false;
    }

    // ------------------------------------------------------------------
    // 4. Buffers: written by the CPU every frame, read by the GPU.
    //    They stay mapped for their whole life, so updating one is a memcpy.
    // ------------------------------------------------------------------
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = sizeof(SceneUniformData);
    bufferInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                      | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    const SceneUniformData initial{};

    for (uint32_t i = 0; i < kFrames; ++i) {
        Slot& slot = m_slots[i];
        slot.set = sets[i];

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(ctx.allocator(), &bufferInfo, &allocCreate,
                            &slot.buffer, &slot.allocation, &info) != VK_SUCCESS) {
            FE_ERROR("SceneUniforms: failed to create uniform buffer %u", i);
            shutdown();
            return false;
        }
        slot.mapped = info.pMappedData;

        // Point the set's binding 0 at this slot's buffer.
        VkDescriptorBufferInfo bufferDescriptor{};
        bufferDescriptor.buffer = slot.buffer;
        bufferDescriptor.offset = 0;
        bufferDescriptor.range  = sizeof(SceneUniformData);

        // Point bindings 1 to 6 at the shadow images. Every image is always in
        // SHADER_READ_ONLY_OPTIMAL by the time a draw call reads it (the end()
        // of each shadow class leaves it there). The plain-sampler bindings
        // (4, 5, 6) use the same views as the comparison ones (1, 2, 3).
        VkDescriptorImageInfo mapCompare[kMaxShadowCasters]{};
        VkDescriptorImageInfo mapDepth[kMaxShadowCasters]{};
        for (uint32_t m = 0; m < kMaxShadowCasters; ++m) {
            mapCompare[m] = {shadows.mapSamplers[m],      shadows.mapViews[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            mapDepth[m]   = {shadows.mapDepthSamplers[m], shadows.mapViews[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }

        VkDescriptorImageInfo cubeCompare[kMaxPointShadowCasters]{};
        VkDescriptorImageInfo cubeDepth[kMaxPointShadowCasters]{};
        for (uint32_t m = 0; m < kMaxPointShadowCasters; ++m) {
            cubeCompare[m] = {shadows.cubeSamplers[m],      shadows.cubeViews[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            cubeDepth[m]   = {shadows.cubeDepthSamplers[m], shadows.cubeViews[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }

        const VkDescriptorImageInfo cascadeCompare{shadows.cascadeSampler,      shadows.cascadeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo cascadeDepth  {shadows.cascadeDepthSampler, shadows.cascadeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[7]{};
        for (VkWriteDescriptorSet& w : writes) {
            w.sType          = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet         = slot.set;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo    = &bufferDescriptor;

        writes[1].dstBinding = 1; writes[1].descriptorCount = kMaxShadowCasters;      writes[1].pImageInfo = mapCompare;
        writes[2].dstBinding = 2; writes[2].descriptorCount = kMaxPointShadowCasters; writes[2].pImageInfo = cubeCompare;
        writes[3].dstBinding = 3; writes[3].descriptorCount = 1;                      writes[3].pImageInfo = &cascadeCompare;
        writes[4].dstBinding = 4; writes[4].descriptorCount = kMaxShadowCasters;      writes[4].pImageInfo = mapDepth;
        writes[5].dstBinding = 5; writes[5].descriptorCount = kMaxPointShadowCasters; writes[5].pImageInfo = cubeDepth;
        writes[6].dstBinding = 6; writes[6].descriptorCount = 1;                      writes[6].pImageInfo = &cascadeDepth;
        vkUpdateDescriptorSets(ctx.device(), 7, writes, 0, nullptr);

        // Valid contents from the start, in case a frame draws before update().
        update(i, initial);
    }

    FE_INFO("SceneUniforms: created (%u frame slots, %zu bytes each)",
            kFrames, sizeof(SceneUniformData));
    return true;
}

void SceneUniforms::shutdown() {
    if (!m_ctx) return;

    for (Slot& slot : m_slots) {
        if (slot.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_ctx->allocator(), slot.buffer, slot.allocation);
        slot = Slot{};
    }

    // Destroying the pool frees the sets allocated from it.
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }

    m_ctx = nullptr;
}

void SceneUniforms::update(uint32_t frameIndex, const SceneUniformData& data) {
    Slot& slot = m_slots[slotFor(frameIndex)];
    if (!slot.mapped) return;

    std::memcpy(slot.mapped, &data, sizeof(data));

    // A no-op on coherent memory; needed on memory that is not coherent.
    vmaFlushAllocation(m_ctx->allocator(), slot.allocation, 0, sizeof(data));
}

VkDescriptorSet SceneUniforms::set(uint32_t frameIndex) const {
    return m_slots[slotFor(frameIndex)].set;
}

} // namespace fe
