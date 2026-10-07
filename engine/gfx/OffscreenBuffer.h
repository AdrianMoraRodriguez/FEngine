#pragma once

/**
 * @file OffscreenBuffer.h
 * @brief An offscreen colour image used as the scene viewport render target.
 *
 * The editor renders the 3D scene into this image instead of directly into
 * the swapchain. ImGui then samples it as a texture inside the Viewport panel,
 * which allows the panel to be any size and to be moved or resized freely.
 *
 * Lifetime:
 *   - Created once at editor startup.
 *   - Resized when the Viewport panel changes size.
 *   - Destroyed at shutdown.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief A GPU image suitable for use as a colour render target and
 *        subsequently sampled by ImGui as a texture.
 */
class OffscreenBuffer {
public:
    OffscreenBuffer() = default;
    ~OffscreenBuffer();

    OffscreenBuffer(const OffscreenBuffer&)            = delete;
    OffscreenBuffer& operator=(const OffscreenBuffer&) = delete;

    /**
     * @brief Allocates the image and creates all required Vulkan objects.
     *
     * @param ctx    Initialised Vulkan context (provides device and allocator).
     * @param width  Initial width in pixels.
     * @param height Initial height in pixels.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, uint32_t width, uint32_t height);

    /**
     * @brief Destroys and recreates the image at a new size.
     *
     * Called when the Viewport panel is resized. Waits for the device to
     * be idle before destroying the old image.
     *
     * @param width  New width in pixels.
     * @param height New height in pixels.
     */
    void resize(uint32_t width, uint32_t height);

    /// Destroys all Vulkan objects. Safe to call if init() was never called.
    void shutdown();

    /// @return The colour image written to by the renderer each frame.
    VkImage     image()     const { return m_image; }

    /// @return Image view used as a render attachment and as an ImGui texture.
    VkImageView imageView() const { return m_imageView; }

    /**
     * @return The ImGui descriptor set that wraps this image as a texture.
     *
     * Pass this to ImGui::Image() to display the scene in the Viewport panel.
     * Valid only after init() succeeds.
     */
    VkDescriptorSet imguiDescriptorSet() const { return m_imguiDescSet; }

    /// @return Image width in pixels.
    uint32_t width()  const { return m_width; }

    /// @return Image height in pixels.
    uint32_t height() const { return m_height; }

    /// @return The colour format of the image (always VK_FORMAT_R8G8B8A8_UNORM).
    static constexpr VkFormat format() { return VK_FORMAT_R8G8B8A8_UNORM; }

private:
    bool create(uint32_t width, uint32_t height);
    void destroy();

    const VulkanContext* m_ctx = nullptr;

    VkImage        m_image      = VK_NULL_HANDLE;
    VmaAllocation  m_allocation = VK_NULL_HANDLE;
    VkImageView    m_imageView  = VK_NULL_HANDLE;
    VkSampler      m_sampler    = VK_NULL_HANDLE;
    VkDescriptorSet m_imguiDescSet = VK_NULL_HANDLE;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fe
