#pragma once

/**
 * @file SceneRenderer.h
 * @brief Draws the entities of a Scene into the current render pass.
 */

#include "core/Geometry.h"
#include "gfx/MeshLibrary.h"
#include "gfx/MeshPipeline.h"
#include "scene/Scene.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace fe {

class VulkanContext;

/**
 * @brief Renders every entity that has a MeshRenderer and a WorldTransform.
 *
 * Owns the mesh pipeline and the mesh library. It does not open or close
 * render passes: call render() between Renderer::beginOffscreenPass() and
 * Renderer::endOffscreenPass().
 *
 * Per frame, in this order:
 *   1. Scene::updateTransforms(), so every WorldTransform is current.
 *   2. prepare(), which loads any model the scene uses for the first time.
 *   3. The frame itself, with render().
 */
class SceneRenderer {
public:
    /**
     * @brief Creates the pipeline and uploads the built-in meshes.
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
     * @brief Records the draw calls for every renderable entity.
     *
     * @param cmd      Command buffer inside an offscreen render pass.
     * @param scene    Scene to draw.
     * @param viewProj Combined view-projection matrix of the camera.
     * @param extent   Size of the render target, for viewport and scissor.
     */
    void render(VkCommandBuffer cmd,
                const Scene& scene,
                const glm::mat4& viewProj,
                VkExtent2D extent) const;

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
    MeshPipeline m_pipeline;
    MeshLibrary  m_meshes;
};

} // namespace fe
