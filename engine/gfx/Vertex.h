#pragma once

/**
 * @file Vertex.h
 * @brief Per-vertex data layout and Vulkan binding/attribute descriptions.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>

namespace fe {

/**
 * @brief A single mesh vertex with position and colour.
 *
 * The layout matches the vertex shader's `layout(location = N) in` inputs:
 *   - location 0: position (vec3)
 *   - location 1: color   (vec3)
 */
struct Vertex {
    glm::vec3 position; ///< Object-space position.
    glm::vec3 color;    ///< Linear RGB vertex colour.

    /**
     * @brief Returns the VkVertexInputBindingDescription for this struct.
     *
     * Describes how the vertex buffer is stepped through: one Vertex per
     * vertex (not per instance), at a stride of sizeof(Vertex).
     */
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription desc{};
        desc.binding   = 0;
        desc.stride    = sizeof(Vertex);
        desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return desc;
    }

    /**
     * @brief Returns the attribute descriptions for each field.
     *
     * Maps each struct member to its shader location, format and byte offset.
     */
    static std::array<VkVertexInputAttributeDescription, 2>
    attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attrs{};

        // location 0: position — three 32-bit floats
        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset   = offsetof(Vertex, position);

        // location 1: color — three 32-bit floats
        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset   = offsetof(Vertex, color);

        return attrs;
    }
};

} // namespace fe
