#pragma once

/**
 * @file ScreenReflections.h
 * @brief What mesh.frag needs to reflect the rest of the scene: the depth the
 *        camera sees, and a copy of the previous frame's picture.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace fe {

class VulkanContext;

/**
 * @brief Screen-space reflections (SSR): shiny surfaces mirror what is on screen.
 *
 * Without this a metal ball or a wet floor can only reflect the sky (the
 * environment map), however much stands next to it. With it, a ray is sent
 * from each shiny pixel along its reflection; mesh.frag walks the ray through
 * the depth image until it passes behind a surface, and takes the colour of
 * that surface.
 *
 * ### Where the colour comes from
 * The picture of the *current* frame does not exist yet while its objects are
 * being shaded (this is a forward renderer: they are drawn in one pass). So the
 * colour is read from the **previous frame's** picture, found again with the
 * previous camera's matrix. When the camera or the objects move, the reflection
 * trails one frame behind, which is hardly visible; when nothing moves it is exact.
 * The same trick is used by Unreal's forward renderer.
 *
 * ### What it owns
 *   - A copy of the last frame's HDR picture, with a chain of ever blurrier
 *     mip levels: a rough surface reads a blurry level, a mirror the sharp one.
 *   - Descriptor set 4 of the mesh pipeline: the depth (that of AmbientOcclusion's
 *     prepass, which this class does not own) and that copy.
 *   - An "empty" set (nothing to reflect) for previews, and for the frames
 *     before the first copy exists.
 *
 * ### Frame order
 * @code
 * ao.resize(extent) ...           // makes the depth
 * reflections.resize(extent, ao.depthView());
 * // ... depth prepass, scene pass: bind liveSet() if ready(), else emptySet() ...
 * reflections.capture(cmd, hdrImage, extent);   // after the scene pass
 * @endcode
 */
class ScreenReflections {
public:
    /// Format of the copy: the same as the HDR picture.
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    /// Most levels in the copy's mip chain.
    static constexpr uint32_t kMaxLevels = 8;

    ScreenReflections() = default;
    ~ScreenReflections() { shutdown(); }

    ScreenReflections(const ScreenReflections&)            = delete;
    ScreenReflections& operator=(const ScreenReflections&) = delete;

    /// Creates the layout, samplers, pool and the empty images and set.
    /// @return `true` on success.
    bool init(const VulkanContext& ctx);

    /// Frees everything. Safe to call twice. The GPU must be done with it.
    void shutdown();

    /**
     * @brief Makes the copy match the render target and points the live set at
     *        the current depth.
     *
     * Cheap when nothing changed. When the size changes it waits for the GPU
     * to be idle (a short stall while the viewport is being resized), and the
     * copy starts empty: ready() is `false` until the next capture().
     *
     * @param depthView The depth the reflections march through (AmbientOcclusion::depthView()).
     * @return `false` if the extent is empty or a Vulkan call failed.
     */
    bool resize(VkExtent2D extent, VkImageView depthView);

    /// @return `true` once a picture has been captured at the current size.
    bool ready() const { return m_valid; }

    /// Forgets the copy (the next frame uses emptySet() until a new capture).
    void invalidate() { m_valid = false; }

    /**
     * @brief Copies @p hdrImage into the history and builds its mip chain.
     *
     * @param hdrImage Finished HDR picture of this frame, in
     *        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL with TRANSFER_SRC usage;
     *        it is left in the same layout. Outside any render pass.
     * @param extent   Its size, the same as resize() was given.
     */
    void capture(VkCommandBuffer cmd, VkImage hdrImage, VkExtent2D extent);

    /// @return Highest mip level of the copy (levels - 1): what the roughest reflection reads.
    float maxMip() const { return m_levels > 0 ? static_cast<float>(m_levels - 1) : 0.0f; }

    /// @return Layout of set 4: the depth and the previous picture, two combined image samplers.
    VkDescriptorSetLayout layout() const { return m_layout; }

    /// @return The set with this frame's depth and the previous picture. Use when ready().
    VkDescriptorSet liveSet() const { return m_liveSet; }

    /// @return A set with nothing to reflect (far depth everywhere, black picture).
    VkDescriptorSet emptySet() const { return m_emptySet; }

private:
    struct Image {
        VkImage       image      = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView   view       = VK_NULL_HANDLE;
    };

    bool createImage(Image& out, VkFormat format, VkExtent2D extent, uint32_t levels, VkImageUsageFlags usage);
    void destroyImage(Image& image);
    bool createEmptyImages();
    void writeLiveSet(VkImageView depthView);

    static void barrier(VkCommandBuffer cmd, VkImage image, uint32_t firstLevel, uint32_t levelCount,
                        VkImageLayout from, VkImageLayout to);

    const VulkanContext* m_ctx = nullptr;

    VkDescriptorSetLayout m_layout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool    = VK_NULL_HANDLE;
    VkSampler             m_nearest = VK_NULL_HANDLE;   ///< For the depth.
    VkSampler             m_linear  = VK_NULL_HANDLE;   ///< For the picture, between its mip levels too.

    VkDescriptorSet m_liveSet  = VK_NULL_HANDLE;
    VkDescriptorSet m_emptySet = VK_NULL_HANDLE;

    Image m_history;
    Image m_emptyDepth;     ///< 1 x 1, 1.0 (far): a ray never hits it.
    Image m_emptyColour;    ///< 1 x 1, black.

    VkExtent2D  m_extent{0, 0};
    uint32_t    m_levels    = 0;
    bool        m_valid     = false;
    VkImageView m_depthView = VK_NULL_HANDLE;   ///< What the live set currently points at.
};

} // namespace fe
