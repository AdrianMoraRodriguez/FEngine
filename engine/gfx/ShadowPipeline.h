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
 * It has no colour attachment: only depth is written. Solid objects need no
 * fragment shader and no descriptor sets, just one matrix per object.
 *
 * Cut-out materials (AlphaMode::Mask) use a second variant of the pipeline
 * whose fragment shader reads the albedo's alpha through the material's own
 * descriptor set and discards the pixels the material does not draw, so a
 * fence or a leaf casts the shadow of its shape and not of its whole quad.
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
     * @param shaderDir   Directory containing shadow.vert.spv, shadow_mask.vert.spv
     *                    and shadow_mask.frag.spv.
     * @param materialLayout Layout of the material descriptor sets
     *                    (TextureLibrary::layout()), for the cut-out variant.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, VkFormat depthFormat, const char* shaderDir,
              VkDescriptorSetLayout materialLayout);

    /// Destroys all Vulkan objects. Safe to call more than once.
    void shutdown();

    /// Binds the pipeline for solid objects. The viewport and scissor are set by ShadowMap::begin().
    void bind(VkCommandBuffer cmd) const;

    /// Binds the cut-out variant with the material set whose alpha it reads.
    /// Call it again to change the material; call bind() to go back to solid.
    void bindMasked(VkCommandBuffer cmd, VkDescriptorSet materialSet) const;

    /// Uploads the matrix for the next draw call (to whichever variant is bound).
    void push(VkCommandBuffer cmd, const ShadowPushConstants& pc) const;

private:
    const VulkanContext* m_ctx      = nullptr;
    VkPipelineLayout     m_layout   = VK_NULL_HANDLE;
    VkPipeline           m_pipeline = VK_NULL_HANDLE;

    VkPipelineLayout     m_maskedLayout   = VK_NULL_HANDLE;   ///< Set 0: the material.
    VkPipeline           m_maskedPipeline = VK_NULL_HANDLE;
    mutable bool         m_maskedBound    = false;            ///< Which variant push() goes to.

    /// Builds one variant. @p fragName is null for the solid one.
    bool createPipeline(VkFormat depthFormat, const char* shaderDir, const char* vertName,
                        const char* fragName, VkPipelineLayout layout, bool withUv,
                        VkPipeline& out);
};

} // namespace fe
