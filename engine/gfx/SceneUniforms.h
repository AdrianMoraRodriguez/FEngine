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

#include <algorithm>
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

/// Number of cascades of the sun's cascaded shadow map (see ShadowCascades).
constexpr uint32_t kShadowCascades = 4;

/// Most reflection probes the scene can use at once (see ReflectionProbes).
constexpr uint32_t kMaxReflectionProbes = 8;

/// Number of levels of the global distance field (GlobalDistanceField::kLevels).
constexpr uint32_t kSdfLevels = 3;

/// Values of SceneUniformData::sdfParams.x: which lights trace the distance field for their shadows.
constexpr float kSdfShadowsOff        = 0.0f;   ///< None: only shadow maps.
constexpr float kSdfShadowsFill       = 1.0f;   ///< Where the shadow maps do not reach: past the sun's cascades, and lights without a map.
constexpr float kSdfShadowsEverywhere = 2.0f;   ///< Every light that casts, instead of its shadow map.

/// Values of GpuLight::colorType.w.
constexpr float kGpuLightDirectional = 0.0f;
constexpr float kGpuLightPoint       = 1.0f;
constexpr float kGpuLightSpot        = 2.0f;

/**
 * @brief One light as the shader sees it. Mirrors `Light` in mesh.frag.
 *
 * Five `vec4`s, so every member starts on a 16-byte boundary (std140) and
 * the C++ and GLSL layouts agree without any padding on our side.
 */
struct GpuLight {
    /// xyz: world position (point and spot). w: range, where the light fades to nothing.
    glm::vec4 positionRange{0.0f};

    /// rgb: colour already multiplied by the intensity. w: the light type (kGpuLight*).
    glm::vec4 colorType{0.0f};

    /// xyz: direction the light travels (directional and spot).
    /// w: size of the light, which sets how wide the soft edge of its shadows
    ///    grows: a radius in world units for point and spot lights, and for a
    ///    directional light the tangent of its angular radius.
    glm::vec4 direction{0.0f, -1.0f, 0.0f, 0.0f};

    /// x: spot only, cosine of the outer half-angle (where the light is gone).
    /// y: spot only, cosine of the inner half-angle (where it is at full strength).
    /// Cosines, not angles, so the shader compares them with a dot product directly.
    /// z: which shadow map this light uses, or -1 if it casts no shadows. A
    ///    point light counts in its own list of cubes (0 .. kMaxPointShadowCasters - 1);
    ///    directional and spot lights in the list of flat maps (0 .. kMaxShadowCasters - 1).
    /// w: 1 for the one directional light whose shadows are cascaded (it then
    ///    uses the cascades instead of a flat map, and z is 0), else 0.
    glm::vec4 spotCone{0.0f, 0.0f, -1.0f, 0.0f};

    /// x: 1 if the light casts shadows at all, whether or not it got a shadow
    /// map. The distance field shadows (see sdfParams) read it to know which
    /// lights to trace. yzw: unused.
    glm::vec4 extra{0.0f};
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
    /// x: size of one texel in world units (per unit of distance to the light if z is 1).
    /// y: how far the shadow lookup is pushed along the surface normal, same units as x.
    /// z: 1 if the map is a perspective view (spot light), whose x and y must be
    ///    multiplied by the distance to the light; 0 if orthographic (directional).
    /// w: far plane of the map's projection. Its near plane is a constant
    ///    (0 when orthographic, kSpotNear for a spot light).
    glm::vec4 shadowParams[kMaxShadowCasters];

    /// For the point lights' shadow cubes, which all share these values.
    /// x: size of one texel on a cube face, per unit of distance to the light.
    /// y: how far the shadow lookup is pushed along the surface normal, per unit of distance.
    /// z: near plane of every face's projection. w: unused.
    glm::vec4 pointShadowParams{0.0f};

    GpuLight lights[kMaxLights];

    /// For each cascade of the sun's shadow: world space -> that cascade's clip space.
    glm::mat4 cascadeViewProj[kShadowCascades];

    /// For each cascade, same meaning as shadowParams (z is always 0: orthographic).
    glm::vec4 cascadeParams[kShadowCascades];

    /// Distance from the camera, along its view direction, at which each
    /// cascade ends. Beyond the last one there are no shadows.
    glm::vec4 cascadeSplits{0.0f};

    /// xyz: the direction the camera looks along. w: such that
    /// dot(xyz, point) + w is the point's distance in front of the camera.
    glm::vec4 cameraForward{0.0f, 0.0f, -1.0f, 0.0f};

    /// w = 1: xyz is the camera's position in the world. w = 0: the camera is
    /// treated as infinitely far (the previews) and xyz is the unit vector
    /// from the scene towards it. The specular highlights need it.
    glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};

    /// x: how bright the environment's light is (a multiplier; 0 turns it off).
    /// yzw: unused.
    glm::vec4 environmentParams{1.0f, 0.0f, 0.0f, 0.0f};

    /// World space -> clip space of the camera drawing this frame. The
    /// reflections march their rays through the screen with it.
    glm::mat4 viewProj{1.0f};

    /// The same matrix for the camera of the picture the reflections read their
    /// colour from (the previous frame; see ScreenReflections).
    glm::mat4 prevViewProj{1.0f};

    /// Screen-space reflections.
    /// x: how strongly they replace the sky's reflection (0 = off, 1 = fully).
    /// y: farthest a reflected ray travels, in world units.
    /// z: how far behind a surface a ray may be and still count as hitting it.
    /// w: highest mip level of the previous picture (what a fully rough surface reads).
    glm::vec4 ssrParams{0.0f};

    /// x, y: the projection's [2][2] and [3][2], to turn a stored depth back
    /// into a distance in front of the camera. zw: unused.
    glm::vec4 ssrProj{0.0f};

    /// x: how many entries of the probe arrays are in use. yzw: unused.
    glm::ivec4 probeCount{0};

    /// For each probe. xyz: the low corner of its box (world space). w: its intensity.
    glm::vec4 probeBoxMin[kMaxReflectionProbes]{};

    /// For each probe. xyz: the high corner of its box. w: width of its soft border.
    glm::vec4 probeBoxMax[kMaxReflectionProbes]{};

    /// For each probe. xyz: where the scene was photographed from.
    /// w: 1 if the reflection is corrected for the box, 0 if not.
    glm::vec4 probePosition[kMaxReflectionProbes]{};

    /// For each probe. x: its layer in the probe cube map array. yzw: unused.
    glm::vec4 probeLayer[kMaxReflectionProbes]{};

    /// For each level of the global distance field (see GlobalDistanceField).
    /// xyz: world position of the level's low corner. w: size of one of its cells.
    glm::vec4 sdfOrigin[kSdfLevels]{};

    /// Shadows traced through the global distance field.
    /// x: which lights use them (kSdfShadows*). y: cells along one side of a level.
    /// z: farthest a ray travels, in world units.
    /// w: 1 if shiny surfaces reflect what the field holds, 0 if not.
    glm::vec4 sdfParams{0.0f};

    /// Reflections traced through the distance field.
    /// x: most steps of a ray. y: size of a cell of the finest level.
    /// z: 1 to also trust hits in the second level, 0 for the finest only.
    /// w: how many objects the field holds (the meshes' own fields traced at a hit).
    glm::vec4 sdfQuality{64.0f, 0.125f, 0.0f, 0.0f};

    /// Tuning of the traced rays.
    /// x: how far above a surface a shadow ray starts, in cells.
    /// y: 1 if what the reflections show is shadowed too. zw: unused.
    glm::vec4 sdfTuning{1.0f, 1.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuLight) == 80,
              "GpuLight must match the std140 struct in mesh.frag");
static_assert(offsetof(SceneUniformData, shadowViewProj) == 32,
              "The shadow matrices must start right after the two vec4s");
static_assert(offsetof(SceneUniformData, shadowParams) == 32 + 64 * kMaxShadowCasters,
              "The shadow parameters must follow the shadow matrices");
static_assert(offsetof(SceneUniformData, pointShadowParams) == 32 + 80 * kMaxShadowCasters,
              "The cube shadow parameters must follow the flat shadow data");
static_assert(offsetof(SceneUniformData, lights) == 48 + 80 * kMaxShadowCasters,
              "The light array must start after the shadow data");
static_assert(offsetof(SceneUniformData, cascadeViewProj) == 48 + 80 * kMaxShadowCasters + 80 * kMaxLights,
              "The cascade data must follow the light array");
static_assert(offsetof(SceneUniformData, cameraForward) ==
              48 + 80 * kMaxShadowCasters + 80 * kMaxLights + 80 * kShadowCascades + 16,
              "The camera data must follow the cascade data");
static_assert(sizeof(SceneUniformData) ==
              48 + 80 * kMaxShadowCasters + 80 * kMaxLights + 80 * kShadowCascades + 64 + 160
              + 16 + 4 * 16 * kMaxReflectionProbes + 16 * kSdfLevels + 16 + 16 + 16,
              "SceneUniformData must match the std140 block in mesh.frag");

/**
 * @brief The shadow images the scene shader reads, as handed to SceneUniforms::init().
 *
 * Every image is read twice: through a comparison sampler, which answers
 * "lit or shadowed" for a reference depth, and through a plain sampler, which
 * returns the stored depth (to find what blocks the light).
 */
struct ShadowBindings {
    std::array<VkImageView, kMaxShadowCasters> mapViews{};            ///< ShadowMap::view() of each flat map.
    std::array<VkSampler,   kMaxShadowCasters> mapSamplers{};         ///< ShadowMap::sampler().
    std::array<VkSampler,   kMaxShadowCasters> mapDepthSamplers{};    ///< ShadowMap::depthSampler().

    std::array<VkImageView, kMaxPointShadowCasters> cubeViews{};          ///< ShadowCube::view() of each cube.
    std::array<VkSampler,   kMaxPointShadowCasters> cubeSamplers{};       ///< ShadowCube::sampler().
    std::array<VkSampler,   kMaxPointShadowCasters> cubeDepthSamplers{};  ///< ShadowCube::depthSampler().

    VkImageView cascadeView         = VK_NULL_HANDLE;   ///< ShadowCascades::view().
    VkSampler   cascadeSampler      = VK_NULL_HANDLE;   ///< ShadowCascades::sampler().
    VkSampler   cascadeDepthSampler = VK_NULL_HANDLE;   ///< ShadowCascades::depthSampler().
};

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
     * Set 0 has seven bindings: 0 is the uniform buffer with SceneUniformData;
     * 1, 2 and 3 are the flat shadow maps, the shadow cubes and the sun's
     * cascades, read through comparison samplers; 4, 5 and 6 are the same
     * three, read through plain samplers for their raw depths. The shadow
     * images never change, so every set is pointed at them once, here.
     *
     * @param ctx     Initialised Vulkan context.
     * @param shadows The views and samplers of every shadow image.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, const ShadowBindings& shadows);

    /// Destroys everything. Safe to call more than once.
    void shutdown();

    /// @return The layout of set 0. The mesh pipeline layout is built from it.
    VkDescriptorSetLayout layout() const { return m_layout; }

    /// Index of the extra slot used for renders that are not the editor scene
    /// (the asset thumbnails). Pass it where a frame index is expected.
    static constexpr uint32_t kPreviewSlot = Renderer::MAX_FRAMES_IN_FLIGHT;

    /// Index of the slot used while a reflection probe photographs the scene.
    /// Its contents are the frame's, seen from the probe's position.
    static constexpr uint32_t kProbeSlot = Renderer::MAX_FRAMES_IN_FLIGHT + 1;

    /**
     * @brief Copies @p data into the buffer of a frame slot.
     * @param frameIndex Renderer::currentFrameIndex(), or kPreviewSlot.
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
    Slot                  m_slots[Renderer::MAX_FRAMES_IN_FLIGHT + 2];   // + kPreviewSlot, kProbeSlot

    /// Maps a frame index to its slot: the preview and probe slots stay
    /// themselves, any other index wraps around the frames in flight.
    static uint32_t slotFor(uint32_t frameIndex) {
        return frameIndex >= kPreviewSlot ? std::min(frameIndex, kProbeSlot)
                                          : frameIndex % Renderer::MAX_FRAMES_IN_FLIGHT;
    }
};

} // namespace fe
