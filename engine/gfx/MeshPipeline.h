#pragma once

/**
 * @file MeshPipeline.h
 * @brief Vulkan graphics pipeline for rendering lit meshes.
 *
 * Loads the compiled SPIR-V shaders, configures all fixed-function state
 * (vertex input, rasteriser, depth test, blending) and creates the pipeline
 * with a push-constant layout for the MVP matrix.
 *
 * Uses dynamic rendering — no VkRenderPass needed.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/**
 * @brief Push constant data sent to the vertex shader each draw call.
 */
struct MeshPushConstants {
    glm::mat4 mvp; ///< Model-View-Projection matrix.
};

/**
 * @brief The graphics pipeline used to draw meshes into the viewport.
 */
class MeshPipeline {
public:
    MeshPipeline() = default;
    ~MeshPipeline();

    MeshPipeline(const MeshPipeline&)            = delete;
    MeshPipeline& operator=(const MeshPipeline&) = delete;

    /**
     * @brief Creates the pipeline layout and pipeline.
     *
     * @param ctx           Initialised Vulkan context.
     * @param colorFormat   Format of the colour attachment (offscreen buffer).
     * @param shaderDir     Directory containing mesh.vert.spv / mesh.frag.spv.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx,
              VkFormat colorFormat,
              const char* shaderDir);

    /// Destroys all Vulkan objects.
    void shutdown();

    /**
     * @brief Binds the pipeline and records a push-constant upload.
     *
     * @param cmd Command buffer to record into.
     * @param pc  MVP matrix to send to the vertex shader.
     */
    void bind(VkCommandBuffer cmd, const MeshPushConstants& pc) const;

private:
    VkShaderModule loadShader(const char* path) const;

    const VulkanContext* m_ctx            = nullptr;
    VkPipelineLayout     m_layout         = VK_NULL_HANDLE;
    VkPipeline           m_pipeline       = VK_NULL_HANDLE;
};

} // namespace fe
