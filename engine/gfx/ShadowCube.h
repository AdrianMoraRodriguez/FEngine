#pragma once

/**
 * @file ShadowCube.h
 * @brief The six depth images a point light's shadows are drawn into, stored
 *        as a cube map, and the sampler that reads them back.
 *
 * A point light shines in every direction, so one view cannot see all that
 * it lights. Instead the scene is drawn six times from the light's position,
 * once towards each of +X, -X, +Y, -Y, +Z and -Z, each with a 90 degree field
 * of view. Together the six squares cover the whole sphere. While shading,
 * the direction from the light to a surface point picks the face and the
 * texel to compare with, exactly like ShadowMap does for a single view.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>

namespace fe {

class VulkanContext;

/**
 * @brief A cube-map depth image used as the shadow map of a point light.
 *
 * Like ShadowMap, it is redrawn every frame and shared by every frame in
 * flight, and begin() / end() must run every frame even when nothing uses it,
 * so the image stays in the layout the shader's descriptor promises.
 *
 * Usage, outside any other render pass:
 * @code
 * cube.begin(cmd);
 * for (uint32_t face = 0; face < ShadowCube::kFaces; ++face) {
 *     cube.beginFace(cmd, face);
 *     // ... draw the shadow casters with that face's view-projection ...
 *     cube.endFace(cmd);
 * }
 * cube.end(cmd);
 * @endcode
 */
class ShadowCube {
public:
    /// Number of faces of a cube.
    static constexpr uint32_t kFaces = 6;

    /// Width and height of each face in texels.
    static constexpr uint32_t kResolution = 1024;

    /// Depth format of the faces.
    static constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;

    ShadowCube() = default;
    ~ShadowCube();

    ShadowCube(const ShadowCube&)            = delete;
    ShadowCube& operator=(const ShadowCube&) = delete;

    /// Creates the image, its views and the sampler. @return `true` on success.
    bool init(const VulkanContext& ctx);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /// Prepares all six faces for drawing. Call once before the faces.
    void begin(VkCommandBuffer cmd);

    /**
     * @brief Opens a depth-only render pass on one face and clears it.
     *
     * Faces follow the Vulkan cube-map layer order: 0 = +X, 1 = -X, 2 = +Y,
     * 3 = -Y, 4 = +Z, 5 = -Z. Also sets the viewport and scissor.
     */
    void beginFace(VkCommandBuffer cmd, uint32_t face);

    /// Closes the pass opened by beginFace().
    void endFace(VkCommandBuffer cmd);

    /// Makes the whole cube readable by fragment shaders. Call after the last face.
    void end(VkCommandBuffer cmd);

    /// @return Cube view of the image, for the descriptor set.
    VkImageView view() const { return m_cubeView; }

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

    VkImageView m_cubeView = VK_NULL_HANDLE;                  ///< All six faces, for sampling.
    std::array<VkImageView, kFaces> m_faceViews{};            ///< One face each, for drawing.
    VkSampler   m_sampler  = VK_NULL_HANDLE;
    VkSampler     m_depthSampler = VK_NULL_HANDLE;
};

} // namespace fe
