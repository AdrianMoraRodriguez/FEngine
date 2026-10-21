#pragma once

/**
 * @file ShadowMap.h
 * @brief The depth image a light's shadows are drawn into, and the sampler
 *        that reads it back.
 *
 * A shadow map is the scene seen from the light, keeping only the distance
 * to the nearest surface in each direction. While shading the scene, a pixel
 * compares its own distance to the light with the one stored here: if
 * something nearer was recorded, the pixel is in shadow.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief A square depth image used as a shadow map, plus its comparison sampler.
 *
 * One image is shared by every frame in flight. That is safe because all the
 * work goes through a single queue, in order: the barrier at the start of
 * begin() waits for the previous frame's reads to finish before the image is
 * written again.
 *
 * The image is cleared and redrawn every frame, so begin() and end() must run
 * every frame, even when no light casts shadows. That keeps the image in the
 * layout the shader's descriptor promises (SHADER_READ_ONLY_OPTIMAL).
 *
 * Usage, outside any other render pass:
 * @code
 * shadowMap.begin(cmd);
 * // ... bind a depth-only pipeline and draw the shadow casters ...
 * shadowMap.end(cmd);
 * @endcode
 */
class ShadowMap {
public:
    /// Width and height of the map in texels. Bigger is sharper and slower.
    static constexpr uint32_t kResolution = 2048;

    /// Depth format of the map.
    static constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;

    ShadowMap() = default;
    ~ShadowMap();

    ShadowMap(const ShadowMap&)            = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    /**
     * @brief Creates the image, its view and the sampler.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /**
     * @brief Opens the depth-only render pass: prepares the image and clears
     *        it to the farthest depth (1.0).
     *
     * Also sets the viewport and scissor to the whole map.
     */
    void begin(VkCommandBuffer cmd);

    /// Closes the pass and makes the image readable by fragment shaders.
    void end(VkCommandBuffer cmd);

    /// @return View of the depth image, for the descriptor set.
    VkImageView view() const { return m_view; }

    /**
     * @brief The sampler to read the map with.
     *
     * A *comparison* sampler: instead of returning the stored depth, it
     * compares it with a reference given by the shader and returns 1 (lit) or
     * 0 (shadowed), already blended between the four nearest texels. Outside
     * the map it answers "lit".
     */
    VkSampler sampler() const { return m_sampler; }

private:
    const VulkanContext* m_ctx = nullptr;

    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_view       = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;
};

} // namespace fe
