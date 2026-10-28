#pragma once

/**
 * @file MeshPipeline.h
 * @brief Vulkan graphics pipeline for rendering meshes.
 *
 * Loads the compiled SPIR-V shaders, configures all fixed-function state
 * (vertex input, rasteriser, depth test, blending) and creates the pipeline.
 * Its layout has two ways of passing data to the shaders:
 *   - push constants, rewritten for every draw call (the matrices), and
 *   - descriptor set 0, bound once per frame (the scene's lights; see
 *     SceneUniforms), and
 *   - descriptor set 1, bound per object (its material's textures; see
 *     TextureLibrary), and
 *   - descriptor set 2, the environment (the sky's light; see Environment), and
 *   - descriptor set 3, the ambient occlusion (see AmbientOcclusion), and
 *   - descriptor set 4, what screen-space reflections read (see ScreenReflections), and
 *   - descriptor set 5, the reflection probes' cube maps (see ReflectionProbes).
 *   - descriptor set 6, the scene's global distance field (see GlobalDistanceField).
 *
 * Uses dynamic rendering: no VkRenderPass is needed.
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/// Push constant data sent to the vertex shader each draw call.
/// Mirrors the `PushConstants` block in mesh.vert.
///
/// Vulkan only guarantees 128 bytes of push constants, and this block uses
/// all of them: the model matrix is sent as its first three rows (the last
/// row of a transform matrix is always 0,0,0,1) to make room for the tint.
struct MeshPushConstants {
    glm::mat4 mvp;           ///< Model-View-Projection matrix.
    glm::vec4 modelRows[3];  ///< Object-to-world matrix, rows 0 to 2, to bring normals and positions to world space.
    glm::vec4 tint;          ///< rgb: material tint (sRGB). w is unused.

    MeshPushConstants(const glm::mat4& mvpMatrix, const glm::mat4& model,
                      const glm::vec3& tintColor = glm::vec3(1.0f))
        : mvp(mvpMatrix), tint(tintColor, 1.0f) {
        // glm stores columns: element [c][r] is column c, row r.
        for (int r = 0; r < 3; ++r)
            modelRows[r] = glm::vec4(model[0][r], model[1][r], model[2][r], model[3][r]);
    }
};
static_assert(sizeof(MeshPushConstants) == 128, "Must match the PushConstants block in mesh.vert");

/**
 * @brief The graphics pipeline used to draw meshes into the viewport.
 */
class MeshPipeline {
public:
    MeshPipeline() = default;
    ~MeshPipeline();

    MeshPipeline(const MeshPipeline&)            = delete;
    MeshPipeline& operator=(const MeshPipeline&) = delete;

    /**
     * @brief Creates the pipeline layout and pipeline.
     *
     * @param ctx         Initialised Vulkan context.
     * @param colorFormat Format of the colour attachment.
     * @param depthFormat Format of the depth attachment.
     * @param shaderDir   Directory containing mesh.vert.spv / mesh.frag.spv.
     * @param sceneLayout Layout of descriptor set 0, which the shaders read
     *                    the scene data from (SceneUniforms::layout()).
     * @param materialLayout    Layout of set 1 (TextureLibrary::layout()).
     * @param environmentLayout Layout of set 2 (Environment::createLayout()).
     * @param occlusionLayout   Layout of set 3 (AmbientOcclusion::resultLayout()).
     * @param reflectionLayout  Layout of set 4 (ScreenReflections::layout()).
     * @param probeLayout       Layout of set 5 (ReflectionProbes::layout()).
     * @param fieldLayout       Layout of set 6 (GlobalDistanceField::sampleLayout()).
     * @param translucent       `true` builds the variant for translucent materials:
     *                          its output is mixed with what is behind it and it
     *                          does not write depth, so it must be drawn after
     *                          the opaque objects, farthest first.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx,
              VkFormat colorFormat,
              VkFormat depthFormat,
              const char* shaderDir,
              VkDescriptorSetLayout sceneLayout,
              VkDescriptorSetLayout materialLayout,
              VkDescriptorSetLayout environmentLayout,
              VkDescriptorSetLayout occlusionLayout,
              VkDescriptorSetLayout reflectionLayout,
              VkDescriptorSetLayout probeLayout,
              VkDescriptorSetLayout fieldLayout,
              bool translucent = false);

    /// Destroys all Vulkan objects.
    void shutdown();

    /**
     * @brief Binds the pipeline and sets the dynamic viewport and scissor.
     *
     * Call once per pass, then bindDescriptorSet(), then call push() + a
     * draw for each object. Faces turned away from the camera are culled
     * until setDoubleSided(true) says otherwise.
     *
     * @param cmd    Command buffer to record into.
     * @param extent Size of the render area.
     */
    void bind(VkCommandBuffer cmd, VkExtent2D extent) const;

    /// Chooses whether the next draw calls also draw the back faces.
    void setDoubleSided(VkCommandBuffer cmd, bool doubleSided) const;

    /**
     * @brief Binds the descriptor set that holds the scene data (set 0).
     *
     * Call once per pass, after bind(). The set stays bound for every draw
     * call that follows.
     */
    void bindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet sceneSet) const;

    /**
     * @brief Binds the descriptor set with the material's texture (set 1).
     *
     * Must be called before the first draw call, and again whenever the next
     * object uses a different texture.
     */
    void bindMaterial(VkCommandBuffer cmd, VkDescriptorSet materialSet) const;

    /**
     * @brief Binds the descriptor set with the environment (set 2).
     *
     * Call once per pass, before the first draw call.
     */
    void bindEnvironment(VkCommandBuffer cmd, VkDescriptorSet environmentSet) const;

    /**
     * @brief Binds the descriptor set with the ambient occlusion (set 3).
     *
     * Call once per pass, before the first draw call.
     */
    void bindOcclusion(VkCommandBuffer cmd, VkDescriptorSet occlusionSet) const;

    /**
     * @brief Binds the descriptor set with the scene depth and the previous
     *        frame's colour that screen-space reflections read (set 4).
     *
     * Call once per pass, before the first draw call.
     */
    void bindReflections(VkCommandBuffer cmd, VkDescriptorSet reflectionSet) const;

    /**
     * @brief Binds the descriptor set with the reflection probes' cube maps (set 5).
     *
     * Call once per pass, before the first draw call. Which probes apply is
     * decided by the scene data (set 0), so the set can always be bound.
     */
    void bindProbes(VkCommandBuffer cmd, VkDescriptorSet probeSet) const;

    /**
     * @brief Binds the descriptor set with the scene's global distance field (set 6).
     *
     * Call once per pass, before the first draw call. Whether the shader traces
     * it is decided by the scene data (set 0), so the set can always be bound.
     */
    void bindDistanceField(VkCommandBuffer cmd, VkDescriptorSet fieldSet) const;

    /**
     * @brief Uploads the per-object push constants.
     *
     * @param cmd Command buffer to record into.
     * @param pc  Matrices for the next draw call.
     */
    void push(VkCommandBuffer cmd, const MeshPushConstants& pc) const;

private:
    VkShaderModule loadShader(const char* path) const;

    const VulkanContext* m_ctx      = nullptr;
    VkPipelineLayout     m_layout   = VK_NULL_HANDLE;
    VkPipeline           m_pipeline = VK_NULL_HANDLE;
};

} // namespace fe
