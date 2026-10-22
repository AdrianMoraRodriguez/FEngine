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

#include <array>
#include <cstddef>
#include <cstdint>

namespace fe {

class VulkanContext;

/// Most lights the shaders can use at once. Extra lights in a scene are ignored.
constexpr uint32_t kMaxLights = 16;

/// Most lights that can cast shadows at once. Each one owns a shadow map
/// (see ShadowMap), so this is also the number of shadow maps in memory.
constexpr uint32_t kMaxShadowCasters = 4;

/// Most point lights that can cast shadows at once. A point light needs a
/// whole cube of six shadow maps (see ShadowCube), so this is kept small.
constexpr uint32_t kMaxPointShadowCasters = 2;

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

    /// x: spot only, cosine of the outer half-angle (where the light is gone).
    /// y: spot only, cosine of the inner half-angle (where it is at full strength).
    /// Cosines, not angles, so the shader compares them with a dot product directly.
    /// z: which shadow map this light uses, or -1 if it casts no shadows. A
    ///    point light counts in its own list of cubes (0 .. kMaxPointShadowCasters - 1);
    ///    directional and spot lights in the list of flat maps (0 .. kMaxShadowCasters - 1).
    /// w: unused.
    glm::vec4 spotCone{0.0f, 0.0f, -1.0f, 0.0f};
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

    /// x: how many entries of `lights` are in use. yzw: padding.
    glm::ivec4 lightCount{0};

    /// For each shadow map: world space -> that map's clip space (the
    /// shadow-casting light's view-projection).
    glm::mat4 shadowViewProj[kMaxShadowCasters];

    /// For each shadow map.
    /// x: size of one texel in texture coordinates (1 / resolution).
    /// y: how far the shadow lookup is pushed along the surface normal, in
    ///    world units (directional) or in world units per unit of distance
    ///    to the light (spot).
    /// z: 1 if y must be multiplied by the distance to the light, else 0.
    /// w: unused.
    glm::vec4 shadowParams[kMaxShadowCasters];

    /// For the point lights' shadow cubes, which all share these values.
    /// x: size of one texel on a cube face, per unit of distance to the light.
    /// y: how far the shadow lookup is pushed along the surface normal, per unit of distance.
    /// z: near plane of every face's projection. w: unused.
    glm::vec4 pointShadowParams{0.0f};

    GpuLight lights[kMaxLights];
};

static_assert(sizeof(GpuLight) == 64,
              "GpuLight must match the std140 struct in mesh.frag");
static_assert(offsetof(SceneUniformData, shadowViewProj) == 32,
              "The shadow matrices must start right after the two vec4s");
static_assert(offsetof(SceneUniformData, shadowParams) == 32 + 64 * kMaxShadowCasters,
              "The shadow parameters must follow the shadow matrices");
static_assert(offsetof(SceneUniformData, pointShadowParams) == 32 + 80 * kMaxShadowCasters,
              "The cube shadow parameters must follow the flat shadow data");
static_assert(offsetof(SceneUniformData, lights) == 48 + 80 * kMaxShadowCasters,
              "The light array must start after the shadow data");
static_assert(sizeof(SceneUniformData) == 48 + 80 * kMaxShadowCasters + 64 * kMaxLights,
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
     * Set 0 has three bindings: 0 is the uniform buffer with SceneUniformData,
     * 1 is an array with the flat shadow maps and 2 an array with the shadow
     * cubes. The shadow maps never change, so every set is pointed at them
     * once, here.
     *
     * @param ctx            Initialised Vulkan context.
     * @param shadowViews    View of each flat shadow map (ShadowMap::view()).
     * @param shadowSamplers Comparison sampler of each (ShadowMap::sampler()).
     * @param cubeViews      View of each shadow cube (ShadowCube::view()).
     * @param cubeSamplers   Comparison sampler of each (ShadowCube::sampler()).
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx,
              const std::array<VkImageView, kMaxShadowCasters>& shadowViews,
              const std::array<VkSampler,   kMaxShadowCasters>& shadowSamplers,
              const std::array<VkImageView, kMaxPointShadowCasters>& cubeViews,
              const std::array<VkSampler,   kMaxPointShadowCasters>& cubeSamplers);

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
