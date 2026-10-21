#pragma once

/**
 * @file OffscreenBuffer.h
 * @brief Offscreen colour + depth render target for the scene viewport.
 *
 * The editor renders the 3D scene into this buffer instead of directly into
 * the swapchain. ImGui then samples the colour image as a texture inside the
 * Viewport panel, so the panel can be any size and be moved or resized freely.
 *
 * The depth image is owned here too because its lifetime and size must always
 * match the colour image.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief A colour image (sampled by ImGui) plus a matching depth image.
 */
class OffscreenBuffer {
public:
    OffscreenBuffer() = default;
    ~OffscreenBuffer();

    OffscreenBuffer(const OffscreenBuffer&)            = delete;
    OffscreenBuffer& operator=(const OffscreenBuffer&) = delete;

    /**
     * @brief Allocates the images and creates all required Vulkan objects.
     *
     * @param ctx    Initialised Vulkan context (provides device and allocator).
     * @param width  Initial width in pixels.
     * @param height Initial height in pixels.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, uint32_t width, uint32_t height);

    /**
     * @brief Destroys and recreates both images at a new size.
     *
     * Does nothing if the size is unchanged. Waits for the device to be idle
     * before destroying the old images.
     */
    void resize(uint32_t width, uint32_t height);

    /// Destroys all Vulkan objects. Safe to call if init() was never called.
    void shutdown();

    /// @return The colour image written to by the scene pass.
    VkImage image() const { return m_image; }

    /// @return View of the colour image (render attachment).
    VkImageView imageView() const { return m_imageView; }

    /// @return The depth image used for depth testing during the scene pass.
    VkImage depthImage() const { return m_depthImage; }

    /// @return View of the depth image (depth attachment).
    VkImageView depthView() const { return m_depthView; }

    /// @return ImGui descriptor set wrapping the colour image as a texture.
    VkDescriptorSet imguiDescriptorSet() const { return m_imguiDescSet; }

    /// @return Image width in pixels.
    uint32_t width() const { return m_width; }

    /// @return Image height in pixels.
    uint32_t height() const { return m_height; }

    /// @return Colour format (always VK_FORMAT_R8G8B8A8_UNORM).
    static constexpr VkFormat format() { return VK_FORMAT_R8G8B8A8_UNORM; }

    /// @return Depth format (always VK_FORMAT_D32_SFLOAT).
    static constexpr VkFormat depthFormat() { return VK_FORMAT_D32_SFLOAT; }

private:
    bool create(uint32_t width, uint32_t height);
    void destroy();

    const VulkanContext* m_ctx = nullptr;

    VkImage         m_image        = VK_NULL_HANDLE;
    VmaAllocation   m_allocation   = VK_NULL_HANDLE;
    VkImageView     m_imageView    = VK_NULL_HANDLE;
    VkSampler       m_sampler      = VK_NULL_HANDLE;
    VkDescriptorSet m_imguiDescSet = VK_NULL_HANDLE;

    VkImage       m_depthImage      = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView   m_depthView       = VK_NULL_HANDLE;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fe
