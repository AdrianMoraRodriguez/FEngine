#pragma once

/**
 * @file Vertex.h
 * @brief Per-vertex data layout and Vulkan binding/attribute descriptions.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <cstddef>

namespace fe {

/**
 * @brief A single mesh vertex: position, normal, colour and texture coordinates.
 *
 * The layout matches the vertex shader's `layout(location = N) in` inputs:
 *   - location 0: position (vec3)
 *   - location 1: normal   (vec3)
 *   - location 2: color    (vec3)
 *   - location 3: uv       (vec2)
 */
struct Vertex {
    glm::vec3 position; ///< Object-space position.
    glm::vec3 normal;   ///< Object-space unit normal, used for lighting.
    glm::vec3 color;    ///< Linear RGB vertex colour.

    /// Texture coordinates. (0,0) is the top-left corner of an image and
    /// (1,1) the bottom-right one, the same as the order in which image files
    /// store their rows. Values outside [0,1] are legal: the sampler's wrap
    /// mode decides what they mean (tiling, by default).
    glm::vec2 uv{0.0f};

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
    static std::array<VkVertexInputAttributeDescription, 4>
    attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 4> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset   = offsetof(Vertex, position);

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset   = offsetof(Vertex, normal);

        attrs[2].binding  = 0;
        attrs[2].location = 2;
        attrs[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[2].offset   = offsetof(Vertex, color);

        attrs[3].binding  = 0;
        attrs[3].location = 3;
        attrs[3].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[3].offset   = offsetof(Vertex, uv);

        return attrs;
    }
};

} // namespace fe
