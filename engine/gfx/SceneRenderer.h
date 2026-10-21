#pragma once

/**
 * @file SceneRenderer.h
 * @brief Draws the entities of a Scene into the current render pass.
 */

#include "core/Geometry.h"
#include "gfx/MeshLibrary.h"
#include "gfx/MeshPipeline.h"
#include "gfx/SceneUniforms.h"
#include "gfx/ShadowMap.h"
#include "gfx/ShadowPipeline.h"
#include "scene/Scene.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/**
 * @brief Renders every entity that has a MeshRenderer and a WorldTransform.
 *
 * Owns the mesh pipeline, the mesh library and the scene uniforms (the
 * lights). It does not open or close render passes: call render() between
 * Renderer::beginOffscreenPass() and Renderer::endOffscreenPass().
 *
 * Per frame, in this order:
 *   1. Scene::updateTransforms(), so every WorldTransform is current.
 *   2. prepare(), which loads any model the scene uses for the first time.
 *   3. Renderer::beginFrame().
 *   4. renderShadows(), outside any render pass.
 *   5. The scene pass: Renderer::beginOffscreenPass(), render(), Renderer::endOffscreenPass().
 *
 * ### Lighting
 * Every LightComponent in the scene lights the objects, up to kMaxLights
 * (16) at once. Directional and spot lights shine along the entity's
 * forward axis (-Z, like a camera), so rotating the entity aims them; point
 * and spot lights also use the entity's position and the light's range.
 * A scene with no lights at all uses the engine's default sun, so a new or
 * old scene is never dark. Surfaces are shaded per pixel with a diffuse
 * (Lambert) term; there are no specular highlights yet.
 *
 * ### Shadows
 * One light casts shadows: the first directional light with castShadows set.
 * Every MeshRenderer with castShadows set is drawn into a shadow map from
 * that light's point of view, and the main pass darkens what the map says is
 * hidden from the light. The map covers a box that fits all the casters in
 * the scene, so it gets less sharp as the scene grows; the default sun used
 * by scenes without lights casts no shadows. Point and spot shadows are not
 * implemented yet.
 *
 * Intensity is the light's brightness at about one unit of distance for point
 * and spot lights, which then fade with the square of the distance and reach
 * nothing at all beyond their range. Values between 5 and 30 are typical.
 */
class SceneRenderer {
public:
    /**
     * @brief Creates the pipeline, the scene uniforms and the built-in meshes.
     *
     * @param ctx       Initialised Vulkan context.
     * @param shaderDir Directory containing the compiled SPIR-V shaders.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, const char* shaderDir);

    /// Destroys all GPU resources. Safe to call more than once.
    void shutdown();

    /**
     * @brief Loads every mesh the scene uses that is not on the GPU yet.
     *
     * Call once per frame before beginning to record it: loading submits
     * GPU work of its own, which must not happen in the middle of a frame.
     * It costs one pass over the scene's MeshRenderer components.
     */
    void prepare(const Scene& scene);

    /**
     * @brief Draws the shadow map and writes this frame's lights to the GPU.
     *
     * Must run every frame, before the scene pass and outside any render
     * pass, because it opens and closes its own. It must run after
     * Renderer::beginFrame(), which guarantees the GPU is done with the frame
     * slot whose uniform buffer is rewritten here. render() relies on both.
     *
     * The pass runs even when nothing casts shadows: the map is simply
     * cleared, which keeps its layout valid for the shader.
     *
     * @param cmd        Command buffer from Renderer::currentCommandBuffer().
     * @param scene      Scene to draw.
     * @param frameIndex Renderer::currentFrameIndex().
     */
    void renderShadows(VkCommandBuffer cmd, const Scene& scene, uint32_t frameIndex);

    /**
     * @brief Records the draw calls for every renderable entity.
     *
     * Call renderShadows() first in the same frame: it provides the lights
     * and the shadow map this reads.
     *
     * @param cmd        Command buffer inside an offscreen render pass.
     * @param scene      Scene to draw.
     * @param viewProj   Combined view-projection matrix of the camera.
     * @param extent     Size of the render target, for viewport and scissor.
     * @param frameIndex Renderer::currentFrameIndex(): which frame slot's
     *                   uniform buffer to use.
     */
    void render(VkCommandBuffer cmd,
                const Scene& scene,
                const glm::mat4& viewProj,
                VkExtent2D extent,
                uint32_t frameIndex);

    /**
     * @brief Finds the nearest renderable entity hit by a ray.
     *
     * Tests the ray against the bounding box of each entity's mesh, using
     * the same set of entities that render() draws: what you see is what
     * you can pick.
     *
     * @param scene        Scene to test.
     * @param ray          Ray in world space.
     * @param[out] outDistance Distance along the ray to the hit. Only
     *                     meaningful when an entity is returned.
     * @return The entity hit, or entt::null if the ray hits nothing.
     */
    entt::entity pick(const Scene& scene, const Ray& ray, float& outDistance) const;

    /// @return The mesh library, used to register models.
    MeshLibrary& meshes() { return m_meshes; }

private:
    /// Collects the scene's lights into the block the shaders read. Also
    /// picks the shadow-casting light (SceneUniformData::lightCount.y).
    static SceneUniformData gatherLights(const Scene& scene);

    /// Builds the shadow light's view-projection so that it covers every
    /// shadow caster. @return `false` if there is no caster to cover.
    bool fitShadowFrustum(const Scene& scene, const glm::vec3& lightDirection,
                          glm::mat4& outViewProj, float& outRadius) const;

    ShadowMap      m_shadowMap;
    ShadowPipeline m_shadowPipeline;
    SceneUniforms  m_uniforms;
    MeshPipeline   m_pipeline;
    MeshLibrary    m_meshes;
};

} // namespace fe
