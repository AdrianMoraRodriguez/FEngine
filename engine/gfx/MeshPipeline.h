#pragma once

/**
 * @file MeshPipeline.h
 * @brief Vulkan graphics pipeline for rendering meshes.
 *
 * Loads the compiled SPIR-V shaders, configures all fixed-function state
 * (vertex input, rasteriser, depth test, blending) and creates the pipeline.
 * Its layout has two ways of passing data to the shaders:
 *   - push constants, rewritten for every draw call (the matrices), and
 *   - descriptor set 0, bound once per frame (the scene's lights; see
 *     SceneUniforms).
 *
 * Uses dynamic rendering: no VkRenderPass is needed.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/// Push constant data sent to the vertex shader each draw call.
/// Mirrors the `PushConstants` block in mesh.vert.
struct MeshPushConstants {
    glm::mat4 mvp;   ///< Model-View-Projection matrix.
    glm::mat4 model; ///< Object-to-world matrix, used to bring normals to world space.
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
     * @param ctx         Initialised Vulkan context.
     * @param colorFormat Format of the colour attachment.
     * @param depthFormat Format of the depth attachment.
     * @param shaderDir   Directory containing mesh.vert.spv / mesh.frag.spv.
     * @param sceneLayout Layout of descriptor set 0, which the shaders read
     *                    the scene data from (SceneUniforms::layout()).
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx,
              VkFormat colorFormat,
              VkFormat depthFormat,
              const char* shaderDir,
              VkDescriptorSetLayout sceneLayout);

    /// Destroys all Vulkan objects.
    void shutdown();

    /**
     * @brief Binds the pipeline and sets the dynamic viewport and scissor.
     *
     * Call once per pass, then bindDescriptorSet(), then call push() + a
     * draw for each object.
     *
     * @param cmd    Command buffer to record into.
     * @param extent Size of the render area.
     */
    void bind(VkCommandBuffer cmd, VkExtent2D extent) const;

    /**
     * @brief Binds the descriptor set that holds the scene data (set 0).
     *
     * Call once per pass, after bind(). The set stays bound for every draw
     * call that follows.
     */
    void bindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet sceneSet) const;

    /**
     * @brief Uploads the per-object push constants.
     *
     * @param cmd Command buffer to record into.
     * @param pc  Matrices for the next draw call.
     */
    void push(VkCommandBuffer cmd, const MeshPushConstants& pc) const;

private:
    VkShaderModule loadShader(const char* path) const;

    const VulkanContext* m_ctx      = nullptr;
    VkPipelineLayout     m_layout   = VK_NULL_HANDLE;
    VkPipeline           m_pipeline = VK_NULL_HANDLE;
};

} // namespace fe
