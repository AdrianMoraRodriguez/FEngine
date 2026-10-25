#pragma once

/**
 * @file OutlinePass.h
 * @brief Draws an outline around the selected objects.
 *
 * Done in two steps, because an outline is a property of the object's shape
 * on screen, not of its surface:
 *   1. The selected objects are drawn into a small single-channel image, the
 *      mask, as 1 where they cover a pixel and 0 elsewhere.
 *   2. A full-screen pass over the scene colour looks around every pixel of
 *      the mask. Any pixel that is not covered but has a covered pixel within
 *      a few pixels of it gets the outline colour.
 *
 * The outline is drawn over everything, so a selected object that is hidden
 * behind another one can still be found.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/// Push constants of the mask pipeline: one object's transform.
struct OutlineMaskPushConstants {
    glm::mat4 mvp;   ///< Camera view-projection * object-to-world.
};

/**
 * @brief Owns the mask image and the two pipelines that make the outline.
 *
 * Per frame, only when something is selected:
 * @code
 * outline.beginMask(cmd, extent);           // outside any render pass
 * for each selected mesh:
 *     outline.pushMask(cmd, mvp);
 *     mesh.draw(cmd);
 * outline.endMask(cmd);
 *
 * // ... scene pass ...
 * outline.draw(cmd, extent);                // inside it, after the scene
 * @endcode
 * The mask follows the size of the scene render target: beginMask() makes a
 * new one whenever @p extent changes.
 */
class OutlinePass {
public:
    OutlinePass() = default;
    ~OutlinePass();

    OutlinePass(const OutlinePass&)            = delete;
    OutlinePass& operator=(const OutlinePass&) = delete;

    /**
     * @param ctx         Initialised Vulkan context.
     * @param colorFormat Format of the scene colour target the outline is drawn on.
     * @param depthFormat Format of that pass's depth target (the pass has one,
     *                    even though the outline does not use it).
     * @param shaderDir   Directory containing the compiled SPIR-V shaders.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
              const char* shaderDir);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /// Opens the mask pass and clears the mask. Call outside any render pass.
    /// @return `false` if the mask could not be created; do not draw then.
    bool beginMask(VkCommandBuffer cmd, VkExtent2D extent);

    /// Sets the transform of the next object drawn into the mask.
    void pushMask(VkCommandBuffer cmd, const OutlineMaskPushConstants& pc) const;

    /// Closes the mask pass and makes the mask readable by draw().
    void endMask(VkCommandBuffer cmd);

    /// Draws the outline over the scene. Call inside the scene's render pass,
    /// after everything else, and only after a beginMask() / endMask() pair
    /// this frame.
    void draw(VkCommandBuffer cmd, VkExtent2D extent) const;

    /// Colour of the outline.
    glm::vec3 color{1.0f, 0.6f, 0.1f};

    /// Thickness of the outline in pixels.
    float thickness = 2.0f;

private:
    bool createMask(VkExtent2D extent);
    void destroyMask();
    VkShaderModule loadShader(const char* path) const;
    bool createMaskPipeline(const char* shaderDir);
    bool createOutlinePipeline(VkFormat colorFormat, VkFormat depthFormat, const char* shaderDir);

    const VulkanContext* m_ctx = nullptr;

    // The mask
    VkImage       m_maskImage      = VK_NULL_HANDLE;
    VmaAllocation m_maskAllocation = VK_NULL_HANDLE;
    VkImageView   m_maskView       = VK_NULL_HANDLE;
    VkSampler     m_maskSampler    = VK_NULL_HANDLE;
    VkExtent2D    m_maskExtent{0, 0};

    // Descriptor set that exposes the mask to the outline shader
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_set       = VK_NULL_HANDLE;

    // Step 1: draws objects into the mask
    VkPipelineLayout m_maskLayout   = VK_NULL_HANDLE;
    VkPipeline       m_maskPipeline = VK_NULL_HANDLE;

    // Step 2: draws the outline over the scene
    VkPipelineLayout m_outlineLayout   = VK_NULL_HANDLE;
    VkPipeline       m_outlinePipeline = VK_NULL_HANDLE;
};

} // namespace fe
