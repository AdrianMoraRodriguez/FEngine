#include "gfx/MeshBuffer.h"
#include "gfx/VulkanContext.h"
#include "core/Log.h"

namespace fe {

MeshBuffer::~MeshBuffer() { destroy(); }

void MeshBuffer::destroy() {
    if (!m_ctx) return;

    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_vertexBuffer, m_vertexAllocation);
        m_vertexBuffer     = VK_NULL_HANDLE;
        m_vertexAllocation = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_indexBuffer, m_indexAllocation);
        m_indexBuffer     = VK_NULL_HANDLE;
        m_indexAllocation = VK_NULL_HANDLE;
    }
    m_indexCount = 0;
    m_ctx        = nullptr;
}

bool MeshBuffer::createBuffer(VkDeviceSize size,
                               VkBufferUsageFlags usage,
                               VmaMemoryUsage memUsage,
                               VkBuffer& buffer,
                               VmaAllocation& allocation) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size  = size;
    bufInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memUsage;

    return vmaCreateBuffer(m_ctx->allocator(), &bufInfo, &allocInfo,
                           &buffer, &allocation, nullptr) == VK_SUCCESS;
}

void MeshBuffer::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    // Allocate a one-time command buffer for the transfer.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_ctx->device(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx->graphicsQueue());

    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
}

bool MeshBuffer::upload(const VulkanContext& ctx,
                        const std::vector<Vertex>&   vertices,
                        const std::vector<uint32_t>& indices) {
    m_ctx        = &ctx;
    m_indexCount = static_cast<uint32_t>(indices.size());

    const VkDeviceSize vSize = sizeof(Vertex)    * vertices.size();
    const VkDeviceSize iSize = sizeof(uint32_t)  * indices.size();

    // --- Vertex buffer ---
    VkBuffer      stagingV; VmaAllocation stagingVAlloc;
    if (!createBuffer(vSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_CPU_ONLY,
                      stagingV, stagingVAlloc)) {
        FE_ERROR("MeshBuffer: failed to create vertex staging buffer");
        return false;
    }

    void* data = nullptr;
    vmaMapMemory(ctx.allocator(), stagingVAlloc, &data);
    memcpy(data, vertices.data(), vSize);
    vmaUnmapMemory(ctx.allocator(), stagingVAlloc);

    if (!createBuffer(vSize,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY,
                      m_vertexBuffer, m_vertexAllocation)) {
        FE_ERROR("MeshBuffer: failed to create vertex buffer");
        return false;
    }

    copyBuffer(stagingV, m_vertexBuffer, vSize);
    vmaDestroyBuffer(ctx.allocator(), stagingV, stagingVAlloc);

    // --- Index buffer ---
    VkBuffer      stagingI; VmaAllocation stagingIAlloc;
    if (!createBuffer(iSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_CPU_ONLY,
                      stagingI, stagingIAlloc)) {
        FE_ERROR("MeshBuffer: failed to create index staging buffer");
        return false;
    }

    vmaMapMemory(ctx.allocator(), stagingIAlloc, &data);
    memcpy(data, indices.data(), iSize);
    vmaUnmapMemory(ctx.allocator(), stagingIAlloc);

    if (!createBuffer(iSize,
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY,
                      m_indexBuffer, m_indexAllocation)) {
        FE_ERROR("MeshBuffer: failed to create index buffer");
        return false;
    }

    copyBuffer(stagingI, m_indexBuffer, iSize);
    vmaDestroyBuffer(ctx.allocator(), stagingI, stagingIAlloc);

    FE_INFO("MeshBuffer: uploaded %zu vertices, %zu indices",
            vertices.size(), indices.size());
    return true;
}

void MeshBuffer::draw(VkCommandBuffer cmd) const {
    VkBuffer     buffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

} // namespace fe
