#pragma once

/**
 * @file SdfAtlas.h
 * @brief One 3D texture that holds the distance fields of all the meshes.
 *
 * A shader cannot choose among many separate textures without extra device
 * features, so the fields are stacked in a single volume instead: each mesh
 * gets a brick of it, placed one after another along z. The brick of a mesh
 * is found from the z where it starts and the size of its field (see
 * SdfVolume); a shader keeps its lookups inside that brick.
 */

#include "gfx/MeshDistanceField.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief The shared 3D texture of mesh distance fields (16-bit floats).
 *
 * The image stays in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sampled with
 * linear filtering. Empty space holds a large positive distance.
 */
class SdfAtlas {
public:
    /// Cells along z in total, shared by all the bricks (the least every device guarantees).
    static constexpr uint32_t kDepth  = 2048;

    /// Returned by add() when the field does not fit.
    static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

    SdfAtlas() = default;
    ~SdfAtlas() { shutdown(); }

    SdfAtlas(const SdfAtlas&)            = delete;
    SdfAtlas& operator=(const SdfAtlas&) = delete;

    /**
     * @brief Creates the texture, empty.
     *
     * @param ctx   Initialised Vulkan context.
     * @param brick Cells along x and y of the image: the largest field that fits
     *              (its longest side). The image is brick x brick x kDepth cells.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, uint32_t brick = 64);

    /// @return Cells along x and y of the image (the largest field that fits).
    uint32_t brick() const { return m_brick; }

    /// Frees the texture. Safe to call more than once.
    void shutdown();

    /**
     * @brief Copies a field into the next free brick.
     *
     * Submits its own work and waits for it, so it must not be called while a
     * frame is being recorded.
     *
     * @return The z where the brick starts, or kNoSlot if the field is
     *         larger than a brick or the atlas is full (logged).
     */
    uint32_t add(const SdfVolume& volume);

    /// @return Cells of z already given out.
    uint32_t used() const { return m_nextZ; }

    VkImageView view()    const { return m_view; }
    VkSampler   sampler() const { return m_sampler; }

private:
    const VulkanContext* m_ctx = nullptr;

    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_view       = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;

    uint32_t m_brick = 64;   ///< Cells along x and y of the image.
    uint32_t m_nextZ = 0;
};

} // namespace fe
