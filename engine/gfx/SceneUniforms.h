#pragma once

/**
 * @file SceneUniforms.h
 * @brief Per-frame scene data (lights) made available to the shaders.
 *
 * Shaders read it through a descriptor set instead of push constants,
 * because it is shared by every draw call of the frame and is larger than
 * what push constants are meant for.
 */

#include "gfx/Renderer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace fe {

class VulkanContext;

/// Most lights the shaders can use at once. Extra lights in a scene are ignored.
constexpr uint32_t kMaxLights = 16;

/// Values of GpuLight::colorType.w.
constexpr float kGpuLightDirectional = 0.0f;
constexpr float kGpuLightPoint       = 1.0f;
constexpr float kGpuLightSpot        = 2.0f;

/**
 * @brief One light as the shader sees it. Mirrors `Light` in mesh.frag.
 *
 * Four `vec4`s, so every member starts on a 16-byte boundary (std140) and
 * the C++ and GLSL layouts agree without any padding on our side.
 */
struct GpuLight {
    /// xyz: world position (point and spot). w: range, where the light fades to nothing.
    glm::vec4 positionRange{0.0f};

    /// rgb: colour already multiplied by the intensity. w: the light type (kGpuLight*).
    glm::vec4 colorType{0.0f};

    /// xyz: direction the light travels (directional and spot). w: unused.
    glm::vec4 direction{0.0f, -1.0f, 0.0f, 0.0f};

    /// Spot only. x: cosine of the outer half-angle (where the light is gone).
    /// y: cosine of the inner half-angle (where it is at full strength).
    /// Cosines, not angles, so the shader compares them with a dot product directly.
    glm::vec4 spotCone{0.0f};
};

/**
 * @brief The scene-wide values the shaders read. Mirrors `SceneData` in mesh.frag.
 *
 * Only the first `lightCount.x` entries of `lights` are valid. The rest of
 * the array is never read by the shader.
 */
struct SceneUniformData {
    /// Light that reaches every surface regardless of its orientation.
    glm::vec4 ambientColor{0.25f, 0.25f, 0.25f, 0.0f};

    /// x: how many entries of `lights` are in use.
    /// y: index of the light that casts shadows, or -1 for none.
    /// zw: padding.
    glm::ivec4 lightCount{0, -1, 0, 0};

    /// World space -> the shadow map's clip space (the shadow light's view-projection).
    glm::mat4 lightViewProj{1.0f};

    /// x: size of one shadow-map texel in texture coordinates (1 / resolution).
    /// y: how far, in world units, the shadow lookup is pushed along the surface normal.
    /// zw: unused.
    glm::vec4 shadowParams{0.0f};

    GpuLight lights[kMaxLights];
};

static_assert(sizeof(GpuLight) == 64,
              "GpuLight must match the std140 struct in mesh.frag");
static_assert(offsetof(SceneUniformData, lightViewProj) == 32,
              "The shadow matrix must start right after the two vec4s");
static_assert(offsetof(SceneUniformData, shadowParams) == 96,
              "The shadow parameters must follow the shadow matrix");
static_assert(offsetof(SceneUniformData, lights) == 112,
              "The light array must start after the shadow data");
static_assert(sizeof(SceneUniformData) == 112 + 64 * kMaxLights,
              "SceneUniformData must match the std140 block in mesh.frag");

/**
 * @brief Owns the uniform buffers and the descriptor set that expose
 *        SceneUniformData to the shaders.
 *
 * There is one buffer and one descriptor set per frame in flight. The GPU
 * may still be reading the buffer of the previous frame while the CPU fills
 * the one of the current frame, so they must not be shared.
 *
 * Usage, once per frame after Renderer::beginFrame():
 * @code
 * uniforms.update(renderer.currentFrameIndex(), data);
 * // ... inside the render pass, with the pipeline bound:
 * pipeline.bindDescriptorSet(cmd, uniforms.set(renderer.currentFrameIndex()));
 * @endcode
 *
 * @note update() must run after Renderer::beginFrame(), which waits for the
 *       GPU to finish the frame that last used this slot. Calling it earlier
 *       could overwrite data the GPU is still reading.
 */
class SceneUniforms {
public:
    SceneUniforms() = default;
    ~SceneUniforms();

    SceneUniforms(const SceneUniforms&)            = delete;
    SceneUniforms& operator=(const SceneUniforms&) = delete;

    /**
     * @brief Creates the descriptor layout, pool, buffers and sets.
     *
     * Set 0 has two bindings: 0 is the uniform buffer with SceneUniformData,
     * 1 is the shadow map. The shadow map never changes, so every set is
     * pointed at it once, here.
     *
     * @param ctx           Initialised Vulkan context.
     * @param shadowView    View of the shadow map (ShadowMap::view()).
     * @param shadowSampler Comparison sampler for it (ShadowMap::sampler()).
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, VkImageView shadowView, VkSampler shadowSampler);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /// @return The layout of set 0. The mesh pipeline layout is built from it.
    VkDescriptorSetLayout layout() const { return m_layout; }

    /**
     * @brief Copies @p data into the buffer of a frame slot.
     * @param frameIndex Renderer::currentFrameIndex().
     */
    void update(uint32_t frameIndex, const SceneUniformData& data);

    /// @return The descriptor set to bind while drawing in the given frame slot.
    VkDescriptorSet set(uint32_t frameIndex) const;

private:
    /// Everything that belongs to one frame slot.
    struct Slot {
        VkBuffer        buffer     = VK_NULL_HANDLE;
        VmaAllocation   allocation = VK_NULL_HANDLE;
        void*           mapped     = nullptr;          ///< Persistently mapped CPU pointer.
        VkDescriptorSet set        = VK_NULL_HANDLE;
    };

    const VulkanContext*  m_ctx    = nullptr;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool   = VK_NULL_HANDLE;
    Slot                  m_slots[Renderer::MAX_FRAMES_IN_FLIGHT];
};

} // namespace fe
