#pragma once

/**
 * @file SkyPipeline.h
 * @brief Draws the environment as the background of the scene.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/// Push constants of sky.frag.
struct SkyPushConstants {
    glm::mat4 inverseViewProj;   ///< Clip space -> world space.
    glm::vec4 params;            ///< x: brightness of the sky.
};
static_assert(sizeof(SkyPushConstants) == 80, "Must match the Push block in sky.frag");

/**
 * @brief A fullscreen triangle on the far plane that shows the environment's
 *        cube map in the direction each pixel looks.
 *
 * Draw it *after* the meshes: it is depth-tested against the far plane, so only
 * the pixels nothing else covered run its shader.
 */
class SkyPipeline {
public:
    SkyPipeline() = default;
    ~SkyPipeline() { shutdown(); }

    SkyPipeline(const SkyPipeline&)            = delete;
    SkyPipeline& operator=(const SkyPipeline&) = delete;

    /**
     * @param environmentLayout Layout of Environment's descriptor set (set 0 here).
     * @param shaderDir         Directory with sky.vert.spv / sky.frag.spv.
     */
    bool init(const VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
              const char* shaderDir, VkDescriptorSetLayout environmentLayout);

    void shutdown();

    /**
     * @brief Records the draw call.
     * @param cmd           Command buffer inside an offscreen render pass.
     * @param extent        Size of the render target.
     * @param environment   Environment::set().
     * @param viewProj      The camera's projection * view (with the Vulkan Y flip).
     * @param brightness    Multiplier of the sky's colours.
     */
    void draw(VkCommandBuffer cmd, VkExtent2D extent, VkDescriptorSet environment,
              const glm::mat4& viewProj, float brightness) const;

private:
    VkShaderModule loadShader(const char* path) const;

    const VulkanContext* m_ctx      = nullptr;
    VkPipelineLayout     m_layout   = VK_NULL_HANDLE;
    VkPipeline           m_pipeline = VK_NULL_HANDLE;
};

} // namespace fe
