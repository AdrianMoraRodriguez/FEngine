#pragma once

/**
 * @file GlobalDistanceField.h
 * @brief The distance field of the whole scene, around the camera.
 *
 * Software ray tracing (shadows and reflections that need no ray tracing
 * hardware) follows a ray through a signed distance field: at each point the
 * field says how far the nearest surface is, so the ray can safely advance
 * that much. Tracing against every object's own field would cost more the
 * more objects there are; instead the fields of all objects are merged once
 * into one volume, and tracing costs the same however many objects it holds.
 *
 * The field has several levels, each a cube around the camera: a small one
 * with fine cells (precision where it matters, near the viewer) and bigger
 * ones with coarser cells (reach). A compute shader rebuilds a level when an
 * object moves or the camera has moved far enough from where the level was
 * centred, taking the smallest distance of all objects at each of its cells
 * (see sdf_global.comp). The levels are stacked along z in one 3D texture.
 *
 * Surfaces are inflated by half a cell of their level, so that sheets thinner
 * than a cell (a plane, a leaf) are not lost between cells.
 *
 * The texture lives in VK_IMAGE_LAYOUT_GENERAL: it is written by a compute
 * shader and sampled by others.
 */

#include "gfx/SdfAtlas.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fe {

class VulkanContext;

/// One object as the compute shader reads it (std430 layout, 128 bytes).
struct SdfInstance {
    glm::mat4 worldToLocal{1.0f};   ///< World space -> the mesh's own space.
    glm::vec4 boxMin{0.0f};         ///< xyz: corner of the mesh's field, in the mesh's space. w: how metallic the object is (0..1).
    glm::vec4 boxMax{0.0f};         ///< xyz: opposite corner. w: size of a cell of the field.
    glm::vec4 atlasInfo{0.0f};      ///< x: where the brick starts along z in the atlas. yzw: cells of the field.
    glm::vec4 extra{1.0f};          ///< x: smallest scale of the object (mesh distances times it are world distances). yzw: its colour (linear), which the reflections show.
};
static_assert(sizeof(SdfInstance) == 128, "SdfInstance must match the shader's Instance");

class GlobalDistanceField {
public:
    /// Cells along each side of a level at the default resolution (see setResolution()).
    static constexpr uint32_t kBaseResolution = 128;

    /// Number of levels.
    static constexpr uint32_t kLevels = 3;

    /// World size of the sides of each level at the default detail, in metres, finest first.
    static constexpr float kBaseExtents[kLevels] = {16.0f, 64.0f, 256.0f};

    /**
     * @brief Chooses how finely the field follows the scene near the camera.
     *
     * Every level but the outermost is scaled by @p scale: below 1 the cells
     * get smaller (sharper edges, but the level covers less of the space
     * around the camera and is rebuilt more often when the camera moves);
     * above 1 they get bigger. The outermost level always covers
     * kBaseExtents[2], so the reach of the shadows does not shrink. The field
     * is rebuilt in the next update() if this changes.
     */
    void setDetail(float scale);

    /**
     * @brief Chooses how many cells each level has along a side (128, 192 or 256).
     *
     * The same space is covered with more, smaller cells: sharper shadows and
     * reflections everywhere, for memory (about 38 MB at 128, 127 MB at 192,
     * 302 MB at 256) and for the time the levels take to build. The volumes are
     * recreated, which waits for the GPU: call it only when the setting changes.
     *
     * @return `false` if the volumes could not be recreated (the field is then unusable).
     */
    bool setResolution(uint32_t cells);

    /// @return Cells along each side of a level.
    uint32_t resolution() const { return m_resolution; }

    /// @return Bytes of video memory the field's two volumes take (distances and colours, all levels).
    uint64_t memoryBytes() const {
        const uint64_t cells = static_cast<uint64_t>(m_resolution) * m_resolution * m_resolution * kLevels;
        return cells * (2 + 4);   // R16F + RGBA8
    }

    /// @return How many objects the field (and the scene shader's copy of the list) holds.
    uint32_t instanceCount() const { return m_instanceCount; }

    /// @return The scale given to setDetail().
    float detail() const { return m_detail; }

    /// @return World size of the side of @p level, in metres.
    float extent(uint32_t level) const {
        return level + 1 == kLevels ? kBaseExtents[level] : kBaseExtents[level] * m_detail;
    }

    /// @return Size of a cell of @p level, in metres.
    float cellSize(uint32_t level) const { return extent(level) / static_cast<float>(m_resolution); }

    /// @return How far from the camera the outermost level reaches, in metres.
    float reach() const { return 0.5f * extent(kLevels - 1); }

    /// Most objects the field can hold; the rest are left out (logged once).
    static constexpr uint32_t kMaxInstances = 512;

    GlobalDistanceField() = default;
    ~GlobalDistanceField() { shutdown(); }

    GlobalDistanceField(const GlobalDistanceField&)            = delete;
    GlobalDistanceField& operator=(const GlobalDistanceField&) = delete;

    /**
     * @param ctx         Vulkan context; must outlive the field.
     * @param shaderDir   Folder with sdf_global.comp.spv, sky.vert.spv and sdf_debug.frag.spv.
     * @param atlas       The meshes' distance fields; must outlive this object.
     * @param colorFormat Format of the scene pass's colour image (for the debug view).
     * @param depthFormat Format of its depth image.
     * @return `true` on success. `false` if something failed; then, if
     *         sampleLayout() is still not null, the descriptors work but the
     *         field is never built (the shaders are missing, say).
     */
    bool init(const VulkanContext& ctx, const char* shaderDir, const SdfAtlas& atlas,
              VkFormat colorFormat, VkFormat depthFormat);

    /// Frees everything. Safe to call more than once.
    void shutdown();

    /**
     * @brief Rebuilds the levels that the objects or the camera changed.
     *
     * Records a compute pass; call it outside any render pass, before the
     * scene pass that samples the volume.
     *
     * @param cmd        Command buffer being recorded.
     * @param frameIndex Renderer::currentFrameIndex(): picks which copy of the
     *                   object list is written, so the GPU can still be using the other.
     * @param instances  Every object that has a distance field.
     * @param eye        Camera position: every level is centred on it (in steps of a few cells).
     * @return `true` if any level was rebuilt.
     */
    bool update(VkCommandBuffer cmd, uint32_t frameIndex,
                const std::vector<SdfInstance>& instances, const glm::vec3& eye);

    /// @return `true` once the volume has been built at least once.
    bool ready() const { return m_built; }

    /// World position of the corner of a level (the corner of its cell (0,0,0)).
    glm::vec3 origin(uint32_t level) const { return m_origin[level]; }

    /**
     * @brief Draws the volume as a picture, from the camera, over the whole
     *        image of the scene pass (replacing the scene).
     *
     * Call inside the scene pass, where the sky would go. Does nothing until ready().
     *
     * @param inverseViewProj Inverse of the camera's view-projection matrix.
     */
    void drawDebug(VkCommandBuffer cmd, VkExtent2D extent, const glm::mat4& inverseViewProj) const;

    /// Layout of the set that samples the field. Binding 0: the distances (R16F).
    /// 1: the colour of the nearest object (RGBA8). 2: the atlas of the meshes' own
    /// fields (SdfAtlas). 3: the list of objects (SdfInstance, std430), of which
    /// instanceCount() are valid.
    /// The levels are stacked along z: level l is layers l * resolution() .. (l + 1) * resolution() - 1.
    VkDescriptorSetLayout sampleLayout() const { return m_sampleLayout; }

    /// Set that samples the volume. The volume is in VK_IMAGE_LAYOUT_GENERAL.
    VkDescriptorSet sampleSet() const { return m_sampleSet; }

private:
    static constexpr uint32_t kFrames = 2;   ///< Renderer::MAX_FRAMES_IN_FLIGHT.

    VkShaderModule loadShader(const char* path) const;
    bool createVolume();
    bool createColourVolume();
    bool createDescriptors(const SdfAtlas& atlas);
    void updateVolumeDescriptors();
    void destroyVolumes();
    bool createComputePipeline(const char* shaderDir);
    bool createDebugPipeline(const char* shaderDir, VkFormat colorFormat, VkFormat depthFormat);

    const VulkanContext* m_ctx = nullptr;

    // The volume.
    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_view       = VK_NULL_HANDLE;

    /// The colour of the nearest object at every cell (RGBA8, same size as the distances; alpha: how metallic it is).
    VkImage       m_colourImage      = VK_NULL_HANDLE;
    VmaAllocation m_colourAllocation = VK_NULL_HANDLE;
    VkImageView   m_colourView       = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;

    // The objects, one copy per frame in flight.
    VkBuffer      m_instanceBuffer[kFrames]{};
    VmaAllocation m_instanceAlloc[kFrames]{};
    void*         m_instanceMapped[kFrames]{};

    // Descriptors.
    VkDescriptorPool      m_pool          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_computeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sampleLayout  = VK_NULL_HANDLE;
    VkDescriptorSet       m_computeSet[kFrames]{};
    VkDescriptorSet       m_sampleSet = VK_NULL_HANDLE;

    // Pipelines.
    VkPipelineLayout m_computeLayoutPipeline = VK_NULL_HANDLE;
    VkPipeline       m_computePipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_debugLayout           = VK_NULL_HANDLE;
    VkPipeline       m_debugPipeline         = VK_NULL_HANDLE;

    // What the levels were last built from.
    bool      m_built = false;
    uint64_t  m_instanceHash = 0;
    /// The objects again, in memory only the GPU uses, which the scene shader reads to
    /// trace the meshes' own fields at a hit. Copied from the host buffers when they change.
    VkBuffer      m_instanceGpu      = VK_NULL_HANDLE;
    VmaAllocation m_instanceGpuAlloc = VK_NULL_HANDLE;
    uint32_t      m_instanceCount    = 0;

    uint32_t  m_resolution = kBaseResolution;
    const SdfAtlas* m_atlas = nullptr;
    float     m_detail = 1.0f;
    glm::vec3 m_origin[kLevels]{};
    bool      m_levelBuilt[kLevels]{};
    bool      m_warnedFull = false;
};

} // namespace fe
