#include "gfx/SceneRenderer.h"

#include "gfx/OffscreenBuffer.h"

#include <limits>

namespace fe {

bool SceneRenderer::init(const VulkanContext& ctx, const char* shaderDir) {
    if (!m_meshes.init(ctx)) return false;

    return m_pipeline.init(ctx,
                           OffscreenBuffer::format(),
                           OffscreenBuffer::depthFormat(),
                           shaderDir);
}

void SceneRenderer::shutdown() {
    m_pipeline.shutdown();
    m_meshes.shutdown();
}

void SceneRenderer::prepare(const Scene& scene) {
    m_meshes.beginFrame();

    auto view = scene.registry().view<const MeshRenderer>();
    for (auto [entity, renderer] : view.each())
        m_meshes.ensureLoaded(renderer.mesh);
}

void SceneRenderer::render(VkCommandBuffer cmd,
                           const Scene& scene,
                           const glm::mat4& viewProj,
                           VkExtent2D extent) const {
    // Pipeline and dynamic state are bound once; only the push constants
    // change per object.
    m_pipeline.bind(cmd, extent);

    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        const MeshBuffer* mesh = m_meshes.get(renderer.mesh);
        if (!mesh) continue;   // MeshNone, or a model that is unknown or failed to load

        MeshPushConstants pc{viewProj * world.matrix, world.matrix};
        m_pipeline.push(cmd, pc);
        mesh->draw(cmd);
    }
}

entt::entity SceneRenderer::pick(const Scene& scene, const Ray& ray,
                                 float& outDistance) const {
    entt::entity best  = entt::null;
    float        bestT = std::numeric_limits<float>::max();

    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        const std::optional<Aabb> box = m_meshes.bounds(renderer.mesh);
        if (!box) continue;   // not drawn, so not pickable either

        float t = 0.0f;
        if (intersectRayBox(ray, *box, world.matrix, t) && t < bestT) {
            bestT = t;
            best  = entity;
        }
    }

    outDistance = bestT;
    return best;
}

} // namespace fe
