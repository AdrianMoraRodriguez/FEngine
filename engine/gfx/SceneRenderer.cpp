#include "gfx/SceneRenderer.h"

#include "gfx/OffscreenBuffer.h"
#include "core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe {
namespace {

/// View matrix of one face of a point light's shadow cube.
///
/// The axes are the ones the Vulkan specification gives for each cube-map
/// layer (0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z): for a direction
/// d, the face's "right" and "up" give the texture coordinates (s, t) the GPU
/// looks up, and the "forward" axis is the one the direction mostly points
/// along. The rows of a view matrix are right, up and minus forward.
glm::mat4 cubeFaceView(uint32_t face, const glm::vec3& position) {
    struct Axes { glm::vec3 right, up, forward; };
    static const Axes kFaces[ShadowCube::kFaces] = {
        {{ 0.0f, 0.0f,-1.0f}, { 0.0f,-1.0f, 0.0f}, { 1.0f, 0.0f, 0.0f}},   // +X
        {{ 0.0f, 0.0f, 1.0f}, { 0.0f,-1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},   // -X
        {{ 1.0f, 0.0f, 0.0f}, { 0.0f, 0.0f, 1.0f}, { 0.0f, 1.0f, 0.0f}},   // +Y
        {{ 1.0f, 0.0f, 0.0f}, { 0.0f, 0.0f,-1.0f}, { 0.0f,-1.0f, 0.0f}},   // -Y
        {{ 1.0f, 0.0f, 0.0f}, { 0.0f,-1.0f, 0.0f}, { 0.0f, 0.0f, 1.0f}},   // +Z
        {{-1.0f, 0.0f, 0.0f}, { 0.0f,-1.0f, 0.0f}, { 0.0f, 0.0f,-1.0f}},   // -Z
    };
    const Axes& a = kFaces[face];

    // glm matrices are indexed [column][row].
    glm::mat4 rotation(1.0f);
    for (int c = 0; c < 3; ++c) {
        rotation[c][0] =  a.right[c];
        rotation[c][1] =  a.up[c];
        rotation[c][2] = -a.forward[c];
    }
    return rotation * glm::translate(glm::mat4(1.0f), -position);
}

} // namespace

bool SceneRenderer::init(const VulkanContext& ctx, const char* shaderDir) {
    if (!m_meshes.init(ctx)) return false;

    // The shadow map comes before the uniforms (their descriptor sets point
    // at it), and the uniforms before the pipeline (its layout is built from
    // their descriptor set layout).
    std::array<VkImageView, kMaxShadowCasters> views{};
    std::array<VkSampler,   kMaxShadowCasters> samplers{};
    for (uint32_t i = 0; i < kMaxShadowCasters; ++i) {
        if (!m_shadowMaps[i].init(ctx)) return false;
        views[i]    = m_shadowMaps[i].view();
        samplers[i] = m_shadowMaps[i].sampler();
    }

    std::array<VkImageView, kMaxPointShadowCasters> cubeViews{};
    std::array<VkSampler,   kMaxPointShadowCasters> cubeSamplers{};
    for (uint32_t i = 0; i < kMaxPointShadowCasters; ++i) {
        if (!m_shadowCubes[i].init(ctx)) return false;
        cubeViews[i]    = m_shadowCubes[i].view();
        cubeSamplers[i] = m_shadowCubes[i].sampler();
    }

    if (!m_uniforms.init(ctx, views, samplers, cubeViews, cubeSamplers)) return false;

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
    for (ShadowMap& map : m_shadowMaps) map.shutdown();
    for (ShadowCube& cube : m_shadowCubes) cube.shutdown();
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
    uint32_t nextShadowSlot = 0;       // flat maps: directional and spot lights
    uint32_t nextCubeSlot   = 0;       // shadow cubes: point lights

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
                                         std::cos(glm::radians(inner)), -1.0f, 0.0f);
                break;
            }

            case LightComponent::Type::Directional:
            default:
                out.colorType = glm::vec4(color, kGpuLightDirectional);
                break;
        }

        // A light that asks for it gets a shadow map of its own kind, while
        // there are some left: a cube for a point light, a flat map otherwise.
        if (light.castShadows) {
            const bool isPoint = light.type == LightComponent::Type::Point;
            uint32_t&      next  = isPoint ? nextCubeSlot : nextShadowSlot;
            const uint32_t limit = isPoint ? kMaxPointShadowCasters : kMaxShadowCasters;

            if (next < limit) {
                out.spotCone.z = static_cast<float>(next++);
            } else {
                static bool warned = false;
                if (!warned) {
                    FE_WARN("SceneRenderer: more shadow-casting lights than shadow maps; the extra ones cast no shadows");
                    warned = true;
                }
            }
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

bool SceneRenderer::fitDirectionalShadow(const Scene& scene,
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

void SceneRenderer::drawShadowCasters(VkCommandBuffer cmd, const Scene& scene,
                                      const glm::mat4& lightViewProj) const {
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

void SceneRenderer::renderShadows(VkCommandBuffer cmd,
                                  const Scene& scene,
                                  uint32_t frameIndex) {
    SceneUniformData data = gatherLights(scene);

    // Which light owns each shadow map, found from the slot gatherLights()
    // wrote into the light. -1 means the map is free this frame. Point lights
    // number their cubes separately from the flat maps.
    int slotLight[kMaxShadowCasters];
    int cubeLight[kMaxPointShadowCasters];
    for (int& l : slotLight) l = -1;
    for (int& l : cubeLight) l = -1;
    for (int i = 0; i < data.lightCount.x; ++i) {
        const int slot = static_cast<int>(data.lights[i].spotCone.z);
        if (slot < 0) continue;
        if (data.lights[i].colorType.w == kGpuLightPoint) cubeLight[slot] = i;
        else                                              slotLight[slot] = i;
    }

    const float resolution = static_cast<float>(ShadowMap::kResolution);

    for (uint32_t slot = 0; slot < kMaxShadowCasters; ++slot) {
        // Every map goes through begin() and end() every frame, even the ones
        // nobody uses: that keeps all of them in the layout the shader's
        // descriptors promise.
        bool casting = false;
        glm::mat4 lightViewProj{1.0f};

        if (slotLight[slot] >= 0) {
            GpuLight& light = data.lights[slotLight[slot]];
            const glm::vec3 direction = glm::vec3(light.direction);

            if (light.colorType.w == kGpuLightSpot) {
                // A spot light sees the cone it lights, plus a little margin,
                // through a perspective projection. The far plane is the
                // light's range: nothing beyond it is lit anyway.
                const float outer = std::acos(std::clamp(light.spotCone.x, -1.0f, 1.0f));
                const float fov   = std::min(2.0f * outer + glm::radians(2.0f), glm::radians(170.0f));
                const float nearPlane = 0.1f;
                const float farPlane  = std::max(light.positionRange.w, nearPlane + 0.1f);

                const glm::vec3 position = glm::vec3(light.positionRange);
                const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
                lightViewProj = glm::perspective(fov, 1.0f, nearPlane, farPlane)
                              * glm::lookAt(position, position + direction, up);
                casting = true;

                // One texel covers 2 * tan(fov / 2) / resolution world units per
                // unit of distance to the light.
                const float texelPerDistance = 2.0f * std::tan(0.5f * fov) / resolution;
                data.shadowParams[slot] = glm::vec4(1.0f / resolution, 1.5f * texelPerDistance, 1.0f, 0.0f);
            } else {
                float radius = 0.0f;
                if (fitDirectionalShadow(scene, direction, lightViewProj, radius)) {
                    casting = true;

                    const float texelWorld = 2.0f * radius / resolution;   // one texel, in world units
                    data.shadowParams[slot] = glm::vec4(1.0f / resolution, 1.5f * texelWorld, 0.0f, 0.0f);
                } else {
                    light.spotCone.z = -1.0f;   // nothing to cast: the shader skips the lookup
                }
            }
        }
        data.shadowViewProj[slot] = lightViewProj;

        m_shadowMaps[slot].begin(cmd);
        if (casting) drawShadowCasters(cmd, scene, lightViewProj);
        m_shadowMaps[slot].end(cmd);
    }

    // ---- Point lights: six views per light, one cube each ----
    // Every face has a 90 degree field of view, so six of them cover the whole
    // sphere around the light. The near plane is a constant shared with the
    // shader; the far plane is the light's range.
    constexpr float kCubeNear = 0.05f;
    const float cubeResolution = static_cast<float>(ShadowCube::kResolution);
    data.pointShadowParams = glm::vec4(2.0f / cubeResolution,          // one texel, per unit of distance
                                       1.5f * 2.0f / cubeResolution,   // normal offset, per unit of distance
                                       kCubeNear, 0.0f);

    for (uint32_t slot = 0; slot < kMaxPointShadowCasters; ++slot) {
        // As with the flat maps, every cube is begun and ended every frame.
        m_shadowCubes[slot].begin(cmd);

        if (cubeLight[slot] >= 0) {
            GpuLight& light = data.lights[cubeLight[slot]];
            const glm::vec3 position = glm::vec3(light.positionRange);
            const float farPlane = std::max(light.positionRange.w, kCubeNear + 0.1f);

            for (uint32_t face = 0; face < ShadowCube::kFaces; ++face) {
                const glm::mat4 faceViewProj = glm::perspective(glm::radians(90.0f), 1.0f, kCubeNear, farPlane)
                                             * cubeFaceView(face, position);
                m_shadowCubes[slot].beginFace(cmd, face);
                drawShadowCasters(cmd, scene, faceViewProj);
                m_shadowCubes[slot].endFace(cmd);
            }
        } else {
            // Nobody uses this cube: still clear each face, so that every
            // layer ends up in the layout the descriptors promise.
            for (uint32_t face = 0; face < ShadowCube::kFaces; ++face) {
                m_shadowCubes[slot].beginFace(cmd, face);
                m_shadowCubes[slot].endFace(cmd);
            }
        }

        m_shadowCubes[slot].end(cmd);
    }

    // The GPU reads the lights when the scene pass executes, later in this frame.
    m_uniforms.update(frameIndex, data);
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
