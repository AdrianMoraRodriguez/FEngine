#pragma once

/**
 * @file AmbientOcclusion.h
 * @brief Screen-space ambient occlusion: how much of the sky each pixel can see.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;
struct PassPush;   // push constants of the full-screen passes (AmbientOcclusion.cpp)

/**
 * @brief Darkens the ambient light in creases and under objects.
 *
 * The ambient and sky light reach a surface from every direction, but in a
 * corner, in a crack or on the ground right under a box a good part of those
 * directions is blocked by other geometry, so less light arrives. Direct
 * lights and their shadow maps cannot show this, so it is estimated from what
 * the camera sees: for each pixel, a few points are taken in the half-sphere
 * above its surface, and the more of them lie behind something that is
 * nearer to the camera, the more closed that spot is.
 *
 * ### What it owns
 *   - A depth image of its own, filled by a depth-only prepass of the scene
 *     (SceneRenderer::renderAmbientOcclusion() draws it; this class only opens
 *     and closes the pass). A separate prepass keeps the main pass untouched
 *     and the depth readable without any render-pass dependency.
 *   - Two single-channel images (R8) that the occlusion and its blur ping-pong
 *     between. The final result stays in the first one.
 *   - The descriptor sets that bind the result to mesh.frag (set 3), and a
 *     1 x 1 white image to bind instead when there is no occlusion (previews,
 *     or the effect turned off): white reads as "fully open" everywhere.
 *
 * ### Frame order
 * Outside any render pass, every frame the effect is on:
 * @code
 * ao.resize(extent);
 * ao.beginDepth(cmd);  // ... draw the scene's depth ...  ao.endDepth(cmd);
 * ao.compute(cmd, projection, radius, intensity);
 * @endcode
 * After that resultSet() holds this frame's occlusion; bind it to the mesh
 * pipeline as set 3.
 */
class AmbientOcclusion {
public:
    /// Format of the depth prepass: the same as the offscreen buffer's.
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    /// Format of the occlusion images: one 8-bit channel is plenty.
    static constexpr VkFormat kFormat = VK_FORMAT_R8_UNORM;

    AmbientOcclusion() = default;
    ~AmbientOcclusion() { shutdown(); }

    AmbientOcclusion(const AmbientOcclusion&)            = delete;
    AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

    /**
     * @brief Creates the layouts, pipelines, samplers and the white image.
     *        The size-dependent images come later, with resize().
     * @param shaderDir Directory with fullscreen.vert.spv, ssao.frag.spv and ssao_blur.frag.spv.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, const char* shaderDir);

    /// Frees everything. Safe to call twice. The GPU must be done with it.
    void shutdown();

    /**
     * @brief Makes the images match the render target.
     *
     * Does nothing when the size has not changed. Otherwise it waits for the
     * GPU to be idle and recreates them, which is a short stall while the
     * viewport is being resized and nothing the rest of the time.
     *
     * @return `false` if the extent is empty or a Vulkan call failed; do not
     *         use the result this frame.
     */
    bool resize(VkExtent2D extent);

    /// Opens the depth prepass: prepares the depth image, clears it and sets
    /// the viewport and scissor. Draw with a depth-only pipeline.
    void beginDepth(VkCommandBuffer cmd);

    /// Closes the prepass and makes the depth readable by the shaders.
    void endDepth(VkCommandBuffer cmd);

    /**
     * @brief Computes the occlusion from the depth and blurs it.
     *
     * @param projection The camera's projection, the one the prepass was drawn with.
     * @param radius     How far from a surface (world units) geometry still occludes it.
     * @param intensity  Exponent applied to the result: 1 is the plain estimate,
     *                   more makes the dark parts darker.
     */
    void compute(VkCommandBuffer cmd, const glm::mat4& projection, float radius, float intensity);

    /// @return Layout of the set mesh.frag reads (set 3): one combined image sampler.
    VkDescriptorSetLayout resultLayout() const { return m_resultLayout; }

    /// @return The set with this frame's occlusion. Valid after compute().
    VkDescriptorSet resultSet() const { return m_resultSet; }

    /// @return A set whose single texel is 1: no occlusion anywhere.
    VkDescriptorSet whiteSet() const { return m_whiteSet; }

    /// @return The view of the prepass's depth (nearest-readable, layout
    ///         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL after endDepth()).
    ///         Changes whenever resize() makes new images. The reflections read it too.
    VkImageView depthView() const { return m_depth.view; }

private:
    struct Image {
        VkImage       image      = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView   view       = VK_NULL_HANDLE;
    };

    bool createImage(Image& out, VkFormat format, VkExtent2D extent,
                     VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroyImage(Image& image);
    void destroySizedImages();
    bool createPipeline(VkPipeline& out, const char* shaderDir, const char* fragName);
    bool createWhiteImage();
    void writeSets();

    /// Records a layout change of the whole image (everything waits for everything).
    static void transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to);

    /// Draws a fullscreen triangle into @p target with @p pipeline and @p set.
    void fullscreenPass(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set,
                        const Image& target, const PassPush& push);

    const VulkanContext* m_ctx = nullptr;
    VkExtent2D           m_extent{0, 0};

    VkDescriptorSetLayout m_passLayout   = VK_NULL_HANDLE;   ///< Two samplers: the input and the depth.
    VkDescriptorSetLayout m_resultLayout = VK_NULL_HANDLE;   ///< One sampler: the occlusion.
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssaoPipeline = VK_NULL_HANDLE;
    VkPipeline            m_blurPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool         = VK_NULL_HANDLE;
    VkSampler             m_nearest      = VK_NULL_HANDLE;
    VkSampler             m_linear       = VK_NULL_HANDLE;

    Image m_depth;      ///< The prepass's depth.
    Image m_a;          ///< Raw occlusion; also the final, blurred result.
    Image m_b;          ///< Half-blurred.
    Image m_white;      ///< 1 x 1, always 1.

    VkDescriptorSet m_ssaoSet   = VK_NULL_HANDLE;   ///< Input: white; depth.
    VkDescriptorSet m_blurHSet  = VK_NULL_HANDLE;   ///< Input: A; depth.
    VkDescriptorSet m_blurVSet  = VK_NULL_HANDLE;   ///< Input: B; depth.
    VkDescriptorSet m_resultSet = VK_NULL_HANDLE;   ///< A, for mesh.frag.
    VkDescriptorSet m_whiteSet  = VK_NULL_HANDLE;   ///< The white texel, for mesh.frag.
};

} // namespace fe
