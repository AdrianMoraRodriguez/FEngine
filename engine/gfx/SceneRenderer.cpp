#include "gfx/SceneRenderer.h"

#include "gfx/OffscreenBuffer.h"
#include "core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe {

bool SceneRenderer::init(const VulkanContext& ctx, const char* shaderDir) {
    if (!m_meshes.init(ctx)) return false;

    // The shadow map comes before the uniforms (their descriptor sets point
    // at it), and the uniforms before the pipeline (its layout is built from
    // their descriptor set layout).
    if (!m_shadowMap.init(ctx)) return false;
    if (!m_uniforms.init(ctx, m_shadowMap.view(), m_shadowMap.sampler())) return false;

    if (!m_shadowPipeline.init(ctx, ShadowMap::kFormat, shaderDir)) return false;

    return m_pipeline.init(ctx,
                           OffscreenBuffer::format(),
                           OffscreenBuffer::depthFormat(),
                           shaderDir,
                           m_uniforms.layout());
}

void SceneRenderer::shutdown() {
    m_pipeline.shutdown();
    m_shadowPipeline.shutdown();
    m_uniforms.shutdown();
    m_shadowMap.shutdown();
    m_meshes.shutdown();
}

void SceneRenderer::prepare(const Scene& scene) {
    m_meshes.beginFrame();

    auto view = scene.registry().view<const MeshRenderer>();
    for (auto [entity, renderer] : view.each())
        m_meshes.ensureLoaded(renderer.mesh);
}

SceneUniformData SceneRenderer::gatherLights(const Scene& scene) {
    SceneUniformData data;
    int count = 0;

    auto lights = scene.registry().view<const LightComponent, const WorldTransform>();
    for (auto [entity, light, world] : lights.each()) {
        if (count >= static_cast<int>(kMaxLights)) {
            // Only worth saying once, not on every frame.
            static bool warned = false;
            if (!warned) {
                FE_WARN("SceneRenderer: more than %u lights in the scene; the extra ones are ignored",
                        kMaxLights);
                warned = true;
            }
            break;
        }

        // A light shines along its local -Z axis, like a camera looks. That
        // axis is the third column of the world matrix, pointing backwards;
        // normalising it removes the entity's scale.
        const glm::vec3 axis = glm::vec3(world.matrix[2]);
        if (glm::dot(axis, axis) < 1e-12f) continue;   // degenerate scale: no direction
        const glm::vec3 travel = -glm::normalize(axis);

        GpuLight& out = data.lights[count];
        out.positionRange = glm::vec4(glm::vec3(world.matrix[3]), light.range);
        out.direction     = glm::vec4(travel, 0.0f);

        const glm::vec3 color = glm::vec3(light.color) * light.intensity;

        switch (light.type) {
            case LightComponent::Type::Point:
                out.colorType = glm::vec4(color, kGpuLightPoint);
                break;

            case LightComponent::Type::Spot: {
                out.colorType = glm::vec4(color, kGpuLightSpot);

                // The shader compares cosines. The inner cone can never be
                // wider than the outer one, even if a hand-edited scene file
                // says so.
                const float outer = std::clamp(light.outerConeDegrees, 1.0f, 89.0f);
                const float inner = std::clamp(light.innerConeDegrees, 0.0f, outer);
                out.spotCone = glm::vec4(std::cos(glm::radians(outer)),
                                         std::cos(glm::radians(inner)), 0.0f, 0.0f);
                break;
            }

            case LightComponent::Type::Directional:
            default:
                out.colorType = glm::vec4(color, kGpuLightDirectional);

                // The first directional light that asks for it casts the shadows.
                if (light.castShadows && data.lightCount.y < 0)
                    data.lightCount.y = count;
                break;
        }
        ++count;
    }

    // A scene without any light would be pitch black apart from the ambient
    // light. Use the engine's default sun instead, so a new or old scene is
    // always readable. Adding any light turns it off.
    if (count == 0) {
        GpuLight& sun = data.lights[0];
        sun.colorType     = glm::vec4(0.75f, 0.75f, 0.75f, kGpuLightDirectional);
        sun.direction     = glm::vec4(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)), 0.0f);
        count = 1;
    }

    data.lightCount.x = count;
    return data;
}

bool SceneRenderer::fitShadowFrustum(const Scene& scene,
                                     const glm::vec3& lightDirection,
                                     glm::mat4& outViewProj,
                                     float& outRadius) const {
    // World-space box around every shadow caster.
    glm::vec3 lo( std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());
    bool any = false;

    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        if (!renderer.castShadows) continue;

        const std::optional<Aabb> box = m_meshes.bounds(renderer.mesh);
        if (!box) continue;

        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) ? box->max.x : box->min.x,
                                  (corner & 2) ? box->max.y : box->min.y,
                                  (corner & 4) ? box->max.z : box->min.z);
            const glm::vec3 point = glm::vec3(world.matrix * glm::vec4(local, 1.0f));
            lo  = glm::min(lo, point);
            hi  = glm::max(hi, point);
            any = true;
        }
    }
    if (!any) return false;

    // A sphere around that box. A sphere looks the same from every direction,
    // so the map covers the casters however the light is turned. The margin
    // keeps geometry that touches the sphere from being clipped.
    const glm::vec3 center = 0.5f * (lo + hi);
    const float     radius = std::max(0.5f * glm::length(hi - lo), 0.5f) * 1.05f;

    // The light looks at the sphere's centre from one radius away. It needs
    // an "up" that is not parallel to its direction.
    const glm::vec3 up = std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                             : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 lightView = glm::lookAt(center - lightDirection * radius, center, up);

    // Directional light: parallel rays, so an orthographic box. Depth runs
    // from 0 (the eye) to 2 radii (the far side of the sphere).
    const glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius);

    outViewProj = lightProj * lightView;
    outRadius   = radius;
    return true;
}

void SceneRenderer::renderShadows(VkCommandBuffer cmd,
                                  const Scene& scene,
                                  uint32_t frameIndex) {
    SceneUniformData data = gatherLights(scene);

    // Is there a shadow-casting light, and anything for it to cast?
    glm::mat4 lightViewProj{1.0f};
    bool casting = false;

    if (data.lightCount.y >= 0) {
        const glm::vec3 direction = glm::vec3(data.lights[data.lightCount.y].direction);
        float radius = 0.0f;

        if (fitShadowFrustum(scene, direction, lightViewProj, radius)) {
            casting = true;

            const float resolution = static_cast<float>(ShadowMap::kResolution);
            const float texelWorld = 2.0f * radius / resolution;   // one texel, in world units

            data.lightViewProj = lightViewProj;
            data.shadowParams  = glm::vec4(1.0f / resolution, 1.5f * texelWorld, 0.0f, 0.0f);
        } else {
            data.lightCount.y = -1;   // nothing to cast: the shader skips the lookup
        }
    }

    // The GPU reads the lights when the scene pass executes, later in this frame.
    m_uniforms.update(frameIndex, data);

    // The pass always runs, so the map is always in the layout the shader expects.
    m_shadowMap.begin(cmd);

    if (casting) {
        m_shadowPipeline.bind(cmd);

        auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
        for (auto [entity, renderer, world] : view.each()) {
            if (!renderer.castShadows) continue;

            const MeshBuffer* mesh = m_meshes.get(renderer.mesh);
            if (!mesh) continue;

            m_shadowPipeline.push(cmd, ShadowPushConstants{lightViewProj * world.matrix});
            mesh->draw(cmd);
        }
    }

    m_shadowMap.end(cmd);
}

void SceneRenderer::render(VkCommandBuffer cmd,
                           const Scene& scene,
                           const glm::mat4& viewProj,
                           VkExtent2D extent,
                           uint32_t frameIndex) {
    // Pipeline, dynamic state and the scene's descriptor set are bound once;
    // only the push constants change per object.
    m_pipeline.bind(cmd, extent);
    m_pipeline.bindDescriptorSet(cmd, m_uniforms.set(frameIndex));

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
