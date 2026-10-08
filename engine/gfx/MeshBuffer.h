#pragma once

/**
 * @file MeshBuffer.h
 * @brief GPU vertex and index buffers for a static mesh.
 *
 * Allocates two VMA-backed buffers (vertex + index) using a staging
 * upload strategy: data is written to a CPU-visible staging buffer and
 * then copied to a GPU-only buffer via a one-time command buffer.
 * The GPU-only buffer is faster to read during rendering.
 */

#include "gfx/Vertex.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief Holds a mesh's vertex and index data on the GPU.
 *
 * Immutable after upload — to update a mesh, destroy and recreate.
 */
class MeshBuffer {
public:
    MeshBuffer() = default;
    ~MeshBuffer();

    MeshBuffer(const MeshBuffer&)            = delete;
    MeshBuffer& operator=(const MeshBuffer&) = delete;

    /**
     * @brief Uploads vertex and index data to GPU-only memory.
     *
     * Creates a staging buffer, copies the data, then issues a transfer
     * command. The staging buffer is destroyed after the copy.
     *
     * @param ctx      Initialised Vulkan context.
     * @param vertices Vertex data to upload.
     * @param indices  Index data to upload (uint32_t indices).
     * @return `true` on success.
     */
    bool upload(const VulkanContext& ctx,
                const std::vector<Vertex>&   vertices,
                const std::vector<uint32_t>& indices);

    /// Destroys the GPU buffers. Safe to call if upload() was never called.
    void destroy();

    /// Binds the vertex buffer and index buffer, then records a draw call.
    void draw(VkCommandBuffer cmd) const;

    /// @return true if the buffers have been successfully uploaded.
    bool isValid() const { return m_vertexBuffer != VK_NULL_HANDLE; }

    /// @return Number of indices (used for the draw call).
    uint32_t indexCount() const { return m_indexCount; }

private:
    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VmaMemoryUsage memUsage,
                      VkBuffer& buffer,
                      VmaAllocation& allocation);

    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

    const VulkanContext* m_ctx = nullptr;

    VkBuffer      m_vertexBuffer     = VK_NULL_HANDLE;
    VmaAllocation m_vertexAllocation = VK_NULL_HANDLE;

    VkBuffer      m_indexBuffer      = VK_NULL_HANDLE;
    VmaAllocation m_indexAllocation  = VK_NULL_HANDLE;

    uint32_t m_indexCount = 0;
};

} // namespace fe
