#include "gfx/SceneUniforms.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <cstring>

namespace fe {

SceneUniforms::~SceneUniforms() { shutdown(); }

bool SceneUniforms::init(const VulkanContext& ctx,
                         VkImageView shadowView, VkSampler shadowSampler) {
    m_ctx = &ctx;
    constexpr uint32_t kFrames = Renderer::MAX_FRAMES_IN_FLIGHT;

    // ------------------------------------------------------------------
    // 1. Layout: the contract.
    //      binding 0: uniform buffer with the lights (both stages can read it)
    //      binding 1: the shadow map, a sampled image read by fragment shaders
    // ------------------------------------------------------------------
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
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
    poolSizes[1].descriptorCount = kFrames;

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

        // Point binding 1 at the shadow map. The image is always in
        // SHADER_READ_ONLY_OPTIMAL by the time a draw call reads it
        // (ShadowMap::end() leaves it there).
        VkDescriptorImageInfo shadowDescriptor{};
        shadowDescriptor.sampler     = shadowSampler;
        shadowDescriptor.imageView   = shadowView;
        shadowDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = slot.set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &bufferDescriptor;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = slot.set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &shadowDescriptor;
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

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
    Slot& slot = m_slots[frameIndex % Renderer::MAX_FRAMES_IN_FLIGHT];
    if (!slot.mapped) return;

    std::memcpy(slot.mapped, &data, sizeof(data));

    // A no-op on coherent memory; needed on memory that is not coherent.
    vmaFlushAllocation(m_ctx->allocator(), slot.allocation, 0, sizeof(data));
}

VkDescriptorSet SceneUniforms::set(uint32_t frameIndex) const {
    return m_slots[frameIndex % Renderer::MAX_FRAMES_IN_FLIGHT].set;
}

} // namespace fe
