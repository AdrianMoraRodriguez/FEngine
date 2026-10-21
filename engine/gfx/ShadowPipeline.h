#pragma once

/**
 * @file ShadowPipeline.h
 * @brief Depth-only graphics pipeline used to draw the shadow map.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/// Push constant data of the shadow vertex shader. Mirrors `PushConstants` in shadow.vert.
struct ShadowPushConstants {
    glm::mat4 lightMvp;   ///< Light view-projection * object-to-world.
};

/**
 * @brief The pipeline that draws the shadow casters as seen from the light.
 *
 * It has no fragment shader and no colour attachment: only depth is written.
 * It needs no descriptor sets either, just one matrix per object.
 *
 * Back faces are not culled. That costs nothing here and keeps thin surfaces
 * (the ground plane, a single quad) casting a shadow whichever way they face.
 * The self-shadowing artefacts this could cause ("shadow acne") are handled
 * by a depth bias applied by the rasteriser, and by a normal offset in
 * mesh.frag.
 */
class ShadowPipeline {
public:
    ShadowPipeline() = default;
    ~ShadowPipeline();

    ShadowPipeline(const ShadowPipeline&)            = delete;
    ShadowPipeline& operator=(const ShadowPipeline&) = delete;

    /**
     * @brief Creates the pipeline layout and pipeline.
     *
     * @param ctx         Initialised Vulkan context.
     * @param depthFormat Format of the shadow map (ShadowMap::kFormat).
     * @param shaderDir   Directory containing shadow.vert.spv.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, VkFormat depthFormat, const char* shaderDir);

    /// Destroys all Vulkan objects. Safe to call more than once.
    void shutdown();

    /// Binds the pipeline. The viewport and scissor are set by ShadowMap::begin().
    void bind(VkCommandBuffer cmd) const;

    /// Uploads the matrix for the next draw call.
    void push(VkCommandBuffer cmd, const ShadowPushConstants& pc) const;

private:
    const VulkanContext* m_ctx      = nullptr;
    VkPipelineLayout     m_layout   = VK_NULL_HANDLE;
    VkPipeline           m_pipeline = VK_NULL_HANDLE;
};

} // namespace fe
