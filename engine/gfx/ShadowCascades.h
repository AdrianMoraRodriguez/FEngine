#pragma once

/**
 * @file ShadowCascades.h
 * @brief The depth images that hold the sun's cascaded shadow maps, one per
 *        cascade, stored as a single array image, and the sampler that reads them.
 *
 * One map for a whole scene gets blurry when the scene is large. Cascaded
 * shadow maps cut what the camera sees into slices by distance and give each
 * slice a map of its own: the slice next to the camera is small, so its map
 * is sharp, while the far slices are large but also far away, where the eye
 * cannot tell. All the maps live as layers of one image, so the shader reads
 * them through a single sampler and picks the layer with a number.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief A 2D array depth image used as the shadow map of a cascaded light.
 *
 * Like ShadowMap, it is redrawn every frame and shared by every frame in
 * flight, and begin() / end() must run every frame even when nothing uses it,
 * so the image stays in the layout the shader's descriptor promises.
 *
 * Usage, outside any other render pass:
 * @code
 * cascades.begin(cmd);
 * for (uint32_t i = 0; i < ShadowCascades::kCascades; ++i) {
 *     cascades.beginCascade(cmd, i);
 *     // ... draw the shadow casters with that cascade's view-projection ...
 *     cascades.endCascade(cmd);
 * }
 * cascades.end(cmd);
 * @endcode
 */
class ShadowCascades {
public:
    /// Number of cascades (layers of the image).
    static constexpr uint32_t kCascades = 4;

    /// Width and height of each cascade in texels.
    static constexpr uint32_t kResolution = 2048;

    /// Depth format of the cascades.
    static constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;

    ShadowCascades() = default;
    ~ShadowCascades();

    ShadowCascades(const ShadowCascades&)            = delete;
    ShadowCascades& operator=(const ShadowCascades&) = delete;

    /// Creates the image, its views and the sampler. @return `true` on success.
    bool init(const VulkanContext& ctx);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /// Prepares every cascade for drawing. Call once before the cascades.
    void begin(VkCommandBuffer cmd);

    /**
     * @brief Opens a depth-only render pass on one cascade and clears it.
     *
     * Also sets the viewport and scissor.
     *
     * @param cascade Index from 0 (nearest to the camera) to kCascades - 1.
     */
    void beginCascade(VkCommandBuffer cmd, uint32_t cascade);

    /// Closes the pass opened by beginCascade().
    void endCascade(VkCommandBuffer cmd);

    /// Makes every cascade readable by fragment shaders. Call after the last one.
    void end(VkCommandBuffer cmd);

    /// @return Array view of the image (all cascades), for the descriptor set.
    VkImageView view() const { return m_arrayView; }

    /// @return Comparison sampler (see ShadowMap::sampler()).
    VkSampler sampler() const { return m_sampler; }

    /// @return Plain nearest-neighbour sampler over the same depth image. It
    ///         returns the stored depth itself instead of a lit / shadowed
    ///         answer, which the shadow filter needs to find blockers.
    VkSampler depthSampler() const { return m_depthSampler; }

private:
    void barrier(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

    const VulkanContext* m_ctx = nullptr;

    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;

    VkImageView m_arrayView = VK_NULL_HANDLE;                      ///< All cascades, for sampling.
    std::array<VkImageView, kCascades> m_cascadeViews{};           ///< One layer each, for drawing.
    VkSampler   m_sampler   = VK_NULL_HANDLE;
    VkSampler     m_depthSampler = VK_NULL_HANDLE;
};

} // namespace fe
