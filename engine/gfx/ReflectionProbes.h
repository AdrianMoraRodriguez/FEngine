#pragma once

/**
 * @file ReflectionProbes.h
 * @brief The cube maps that reflection probes photograph, and the pass that blurs them.
 */

#include "gfx/SceneUniforms.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <vector>

namespace fe {

class VulkanContext;

/**
 * @brief Storage and filtering for the scene's reflection probes.
 *
 * Every probe owns six layers of one image (kMaxReflectionProbes probes in
 * all), seen by the shaders as a cube map of its own: an array of cube
 * samplers, indexed with constants (no dynamic indexing or cube-array feature
 * is needed). Each layer has a chain of mip levels: level 0 is the sharp
 * photograph, and every level after it is blurrier, filtered the way a rougher
 * surface would blur a reflection (level = roughness * last level, the same
 * convention as Environment).
 *
 * ### Making one probe
 * The caller (SceneRenderer) renders the scene into a square target six times,
 * once per face, and after each one hands the picture over:
 * @code
 * probes.beginCapture(cmd);
 * for (uint32_t face = 0; face < 6; ++face) {
 *     // ... render the scene with ReflectionProbes::faceView(face, position) ...
 *     probes.captureFace(cmd, target.hdrImage(), face);
 * }
 * probes.filter(cmd, probeIndex);   // blurs every mip level into the probe's layers
 * @endcode
 * The six pictures first go to a scratch cube map (the "photograph") with its
 * own mip chain; filter() then reads that and writes the probe's layers.
 *
 * ### Descriptor set
 * set() holds the probes' cube maps, an array of combined image samplers, for set 5
 * of the mesh pipeline. It never changes, and every layer is always in a
 * readable layout between filter() calls, so any frame can bind it. Layers of
 * probes that were never photographed are black.
 *
 * @note Capture and filtering are recorded into the frame's command buffer
 *       and execute in order with the rest of the frame. Barriers are wide
 *       (everything before waits for everything after) because a bake is rare.
 */
class ReflectionProbes {
public:
    /// Width and height of one face, in texels.
    static constexpr uint32_t kResolution = 128;

    /// Mip levels of a face: 128, 64, ... 1.
    static constexpr uint32_t kLevels = 8;

    /// Format of the cube maps (linear HDR light).
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    /// The cube faces, in the order of Vulkan's cube maps: +X, -X, +Y, -Y, +Z, -Z.
    static constexpr uint32_t kFaces = 6;

    ReflectionProbes() = default;
    ~ReflectionProbes() { shutdown(); }

    ReflectionProbes(const ReflectionProbes&)            = delete;
    ReflectionProbes& operator=(const ReflectionProbes&) = delete;

    /// Creates the images, the filtering pipeline and the descriptor set.
    /// @param shaderDir Folder with the compiled shaders.
    /// @return `true` on success.
    bool init(const VulkanContext& ctx, const char* shaderDir);

    /// Frees everything. Safe to call twice. The GPU must be done with it.
    void shutdown();

    /// @return Layout of the probe set (set 5 of the mesh pipeline).
    VkDescriptorSetLayout layout() const { return m_layout; }

    /// @return The set holding every probe's cube map (binding 0, kMaxReflectionProbes samplers).
    ///         Always safe to bind.
    VkDescriptorSet set() const { return m_probeSet; }

    /// Starts photographing a probe: readies the scratch cube map for the six faces.
    void beginCapture(VkCommandBuffer cmd);

    /**
     * @brief Copies a rendered picture into one face of the scratch cube map.
     * @param hdrImage A kResolution x kResolution picture in
     *        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL with TRANSFER_SRC usage
     *        (OffscreenBuffer::hdrImage()); it is left in the same layout.
     *        Outside any render pass.
     * @param face     0..5, as in the order above.
     *
     * The picture must have been rendered with faceView() and
     * faceProjection(): the copy flips it sideways, because cube maps are
     * stored mirrored compared with how a camera sees.
     */
    void captureFace(VkCommandBuffer cmd, VkImage hdrImage, uint32_t face);

    /**
     * @brief Blurs the six photographs into every mip level of one probe.
     * @param probe Index of the probe, below kMaxReflectionProbes.
     * Call after the six captureFace() calls, outside any render pass.
     */
    void filter(VkCommandBuffer cmd, uint32_t probe);

    /// @return The view matrix of a cube face for a camera standing at @p position.
    static glm::mat4 faceView(uint32_t face, const glm::vec3& position);

    /// @return The projection of a cube face: 90 degrees, square, with the Vulkan Y flip.
    static glm::mat4 faceProjection(float nearPlane, float farPlane);

private:
    struct Image {
        VkImage       image      = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView   view       = VK_NULL_HANDLE;   ///< The view the shaders read through.
    };

    bool createImage(Image& out, uint32_t layers, VkImageUsageFlags usage, bool makeView);
    void destroyImage(Image& image);
    bool createPipeline(const char* shaderDir);
    bool clearProbes();

    static void barrier(VkCommandBuffer cmd, VkImage image, uint32_t baseLevel, uint32_t levelCount,
                        uint32_t baseLayer, uint32_t layerCount, VkImageLayout from, VkImageLayout to);

    const VulkanContext* m_ctx = nullptr;

    VkDescriptorSetLayout m_layout         = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool           = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;

    VkDescriptorSet m_probeSet  = VK_NULL_HANDLE;   ///< The cube map array.
    VkDescriptorSet m_sourceSet = VK_NULL_HANDLE;   ///< The scratch cube map, for the filtering pass.

    Image m_probes;   ///< kMaxReflectionProbes cubes, kLevels mips each (no view of its own).
    VkImageView m_cubeViews[kMaxReflectionProbes] = {};   ///< One cube view per probe.
    Image m_source;   ///< The photograph of the probe being made.

    /// One 2D view per (probe, face, level), to draw into.
    std::vector<VkImageView> m_targetViews;
};

} // namespace fe
