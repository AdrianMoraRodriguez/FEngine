#pragma once

/**
 * @file PostProcess.h
 * @brief Turns the HDR scene into the finished picture: bloom, exposure and
 *        tone mapping.
 *
 * ### Why
 * The scene pass draws in linear light that can be far brighter than 1: the
 * sun, an emissive texture, a glint on metal. A screen can only show 0..1, so
 * the light has to be squeezed at the end, and doing it as a separate step
 * (instead of inside every object's shader) allows two things:
 *  - **Bloom**: the very bright parts leak a soft glow into their surroundings,
 *    as light does in a camera lens or an eye.
 *  - **Exposure and a choice of tone mapping curve**, changed live.
 *
 * ### The passes
 * @code
 *   hdrImage ──► downsample ► ► ► ► ►  (6 images, each half the size of the last)
 *                   │  the first one keeps only the light above the threshold
 *                   ▼
 *               upsample and add ◄ ◄ ◄ ◄ ◄   (back up to half size)
 *                   │
 *   hdrImage + glow ┴──► exposure ► tone mapping ► sRGB ──► image()
 * @endcode
 * The downsample is the 13-tap filter and the upsample the 3x3 tent filter of
 * the Call of Duty: Advanced Warfare talk (Jimenez, SIGGRAPH 2014); the first
 * downsample weights its samples by 1 / (1 + brightness) ("Karis average") so
 * that a single super-bright pixel cannot make the whole glow flicker.
 *
 * ### Who draws what
 * The finished image() is display-ready, so the selection outline is drawn
 * over it afterwards (render() calls a function of the caller's for that, inside
 * the same pass) and is not affected by exposure or tone mapping.
 *
 * Main thread only.
 */

#include "core/ToneMapper.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <functional>

namespace fe {

class VulkanContext;
class OffscreenBuffer;

/// What render() does to the picture.
struct PostSettings {
    float      exposure       = 0.0f;                ///< Exposure compensation in stops (EV).
    ToneMapper toneMapper     = ToneMapper::Neutral; ///< The curve into the 0..1 range.
    bool       bloom          = false;               ///< Whether bright parts glow.
    float      bloomIntensity = 0.3f;                ///< Strength of the glow (0 = none).
    float      bloomThreshold = 1.0f;                ///< Linear brightness from which a pixel glows.
};

/**
 * @brief The post-processing step of a frame.
 *
 * One instance serves every OffscreenBuffer: the bloom images are sized for
 * whichever buffer asks with bloom on (the editor viewport), and buffers that
 * ask without bloom (thumbnails, model viewers) only use the final pass.
 */
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess() { shutdown(); }

    PostProcess(const PostProcess&)            = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    /**
     * @brief Creates the pipelines and descriptor sets.
     * @param ctx       Initialised Vulkan context; must outlive this object.
     * @param shaderDir Folder with the compiled `fullscreen.vert.spv`,
     *                  `bloom_down.frag.spv`, `bloom_up.frag.spv` and
     *                  `tonemap.frag.spv`.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, const char* shaderDir);

    /// Destroys everything. Safe to call twice.
    void shutdown();

    /// Push constants of the post-processing shaders (defined in the .cpp).
    struct PassPush;

    /**
     * @brief Makes @p target's finished image() from its HDR image.
     *
     * Call after Renderer::endOffscreenPass(): the HDR image must be in
     * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL. Leaves image() in that layout
     * too, ready for ImGui.
     *
     * @param cmd      Command buffer, outside any render pass.
     * @param target   The buffer the scene was drawn into.
     * @param settings What to do to the picture.
     * @param drawOver Optional. Called once, inside the final pass, after the
     *                 picture is written: draws that go on top of the finished
     *                 image and must not be tone mapped (the selection
     *                 outline). The pass is single-sample with no depth.
     */
    void render(VkCommandBuffer cmd, const OffscreenBuffer& target, const PostSettings& settings,
                const std::function<void(VkCommandBuffer)>& drawOver = {});

private:
    /// Most downsampled images in the bloom chain.
    static constexpr uint32_t kMaxLevels = 6;

    struct Image {
        VkImage       image      = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView   view       = VK_NULL_HANDLE;
        VkExtent2D    extent{};
    };

    bool createPipeline(VkPipeline& out, const char* shaderDir, const char* fragName,
                        VkFormat colorFormat, bool additive);

    /// Makes the bloom images for a target of this size (waits for the GPU if
    /// they exist at another size). @return the number of levels, 0 if the target is too small.
    uint32_t ensureBloomImages(uint32_t width, uint32_t height);
    void     destroyBloomImages();

    /// Records the downsample and upsample passes. The result is bloom image 0.
    void renderBloom(VkCommandBuffer cmd, VkDescriptorSet hdrSet, VkExtent2D hdrExtent,
                     const PostSettings& settings);

    /// Starts one fullscreen pass into @p dstView and draws the triangle. The
    /// caller ends the rendering (the final pass draws more into it).
    void drawPass(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView dstView, VkExtent2D dstExtent,
                  bool loadDst, VkDescriptorSet source, VkDescriptorSet extra, const PassPush& push);

    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to);

    const VulkanContext* m_ctx = nullptr;

    VkSampler             m_linear         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout      = VK_NULL_HANDLE;   ///< One combined image sampler: compatible with OffscreenBuffer::hdrDescriptorSet().
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool           = VK_NULL_HANDLE;

    VkPipeline m_downPipeline      = VK_NULL_HANDLE;
    VkPipeline m_upPipeline        = VK_NULL_HANDLE;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;

    std::array<Image, kMaxLevels>           m_levels{};
    std::array<VkDescriptorSet, kMaxLevels> m_levelSets{};
    uint32_t m_levelCount   = 0;
    uint32_t m_bloomWidth   = 0;   ///< Size of the target the bloom images were made for.
    uint32_t m_bloomHeight  = 0;
};

} // namespace fe
