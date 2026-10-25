#pragma once

/**
 * @file Texture.h
 * @brief A 2D image in GPU memory, with mipmaps, ready to be sampled by shaders.
 */

#include "gfx/VulkanContext.h"
#include "import/ImageLoader.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace fe {

/**
 * @brief An image uploaded to the GPU, plus the view and sampler to read it.
 *
 * ### What happens in create()
 *  1. The pixels are copied into a CPU-visible *staging* buffer.
 *  2. A GPU-only `VkImage` is created. GPU-only memory is much faster to
 *     read from a shader, but the CPU cannot write to it directly.
 *  3. A one-time command buffer copies staging -> image, then builds the
 *     mip chain: each level is a half-size, filtered copy of the previous
 *     one (`vkCmdBlitImage`). Distant surfaces read the small levels, which
 *     avoids shimmering and is cheaper for the GPU's caches.
 *  4. The image ends in `SHADER_READ_ONLY_OPTIMAL` and the staging buffer is
 *     freed.
 *
 * ### Colour space
 * The caller chooses the format. Colour images (albedo, emissive) are
 * authored in sRGB, so they use `VK_FORMAT_R8G8B8A8_SRGB`: the GPU converts
 * to linear on every read, *before* filtering, which is the right order.
 * Images that hold data (normals, roughness, ...) are not colours and must
 * use `VK_FORMAT_R8G8B8A8_UNORM`.
 *
 * The upload blocks until the GPU has finished. Call it from the thread that
 * owns the Vulkan queues, not from an import thread.
 */
class Texture {
public:
    Texture() = default;
    ~Texture() { destroy(); }

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept  { moveFrom(other); }
    Texture& operator=(Texture&& other) noexcept {
        if (this != &other) { destroy(); moveFrom(other); }
        return *this;
    }

    /**
     * @brief Uploads @p image and creates its view and sampler.
     *
     * @param ctx            Vulkan context; must outlive the texture.
     * @param image          Decoded pixels; must not be empty.
     * @param format         `VK_FORMAT_R8G8B8A8_SRGB` for colours or
     *                       `VK_FORMAT_R8G8B8A8_UNORM` for data.
     * @param generateMips   Build the mip chain. Ignored (one level) if the
     *                       GPU cannot filter-blit this format.
     * @param maxAnisotropy  1 disables anisotropic filtering. Values above 1
     *                       need `samplerAnisotropy` to have been enabled when
     *                       the device was created; they are clamped to the
     *                       device limit.
     * @return `true` on success. On failure nothing is left allocated.
     */
    bool create(const VulkanContext& ctx, const ImageData& image,
                VkFormat format, bool generateMips = true,
                float maxAnisotropy = 1.0f);

    /// Frees the GPU objects. Safe to call twice. The GPU must no longer use the texture.
    void destroy();

    bool        valid()     const { return m_view != VK_NULL_HANDLE; }
    VkImageView view()      const { return m_view; }
    VkSampler   sampler()   const { return m_sampler; }
    uint32_t    width()     const { return m_width; }
    uint32_t    height()    const { return m_height; }
    uint32_t    mipLevels() const { return m_mipLevels; }

    /// Ready-made descriptor data for a combined image sampler binding.
    VkDescriptorImageInfo descriptorInfo() const {
        return {m_sampler, m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

private:
    void moveFrom(Texture& other) noexcept;

    const VulkanContext* m_ctx        = nullptr;
    VkImage              m_image      = VK_NULL_HANDLE;
    VmaAllocation        m_allocation = nullptr;
    VkImageView          m_view       = VK_NULL_HANDLE;
    VkSampler            m_sampler    = VK_NULL_HANDLE;
    uint32_t             m_width      = 0;
    uint32_t             m_height     = 0;
    uint32_t             m_mipLevels  = 1;
};

} // namespace fe
