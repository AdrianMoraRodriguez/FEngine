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
 *
 * ### Anti-aliasing and HDR
 * The scene is drawn with multisampling (kSampleCount samples per pixel) so
 * that the edges of objects are smooth, and in high dynamic range: the colour
 * is linear light that may be far brighter than 1 (the sun, an emissive
 * texture), not yet squeezed into what a screen can show. The multisampled
 * colour and depth images are private to the scene pass; at the end of it the
 * colour is averaged into the single-sample hdrImage().
 *
 * PostProcess then turns hdrImage() into the finished picture: it adds the
 * bloom, applies exposure and tone mapping, and writes the ordinary 8-bit
 * image() that everything else reads (ImGui, the readbacks). Every pipeline
 * that draws into the scene pass must be created with hdrFormat() and
 * kSampleCount samples.
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
     * @param sharedMultisampled If given, the multisampled colour and depth
     *        images and the HDR image of that buffer are used instead of
     *        creating new ones. They only hold the scene pass and the
     *        post-processing that reads it (cleared at the start of the next
     *        picture, never kept), so buffers that are drawn one after
     *        another, like the asset thumbnails, can share one set. It must
     *        have the same size, outlive this buffer, and this buffer cannot
     *        be resized.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, uint32_t width, uint32_t height,
              const OffscreenBuffer* sharedMultisampled = nullptr);

    /**
     * @brief Destroys and recreates both images at a new size.
     *
     * Does nothing if the size is unchanged. Waits for the device to be idle
     * before destroying the old images.
     */
    void resize(uint32_t width, uint32_t height);

    /// Destroys all Vulkan objects. Safe to call if init() was never called.
    void shutdown();

    /// @return The finished colour image: PostProcess writes it, and it is what
    ///         ImGui samples. 8 bits per channel, display-ready (sRGB-encoded),
    ///         one sample per pixel.
    VkImage image() const { return m_image; }

    /// @return View of the finished colour image (the post-processing target).
    VkImageView imageView() const { return m_imageView; }

    /// @return The scene as linear HDR light: the scene pass resolves into it.
    ///         One sample per pixel; shared buffers return the owner's.
    VkImage hdrImage() const { return m_hdrImage; }

    /// @return View of the HDR image (the resolve target).
    VkImageView hdrView() const { return m_hdrView; }

    /**
     * @brief Descriptor set that samples the HDR image.
     *
     * One combined image sampler at binding 0, visible to the fragment stage,
     * so any descriptor set layout defined the same way can bind it. It does
     * not come from ImGui (see m_hdrPool). The
     * image is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once the scene
     * pass has ended.
     */
    VkDescriptorSet hdrDescriptorSet() const { return m_hdrDescSet; }

    /// @return The multisampled colour image the scene pass draws into.
    VkImage msaaImage() const { return m_msaaImage; }

    /// @return View of the multisampled colour image (colour attachment).
    VkImageView msaaView() const { return m_msaaView; }

    /// @return The multisampled depth image used for depth testing during the scene pass.
    VkImage depthImage() const { return m_depthImage; }

    /// @return View of the depth image (depth attachment).
    VkImageView depthView() const { return m_depthView; }

    /// @return ImGui descriptor set wrapping the colour image as a texture.
    VkDescriptorSet imguiDescriptorSet() const { return m_imguiDescSet; }

    /// @return Image width in pixels.
    uint32_t width() const { return m_width; }

    /// @return Image height in pixels.
    uint32_t height() const { return m_height; }

    /// @return Format of the finished image (always VK_FORMAT_R8G8B8A8_UNORM).
    static constexpr VkFormat format() { return VK_FORMAT_R8G8B8A8_UNORM; }

    /// @return Format of what the scene pass draws (always VK_FORMAT_R16G16B16A16_SFLOAT).
    static constexpr VkFormat hdrFormat() { return VK_FORMAT_R16G16B16A16_SFLOAT; }

    /// @return Depth format (always VK_FORMAT_D32_SFLOAT).
    static constexpr VkFormat depthFormat() { return VK_FORMAT_D32_SFLOAT; }

    /// Samples per pixel of the scene pass. Vulkan guarantees 4 for colour and depth.
    static constexpr VkSampleCountFlagBits kSampleCount = VK_SAMPLE_COUNT_4_BIT;

private:
    bool create(uint32_t width, uint32_t height);
    void destroy();

    const VulkanContext*   m_ctx    = nullptr;
    const OffscreenBuffer* m_shared = nullptr;   ///< Whose multisampled images are borrowed, or null.

    VkImage         m_image        = VK_NULL_HANDLE;
    VmaAllocation   m_allocation   = VK_NULL_HANDLE;
    VkImageView     m_imageView    = VK_NULL_HANDLE;
    VkSampler       m_sampler      = VK_NULL_HANDLE;
    VkDescriptorSet m_imguiDescSet = VK_NULL_HANDLE;

    VkImage         m_hdrImage      = VK_NULL_HANDLE;
    VmaAllocation   m_hdrAllocation = VK_NULL_HANDLE;
    VkImageView     m_hdrView       = VK_NULL_HANDLE;
    VkDescriptorSet m_hdrDescSet    = VK_NULL_HANDLE;
    /// The HDR set is made here, not through ImGui: the layout of the sets ImGui
    /// hands out changes between its versions (a combined image sampler, or a
    /// bare sampled image), and PostProcess needs it to stay what it is.
    VkDescriptorPool      m_hdrPool   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_hdrLayout = VK_NULL_HANDLE;

    VkImage       m_msaaImage       = VK_NULL_HANDLE;
    VmaAllocation m_msaaAllocation  = VK_NULL_HANDLE;
    VkImageView   m_msaaView        = VK_NULL_HANDLE;

    VkImage       m_depthImage      = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView   m_depthView       = VK_NULL_HANDLE;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fe
