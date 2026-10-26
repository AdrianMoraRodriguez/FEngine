#pragma once

/**
 * @file Environment.h
 * @brief The sky as the GPU holds it: a reflection cube map and the diffuse lighting coefficients.
 */

#include "gfx/EnvironmentBake.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace fe {

class VulkanContext;

/**
 * @brief One baked environment on the GPU, with the descriptor set that binds it.
 *
 * The descriptor set follows the layout from createLayout(), the same for every
 * Environment, so the pipelines are built against that layout and any
 * Environment can be bound to them:
 *   - binding 0: the cube map (RGBA16F, all mip levels), a combined image sampler;
 *   - binding 1: a uniform buffer with the nine diffuse coefficients.
 *
 * ### Replacing the contents
 * upload() waits for the GPU to be idle before it swaps the image and rewrites
 * the descriptors, so the frames in flight never see a half-changed set. That
 * stall is a few milliseconds and happens only when the user picks another
 * environment. Call it between frames (SceneRenderer::prepare), never while
 * recording one.
 */
class Environment {
public:
    Environment() = default;
    ~Environment() { shutdown(); }

    Environment(const Environment&)            = delete;
    Environment& operator=(const Environment&) = delete;

    /// Creates the layout of set 2 of the mesh pipeline (set 0 of the sky's).
    /// The caller owns it and destroys it with vkDestroyDescriptorSetLayout.
    static VkDescriptorSetLayout createLayout(VkDevice device);

    /**
     * @brief Creates the descriptor set, the sampler and the uniform buffer.
     *        There is no image yet: call upload() before binding the set.
     * @param layout From createLayout(); must outlive this object.
     */
    bool init(const VulkanContext& ctx, VkDescriptorSetLayout layout);

    /// Frees everything. Safe to call twice. The GPU must be done with it.
    void shutdown();

    /**
     * @brief Replaces the environment with @p bake.
     * @return `false` if the bake is invalid or a Vulkan call failed; the
     *         previous environment, if any, is then gone: check valid().
     */
    bool upload(const EnvironmentBake& bake);

    /// @return `true` once an environment has been uploaded.
    bool valid() const { return m_view != VK_NULL_HANDLE; }

    /// @return The descriptor set to bind. Only usable if valid().
    VkDescriptorSet set() const { return m_set; }

private:
    void destroyImage();

    const VulkanContext* m_ctx     = nullptr;
    VkDescriptorPool     m_pool    = VK_NULL_HANDLE;
    VkDescriptorSet      m_set     = VK_NULL_HANDLE;
    VkSampler            m_sampler = VK_NULL_HANDLE;
    VkImage              m_image   = VK_NULL_HANDLE;
    VmaAllocation        m_imageAllocation = nullptr;
    VkImageView          m_view    = VK_NULL_HANDLE;

    VkBuffer             m_uniform           = VK_NULL_HANDLE;
    VmaAllocation        m_uniformAllocation = nullptr;
    void*                m_uniformMapped     = nullptr;
};

} // namespace fe
