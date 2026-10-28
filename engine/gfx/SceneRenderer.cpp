#include "gfx/SceneRenderer.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/VulkanContext.h"
#include "import/AssetDatabase.h"
#include "import/ImageLoader.h"
#include "gfx/Renderer.h"
#include "core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace fe {
namespace {

static_assert(kShadowCascades == ShadowCascades::kCascades,
              "The shader and the image must agree on the number of cascades");

/// Fraction of a cascade's far distance over which it blends into the next one.
/// Must equal CASCADE_BLEND in mesh.frag. Each cascade also covers that band
/// of the cascade before it, since that is where the shader reads it.
constexpr float kCascadeBlend = 0.1f;

/// Near plane of a spot light's shadow projection. Must equal SPOT_NEAR in mesh.frag.
constexpr float kSpotNear = 0.1f;

/// How much the cascade splits follow a logarithmic spacing (1) instead of an
/// even one (0). Logarithmic puts more of the maps near the camera, where
/// detail is seen; even gives the far cascades more.
constexpr float kSplitLambda = 0.75f;

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

/// View-projection of one cascade of the sun's shadow.
///
/// The cascade covers a sphere around its slice of the camera's view, so the
/// size of its map does not change when the camera turns: a box that followed
/// the slice's shape would grow and shrink, and every shadow edge would
/// shimmer. For the same reason the centre is moved to a multiple of one
/// texel in the light's own axes, so that a shadow edge always falls on the
/// same texels while the camera moves.
///
/// The light's eye is pulled back towards the light until it reaches the
/// nearest shadow caster, even one that lies outside the slice: a tree beyond
/// the edge of the view still shadows what is inside it.
///
/// @param direction     Unit vector the light travels along.
/// @param center        Centre of the sphere around the slice.
/// @param radius        Radius of that sphere.
/// @param casterCenter  Centre of a sphere around every shadow caster.
/// @param casterRadius  Radius of that sphere.
/// @param resolution    Width of the map in texels.
/// @param[out] outDepthRange  Depth the map covers, from the eye to its far plane.
glm::mat4 cascadeViewProjection(const glm::vec3& direction,
                                glm::vec3 center, float radius,
                                const glm::vec3& casterCenter, float casterRadius,
                                float resolution, float& outDepthRange) {
    const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                       : glm::vec3(0.0f, 1.0f, 0.0f);

    // The light's axes, as a rotation with no position.
    const glm::mat3 rotation = glm::mat3(glm::lookAt(glm::vec3(0.0f), direction, up));

    // Snap the centre to the texel grid. The depth axis is left alone.
    const float texel = 2.0f * radius / resolution;
    glm::vec3 inLight = rotation * center;
    inLight.x = std::floor(inLight.x / texel) * texel;
    inLight.y = std::floor(inLight.y / texel) * texel;
    center = glm::transpose(rotation) * inLight;    // a rotation's inverse is its transpose

    // Distances along the light's direction: the slice, and the casters.
    const float sliceMid  = glm::dot(center, direction);
    const float casterMid = glm::dot(casterCenter, direction);
    const float nearest   = std::min(sliceMid - radius, casterMid - casterRadius);
    const float farthest  = sliceMid + radius;

    const glm::vec3 eye = center + direction * (nearest - sliceMid);
    outDepthRange = farthest - nearest;
    return glm::ortho(-radius, radius, -radius, radius, 0.0f, farthest - nearest)
         * glm::lookAt(eye, eye + direction, up);
}

} // namespace

const ProjectSettings& SceneRenderer::project() const {
    static const ProjectSettings kDefaults;
    return m_project ? *m_project : kDefaults;
}

bool SceneRenderer::init(const VulkanContext& ctx, const char* shaderDir, const ProjectSettings* project) {
    m_project = project;
    m_ctx = &ctx;
    if (!m_meshes.init(ctx, this->project().meshFieldCells())) return false;
    if (!m_textures.init(ctx)) return false;   // before the pipeline: its layout is set 1

    // The environments come before the pipelines too: their layout is set 2.
    // The built-in sky is available at once, as a rough 32-texel bake that takes
    // a few milliseconds; the full-quality one is baked in the background and
    // replaces it when ready (updateEnvironment()).
    m_environmentLayout = Environment::createLayout(ctx.device());
    if (!m_environmentLayout) return false;
    if (!m_defaultEnvironment.init(ctx, m_environmentLayout)) return false;
    if (!m_sceneEnvironment.init(ctx, m_environmentLayout)) return false;
    if (!m_defaultEnvironment.upload(bakeEnvironment(makeProceduralSky(), 32))) return false;
    m_defaultBake = std::async(std::launch::async, [] { return bakeEnvironment(makeProceduralSky()); });

    // The shadow map comes before the uniforms (their descriptor sets point
    // at it), and the uniforms before the pipeline (its layout is built from
    // their descriptor set layout).
    ShadowBindings shadows;
    for (uint32_t i = 0; i < kMaxShadowCasters; ++i) {
        if (!m_shadowMaps[i].init(ctx)) return false;
        shadows.mapViews[i]         = m_shadowMaps[i].view();
        shadows.mapSamplers[i]      = m_shadowMaps[i].sampler();
        shadows.mapDepthSamplers[i] = m_shadowMaps[i].depthSampler();
    }

    for (uint32_t i = 0; i < kMaxPointShadowCasters; ++i) {
        if (!m_shadowCubes[i].init(ctx)) return false;
        shadows.cubeViews[i]         = m_shadowCubes[i].view();
        shadows.cubeSamplers[i]      = m_shadowCubes[i].sampler();
        shadows.cubeDepthSamplers[i] = m_shadowCubes[i].depthSampler();
    }

    if (!m_shadowCascades.init(ctx)) return false;
    shadows.cascadeView         = m_shadowCascades.view();
    shadows.cascadeSampler      = m_shadowCascades.sampler();
    shadows.cascadeDepthSampler = m_shadowCascades.depthSampler();

    if (!m_uniforms.init(ctx, shadows)) return false;

    if (!m_shadowPipeline.init(ctx, ShadowMap::kFormat, shaderDir, m_textures.layout())) return false;

    if (!m_occlusion.init(ctx, shaderDir)) return false;   // before the pipeline: its layout is set 3

    if (!m_reflections.init(ctx)) return false;            // before the pipeline: its layout is set 4
    if (!m_probes.init(ctx, shaderDir)) return false;      // likewise set 5
    if (!m_probeTarget.init(ctx, ReflectionProbes::kResolution, ReflectionProbes::kResolution)) return false;

    // Before the pipelines: its sampling layout is set 6. Not essential: if the
    // shaders are missing the field is never built and the scene simply has no
    // software ray tracing. Only a field that cannot even be sampled is fatal.
    static_assert(GlobalDistanceField::kLevels == kSdfLevels,
                  "SceneUniformData must have a slot for each level of the distance field");
    if (!m_globalField.init(ctx, shaderDir, m_meshes.distanceAtlas(),
                            OffscreenBuffer::hdrFormat(), OffscreenBuffer::depthFormat())) {
        FE_WARN("SceneRenderer: the scene's distance field is unavailable (are sdf_global.comp.spv and sdf_debug.frag.spv built?)");
        if (m_globalField.sampleLayout() == VK_NULL_HANDLE) return false;
    }

    if (!m_pipeline.init(ctx,
                         OffscreenBuffer::hdrFormat(),
                         OffscreenBuffer::depthFormat(),
                         shaderDir,
                         m_uniforms.layout(),
                         m_textures.layout(),
                         m_environmentLayout,
                         m_occlusion.resultLayout(),
                         m_reflections.layout(),
                         m_probes.layout(),
                         m_globalField.sampleLayout())) return false;

    if (!m_translucentPipeline.init(ctx,
                                    OffscreenBuffer::hdrFormat(),
                                    OffscreenBuffer::depthFormat(),
                                    shaderDir,
                                    m_uniforms.layout(),
                                    m_textures.layout(),
                                    m_environmentLayout,
                                    m_occlusion.resultLayout(),
                                    m_reflections.layout(),
                                    m_probes.layout(),
                                    m_globalField.sampleLayout(),
                                    true)) return false;

    if (!m_sky.init(ctx, OffscreenBuffer::hdrFormat(), OffscreenBuffer::depthFormat(),
                    shaderDir, m_environmentLayout)) return false;

    // The outline is drawn over the finished 8-bit picture, not in the scene pass.
    if (!m_outline.init(ctx, OffscreenBuffer::format(), shaderDir))
        return false;

    if (!m_post.init(ctx, shaderDir)) return false;

    setupPreviewLighting();
    return true;
}

void SceneRenderer::shutdown() {
    // The bakes run on their own threads: wait for them before anything goes.
    if (m_defaultBake.valid()) m_defaultBake.wait();
    if (m_sceneBake.valid())   m_sceneBake.wait();

    m_globalField.shutdown();
    m_sky.shutdown();
    m_occlusion.shutdown();
    m_reflections.shutdown();
    m_probeTarget.shutdown();
    m_probes.shutdown();
    m_defaultEnvironment.shutdown();
    m_sceneEnvironment.shutdown();
    if (m_environmentLayout) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_environmentLayout, nullptr);
        m_environmentLayout = VK_NULL_HANDLE;
    }
    m_post.shutdown();
    m_outline.shutdown();
    m_translucentPipeline.shutdown();
    m_pipeline.shutdown();
    m_shadowPipeline.shutdown();
    m_uniforms.shutdown();
    for (ShadowMap& map : m_shadowMaps) map.shutdown();
    for (ShadowCube& cube : m_shadowCubes) cube.shutdown();
    m_shadowCascades.shutdown();
    m_textures.shutdown();
    m_meshes.shutdown();
}

void SceneRenderer::updateEnvironment(const Scene& scene) {
    using namespace std::chrono_literals;
    const auto finished = [](std::future<EnvironmentBake>& f) {
        return f.valid() && f.wait_for(0s) == std::future_status::ready;
    };

    // The built-in sky, at full quality.
    if (finished(m_defaultBake)) {
        const EnvironmentBake bake = m_defaultBake.get();
        if (bake.valid()) m_defaultEnvironment.upload(bake);
    }

    const AssetHandle wanted = scene.settings().environment;
    if (wanted != m_failedEnvironment) m_failedEnvironment = 0;   // chosen something else: a retry is fine later

    // A finished bake of the scene's picture. If the user has chosen another
    // one meanwhile, it is dropped.
    if (finished(m_sceneBake)) {
        const EnvironmentBake bake = m_sceneBake.get();
        const AssetHandle id = m_sceneBakeId;
        m_sceneBakeId = 0;
        if (id == wanted) {
            if (bake.valid() && m_sceneEnvironment.upload(bake)) {
                m_loadedEnvironment = id;
            } else {
                FE_WARN("SceneRenderer: environment '%s' could not be used; showing the default sky",
                        AssetDatabase::instance().pathOf(id).c_str());
                m_failedEnvironment = id;
                m_loadedEnvironment = 0;
            }
        }
    }

    // Start loading what the scene asks for. One bake at a time. With no
    // picture chosen the default sky is used; the last picture stays in memory,
    // so choosing it again is instant.
    if (wanted != 0 && wanted != m_loadedEnvironment && wanted != m_failedEnvironment &&
        !m_sceneBake.valid()) {
        const std::filesystem::path file = AssetDatabase::instance().fullPathOf(wanted);
        if (file.empty()) {
            FE_WARN("SceneRenderer: the scene's environment image is missing; showing the default sky");
            m_failedEnvironment = wanted;
        } else {
            m_sceneBakeId = wanted;
            m_sceneBake = std::async(std::launch::async, [file] {
                const std::optional<HdrImageData> image = loadImageHdr(file);
                return image ? bakeEnvironment(*image) : EnvironmentBake{};
            });
        }
    }
}

VkDescriptorSet SceneRenderer::activeEnvironment(const Scene& scene) const {
    // While another picture is loading, the previous one stays up.
    if (scene.settings().environment != 0 && m_loadedEnvironment != 0 && m_sceneEnvironment.valid())
        return m_sceneEnvironment.set();
    return m_defaultEnvironment.set();
}

void SceneRenderer::prepare(const Scene& scene) {
    m_meshes.beginFrame();
    m_textures.update();
    updateEnvironment(scene);

    auto view = scene.registry().view<const MeshRenderer>();
    for (auto [entity, renderer] : view.each())
        m_meshes.ensureLoaded(renderer.mesh);
}

void SceneRenderer::setupPreviewLighting() {
    // A tiny scene with one light, run through the same code as the editor
    // scene so the preview is lit by exactly the same shader inputs. It is
    // uploaded once: the light never changes, so the slot is never rewritten
    // while the GPU might still be reading it.
    Scene scene;
    scene.settings().ambientIntensity = 0.05f;   // the built-in sky does most of the filling-in

    const entt::entity key = scene.create("Preview Light");

    LightComponent light;
    light.type        = LightComponent::Type::Directional;
    light.intensity   = 1.1f;
    light.castShadows = false;
    scene.registry().emplace<LightComponent>(key, light);

    // Light comes from above, a little to the left of the camera (the camera
    // sits towards +X, +Z, see renderPreview()), so that the faces turned to
    // the viewer are lit and the others fall into the ambient light.
    const glm::vec3 towardsLight = glm::normalize(glm::vec3(-0.35f, 0.85f, 0.75f));
    scene.transform(key).rotation = glm::quatLookAt(-towardsLight, glm::vec3(0.0f, 1.0f, 0.0f));
    scene.updateTransforms();

    // The preview camera always looks from the same direction (renderPreview()
    // and the model viewer), so the shader gets that direction: w = 0 tells it
    // that xyz is a direction towards the camera and not a position.
    SceneUniformData data = gatherLights(scene);
    data.cameraPosition = glm::vec4(glm::normalize(glm::vec3(0.75f, 0.55f, 1.0f)), 0.0f);
    m_uniforms.update(SceneUniforms::kPreviewSlot, data);
}

bool SceneRenderer::renderPreview(VkCommandBuffer cmd, AssetHandle handle,
                                  const OffscreenBuffer& target, bool* settled) {
    const std::optional<Aabb> box = m_meshes.bounds(handle);
    if (!box) return false;

    // The camera looks at the middle of the mesh's bounding sphere, from far
    // enough that the whole sphere fits the vertical field of view.
    const glm::vec3 centre = 0.5f * (box->min + box->max);
    const float     radius = std::max(0.5f * glm::length(box->max - box->min), 1e-4f);

    const float     fovY     = glm::radians(30.0f);
    const float     distance = radius / std::sin(0.5f * fovY) * 1.05f;
    const glm::vec3 eye      = centre + glm::normalize(glm::vec3(0.75f, 0.55f, 1.0f)) * distance;

    const glm::mat4 view = glm::lookAt(eye, centre, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(fovY, 1.0f, distance - 1.5f * radius, distance + 1.5f * radius);
    proj[1][1] *= -1.0f;   // Vulkan clip space: +Y points down (as in EditorCamera).

    const uint64_t waitingBefore = m_textures.waitingRequests();
    m_previewTextures = true;
    const bool drawn = renderMeshView(cmd, handle, target, proj * view, glm::mat4(1.0f), true);
    m_previewTextures = false;

    if (settled) *settled = (m_textures.waitingRequests() == waitingBefore);
    return drawn;
}

void SceneRenderer::MaterialOverride::applyTo(TextureLibrary::MaterialRequest& request) const {
    for (int slot = 0; slot < TextureLibrary::SlotCount; ++slot)
        if (textures[slot] != 0)
            request.textures[slot] = {textures[slot], nullptr};

    if (custom) {
        request.metallic    = metallic;
        request.roughness   = roughness;
        request.normalScale = normalScale;
        request.emissive    = emissive;
    }

    // Transparency. The model's own mode stays unless the entity chose one.
    request.alphaMode = alphaModeOf(request.alphaMode);
    if (transparency == MaterialComponent::Transparency::Cutout) request.alphaCutoff = alphaCutoff;
    if (transparency != MaterialComponent::Transparency::Opaque) request.opacity *= opacity;
    if (doubleSided) request.doubleSided = true;

    // An emissive image the user chose must show: with no light colour to
    // multiply it (a model that has none, or a black one), use white.
    if (textures[TextureLibrary::Emissive] != 0 && request.emissive == glm::vec3(0.0f))
        request.emissive = glm::vec3(1.0f);
}

AlphaMode SceneRenderer::MaterialOverride::alphaModeOf(AlphaMode fromModel) const {
    switch (transparency) {
        case MaterialComponent::Transparency::Opaque: return AlphaMode::Opaque;
        case MaterialComponent::Transparency::Cutout: return AlphaMode::Mask;
        case MaterialComponent::Transparency::Blend:  return AlphaMode::Blend;
        default:                                      return fromModel;
    }
}

SceneRenderer::MaterialOverride SceneRenderer::MaterialOverride::from(const MaterialComponent& c) {
    MaterialOverride o;
    o.textures[TextureLibrary::Albedo]     = c.albedoTexture;
    o.textures[TextureLibrary::MetalRough] = c.metalRoughTexture;
    o.textures[TextureLibrary::Normal]     = c.normalTexture;
    o.textures[TextureLibrary::Emissive]   = c.emissiveTexture;

    o.custom      = c.customSurface;
    o.metallic    = std::clamp(c.metallic, 0.0f, 1.0f);
    o.roughness   = std::clamp(c.roughness, 0.0f, 1.0f);
    o.normalScale = std::max(c.normalStrength, 0.0f);
    // The picker shows sRGB; the shader works in linear light.
    o.emissive    = glm::pow(glm::max(c.emissiveColor, glm::vec3(0.0f)), glm::vec3(2.2f))
                  * std::max(c.emissiveIntensity, 0.0f);

    o.transparency = c.transparency;
    o.alphaCutoff  = std::clamp(c.alphaCutoff, 0.0f, 1.0f);
    o.opacity      = std::clamp(c.opacity, 0.0f, 1.0f);
    o.doubleSided  = c.doubleSided;
    return o;
}

TextureLibrary::MaterialRequest SceneRenderer::resolvePart(AssetHandle handle, const ModelParts* parts,
                                                          const MeshPart* part,
                                                          const MaterialOverride& material) const {
    TextureLibrary::MaterialRequest request;

    if (parts && part && part->material >= 0) {
        const ModelMaterial& source = parts->materials[part->material];

        // The slots, in the order of TextureLibrary::Slot.
        const int images[TextureLibrary::SlotCount] = {
            source.albedo, source.metalRough, source.normal, source.emissive};
        for (int slot = 0; slot < TextureLibrary::SlotCount; ++slot) {
            if (images[slot] < 0) continue;
            request.textures[slot] = {
                TextureLibrary::modelTextureKey(handle, static_cast<uint32_t>(images[slot])),
                &parts->images[images[slot]]};
        }
        request.metallic    = source.metallic;
        request.roughness   = source.roughness;
        request.normalScale = source.normalScale;
        request.emissive    = source.emissiveFactor;
        request.alphaMode   = source.alphaMode;
        request.alphaCutoff = source.alphaCutoff;
        request.opacity     = source.opacity;
        request.doubleSided = source.doubleSided;
    }
    material.applyTo(request);

    // A thumbnail only needs the colour, and a small version of it: the other
    // maps are invisible at 128 pixels, and a folder full of models must not
    // load hundreds of full-size textures. Once the texture cache is half
    // full, thumbnails stop adding to it and show the plain colours.
    if (m_previewTextures) {
        for (int slot = 0; slot < TextureLibrary::SlotCount; ++slot)
            if (slot != TextureLibrary::Albedo) request.textures[slot] = {};
        if (m_textures.textureCount() >= TextureLibrary::kMaxTextures / 2)
            request.textures[TextureLibrary::Albedo] = {};
        else
            request.textures[TextureLibrary::Albedo].maxSize = 256;
    }
    return request;
}

bool SceneRenderer::hasTranslucentParts(AssetHandle handle, const MaterialOverride& material) const {
    const ModelParts* parts = m_meshes.parts(handle);
    if (!parts) return material.alphaModeOf(AlphaMode::Opaque) == AlphaMode::Blend;

    for (const MeshPart& part : parts->parts) {
        const AlphaMode fromModel = part.material >= 0 ? parts->materials[part.material].alphaMode
                                                       : AlphaMode::Opaque;
        if (material.alphaModeOf(fromModel) == AlphaMode::Blend) return true;
    }
    return false;
}

void SceneRenderer::drawMesh(VkCommandBuffer cmd, const MeshPipeline& pipeline, AssetHandle handle,
                             const MeshBuffer& mesh, const MaterialOverride& material, bool textured,
                             VkDescriptorSet& boundMaterial, DrawPass pass) {
    const auto bindMaterial = [&](VkDescriptorSet set) {
        if (set != boundMaterial) {   // neighbouring draws often share a material
            pipeline.bindMaterial(cmd, set);
            boundMaterial = set;
        }
    };
    const bool wantTranslucent = (pass == DrawPass::Translucent);

    // Built-in meshes have no material of their own: the default one, with
    // whatever the entity's material puts in it. Without textures (the asset
    // thumbnails) everything is drawn solid with the default material.
    const ModelParts* parts = textured ? m_meshes.parts(handle) : nullptr;
    if (!parts) {
        if (!textured) {
            if (wantTranslucent) return;
            pipeline.setDoubleSided(cmd, false);
            bindMaterial(m_textures.fallback());
            mesh.draw(cmd);
            return;
        }
        const TextureLibrary::MaterialRequest request = resolvePart(handle, nullptr, nullptr, material);
        if ((request.alphaMode == AlphaMode::Blend) != wantTranslucent) return;
        pipeline.setDoubleSided(cmd, request.doubleSided);
        bindMaterial(m_textures.request(request));
        mesh.draw(cmd);
        return;
    }

    mesh.bind(cmd);
    for (const MeshPart& part : parts->parts) {
        const TextureLibrary::MaterialRequest request = resolvePart(handle, parts, &part, material);
        if ((request.alphaMode == AlphaMode::Blend) != wantTranslucent) continue;

        pipeline.setDoubleSided(cmd, request.doubleSided);
        bindMaterial(m_textures.request(request));
        mesh.drawRange(cmd, part.firstIndex, part.indexCount);
    }
}

void SceneRenderer::drawDepthOnly(VkCommandBuffer cmd, AssetHandle handle, const MeshBuffer& mesh,
                                  const MaterialOverride& material, const glm::mat4& mvp, bool translucentCasts) {
    const ModelParts* parts = m_meshes.parts(handle);

    // One part: the solid pipeline, or the cut-out one with the material that
    // holds the alpha. (The push constants go after the pipeline, which may
    // have a different layout from the one before it.)
    const auto drawPart = [&](const MeshPart* part, auto&& draw) {
        const AlphaMode fromModel = (parts && part && part->material >= 0)
                                  ? parts->materials[part->material].alphaMode : AlphaMode::Opaque;
        const AlphaMode mode = material.alphaModeOf(fromModel);
        if (mode == AlphaMode::Blend && !translucentCasts) return;   // no occlusion from glass

        // A translucent part uses the cut-out pipeline too: its shader tells the
        // two apart by the material's alpha mode.
        if (mode != AlphaMode::Opaque)
            m_shadowPipeline.bindMasked(cmd, m_textures.request(resolvePart(handle, parts, part, material)));
        else
            m_shadowPipeline.bind(cmd);
        m_shadowPipeline.push(cmd, ShadowPushConstants{mvp});
        draw();
    };

    if (!parts) {
        drawPart(nullptr, [&] { mesh.draw(cmd); });
        return;
    }
    mesh.bind(cmd);
    for (const MeshPart& part : parts->parts)
        drawPart(&part, [&] { mesh.drawRange(cmd, part.firstIndex, part.indexCount); });
}

SceneRenderer::MaterialOverride SceneRenderer::overrideOf(const Scene& scene, entt::entity entity) {
    if (const auto* material = scene.registry().try_get<MaterialComponent>(entity))
        return MaterialOverride::from(*material);
    return MaterialOverride{};
}

bool SceneRenderer::renderMeshView(VkCommandBuffer cmd, AssetHandle handle,
                                   const OffscreenBuffer& target, const glm::mat4& viewProj,
                                   const glm::mat4& model, bool textured) {
    const MeshBuffer* mesh = m_meshes.get(handle);
    if (!mesh) return false;

    const VkExtent2D extent{target.width(), target.height()};

    Renderer::beginOffscreenPass(cmd, target);

    m_pipeline.bind(cmd, extent);
    m_pipeline.bindDescriptorSet(cmd, m_uniforms.set(SceneUniforms::kPreviewSlot));
    m_pipeline.bindEnvironment(cmd, m_defaultEnvironment.set());   // previews always use the built-in sky
    m_pipeline.bindOcclusion(cmd, m_occlusion.whiteSet());          // and have no occlusion
    m_pipeline.bindReflections(cmd, m_reflections.emptySet());      // nor on-screen reflections
    m_pipeline.bindProbes(cmd, m_probes.set());                     // nor probes (the preview data lists none)
    m_pipeline.bindDistanceField(cmd, m_globalField.sampleSet());   // nor traced shadows (the preview data turns them off)
    VkDescriptorSet boundTexture = m_textures.fallback();
    m_pipeline.bindMaterial(cmd, boundTexture);
    m_pipeline.push(cmd, MeshPushConstants{viewProj * model, model});
    drawMesh(cmd, m_pipeline, handle, *mesh, MaterialOverride{}, textured, boundTexture, DrawPass::Solid);

    // A model with translucent parts: they go over the solid ones (in the
    // order the model lists them; the viewer does not sort).
    if (textured && hasTranslucentParts(handle, MaterialOverride{})) {
        m_translucentPipeline.bind(cmd, extent);
        m_translucentPipeline.bindDescriptorSet(cmd, m_uniforms.set(SceneUniforms::kPreviewSlot));
        m_translucentPipeline.bindEnvironment(cmd, m_defaultEnvironment.set());
        m_translucentPipeline.bindOcclusion(cmd, m_occlusion.whiteSet());
        m_translucentPipeline.bindReflections(cmd, m_reflections.emptySet());
        m_translucentPipeline.bindProbes(cmd, m_probes.set());
        m_translucentPipeline.bindDistanceField(cmd, m_globalField.sampleSet());
        boundTexture = m_textures.fallback();
        m_translucentPipeline.bindMaterial(cmd, boundTexture);
        m_translucentPipeline.push(cmd, MeshPushConstants{viewProj * model, model});
        drawMesh(cmd, m_translucentPipeline, handle, *mesh, MaterialOverride{}, textured, boundTexture,
                 DrawPass::Translucent);
    }

    Renderer::endOffscreenPass(cmd, target);

    // Previews are plain tone-mapped pictures: no bloom, default exposure.
    m_post.render(cmd, target, PostSettings{});
    return true;
}

SceneUniformData SceneRenderer::gatherLights(const Scene& scene) {
    SceneUniformData data;

    // The scene's ambient light: the same on every surface, from every direction.
    const SceneSettings& settings = scene.settings();
    data.ambientColor = glm::vec4(settings.ambientColor * std::max(settings.ambientIntensity, 0.0f), 0.0f);
    data.environmentParams = glm::vec4(std::max(settings.environmentIntensity, 0.0f), 0.0f, 0.0f, 0.0f);

    int count = 0;
    uint32_t nextShadowSlot = 0;       // flat maps: directional and spot lights
    uint32_t nextCubeSlot   = 0;       // shadow cubes: point lights
    bool     cascadeTaken   = false;   // the sun's cascades: the first directional light that casts

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

        // The size of the light, which sets how wide its shadows' soft edges
        // grow. A point or spot light's size is a radius in world units. The
        // sun's is an angle, and what the shader needs is its tangent: how many
        // units of blur each unit of distance from the blocker adds.
        const float softness = std::max(light.shadowSoftness, 0.0f);
        const float size = (light.type == LightComponent::Type::Directional)
                         ? std::tan(glm::radians(std::min(softness, 45.0f)))
                         : softness;
        out.direction     = glm::vec4(travel, size);

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

        // Whether or not it gets a shadow map below, the distance field
        // shadows know this light casts.
        out.extra.x = light.castShadows ? 1.0f : 0.0f;

        // A light that asks for it gets a shadow map of its own kind, while
        // there are some left: a cube for a point light, a flat map otherwise.
        // The first directional light that casts shadows gets the cascades,
        // which suit a light that lights the whole scene. Further directional
        // lights fall back to a flat map like the spot lights.
        if (light.castShadows && light.type == LightComponent::Type::Directional && !cascadeTaken) {
            cascadeTaken   = true;
            out.spotCone.z = 0.0f;
            out.spotCone.w = 1.0f;
        } else if (light.castShadows) {
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

bool SceneRenderer::casterSphere(const Scene& scene, glm::vec3& outCenter, float& outRadius) const {
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
    // so a map fitted to it covers the casters however the light is turned.
    // The margin keeps geometry that touches the sphere from being clipped.
    outCenter = 0.5f * (lo + hi);
    outRadius = std::max(0.5f * glm::length(hi - lo), 0.5f) * 1.05f;
    return true;
}

bool SceneRenderer::fitDirectionalShadow(const Scene& scene,
                                         const glm::vec3& lightDirection,
                                         glm::mat4& outViewProj,
                                         float& outRadius) const {
    glm::vec3 center;
    float     radius;
    if (!casterSphere(scene, center, radius)) return false;

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
                                      const glm::mat4& lightViewProj) {
    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        if (!renderer.castShadows) continue;

        const MeshBuffer* mesh = m_meshes.get(renderer.mesh);
        if (!mesh) continue;

        drawDepthOnly(cmd, renderer.mesh, *mesh, overrideOf(scene, entity),
                      lightViewProj * world.matrix, true);
    }
}

void SceneRenderer::updateDistanceField(VkCommandBuffer cmd, const Scene& scene, uint32_t frameIndex,
                                        const glm::mat4& cameraView) {
    // The detail and the resolution are settings of the project; changing them rebuilds the field.
    const ProjectSettings& quality = project();
    static constexpr float kDetail[] = {2.0f, 1.0f, 0.5f};
    m_globalField.setDetail(kDetail[std::clamp(quality.distanceFieldDetail, 0, 2)]);
    static constexpr uint32_t kResolutions[] = {128, 192, 256};
    m_globalField.setResolution(kResolutions[std::clamp(quality.distanceFieldResolution, 0, 2)]);

    std::vector<SdfInstance> instances;

    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        const std::optional<MeshLibrary::SdfPlacement> placement = m_meshes.distancePlacement(renderer.mesh);
        if (!placement) continue;   // no mesh, or its field is still being built

        // Mesh distances become world distances by the smallest scale of the
        // object: with an uneven scale this underestimates, which is the safe side.
        const float scale = std::min({glm::length(glm::vec3(world.matrix[0])),
                                      glm::length(glm::vec3(world.matrix[1])),
                                      glm::length(glm::vec3(world.matrix[2]))});
        if (!(scale > 1e-6f)) continue;

        SdfInstance instance;
        instance.worldToLocal = glm::inverse(world.matrix);
        instance.boxMin       = glm::vec4(placement->boxMin, 0.0f);   // w: metallic, set below
        instance.boxMax       = glm::vec4(placement->boxMax, placement->voxelSize);
        instance.atlasInfo    = glm::vec4(static_cast<float>(placement->atlasZ), glm::vec3(placement->dims));
        // What a reflection of this object shows: the average of the mesh's
        // vertex colours times the material's colour, as the shader would
        // multiply them. The textures are not sampled, so a textured model
        // shows up in reflections without them.
        glm::vec3 colour = placement->colour;
        if (const auto* material = scene.registry().try_get<MaterialComponent>(entity)) {
            colour *= material->baseColor;
            if (material->customSurface) instance.boxMin.w = std::clamp(material->metallic, 0.0f, 1.0f);
        }
        instance.extra        = glm::vec4(scale, colour);
        instances.push_back(instance);
    }

    const glm::vec3 eye = glm::vec3(glm::inverse(cameraView)[3]);
    m_globalField.update(cmd, frameIndex, instances, eye);
}

void SceneRenderer::renderShadows(VkCommandBuffer cmd,
                                  const Scene& scene,
                                  uint32_t frameIndex,
                                  const glm::mat4& cameraView,
                                  const glm::mat4& cameraProjection) {
    updateDistanceField(cmd, scene, frameIndex, cameraView);

    SceneUniformData data = gatherLights(scene);

    // Shadows traced through the distance field. Off until the field has been
    // built, so the shader never reads a volume that holds nothing yet.
    {
        const int  mode        = std::clamp(scene.settings().distanceFieldShadows, 0, 2);
        const bool reflections = scene.settings().distanceFieldReflections;
        if ((mode > 0 || reflections) && m_globalField.ready()) {
            data.sdfParams = glm::vec4(mode == 0 ? kSdfShadowsOff : (mode == 1 ? kSdfShadowsFill : kSdfShadowsEverywhere),
                                       static_cast<float>(m_globalField.resolution()),
                                       m_globalField.reach(),
                                       reflections ? 1.0f : 0.0f);
            for (uint32_t l = 0; l < GlobalDistanceField::kLevels; ++l)
                data.sdfOrigin[l] = glm::vec4(m_globalField.origin(l), m_globalField.cellSize(l));
            static constexpr float kSteps[] = {32.0f, 64.0f, 128.0f};
            const ProjectSettings& qs = project();
            data.sdfQuality = glm::vec4(kSteps[std::clamp(qs.distanceFieldReflectionQuality, 0, 2)],
                                        m_globalField.cellSize(0),
                                        qs.distanceFieldReflectionReach > 0 ? 1.0f : 0.0f,
                                        static_cast<float>(m_globalField.instanceCount()));
            data.sdfTuning  = glm::vec4(std::clamp(qs.distanceFieldShadowBias, 0.25f, 4.0f),
                                        qs.distanceFieldReflectedShadows ? 1.0f : 0.0f, 0.0f, 0.0f);
        }
    }

    // Which light owns each shadow map, found from the slot gatherLights()
    // wrote into the light. -1 means the map is free this frame. Point lights
    // number their cubes separately from the flat maps.
    int slotLight[kMaxShadowCasters];
    int cubeLight[kMaxPointShadowCasters];
    for (int& l : slotLight) l = -1;
    for (int& l : cubeLight) l = -1;
    for (int i = 0; i < data.lightCount.x; ++i) {
        const int slot = static_cast<int>(data.lights[i].spotCone.z);
        if (slot < 0 || data.lights[i].spotCone.w > 0.5f) continue;   // none, or cascaded
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
                const float nearPlane = kSpotNear;
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
                data.shadowParams[slot] = glm::vec4(texelPerDistance, 1.5f * texelPerDistance, 1.0f, farPlane);
            } else {
                float radius = 0.0f;
                if (fitDirectionalShadow(scene, direction, lightViewProj, radius)) {
                    casting = true;

                    const float texelWorld = 2.0f * radius / resolution;   // one texel, in world units
                    data.shadowParams[slot] = glm::vec4(texelWorld, 1.5f * texelWorld, 0.0f,
                                                        2.0f * radius);   // far plane of the box
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

    // ---- The sun's cascades ----
    // What the camera sees, from its matrices: where it is, where it looks,
    // how wide its view is, and its near and far planes (read back from the
    // two numbers a perspective projection keeps for depth).
    const glm::mat4 cameraWorld = glm::inverse(cameraView);
    const glm::vec3 cameraPos   = glm::vec3(cameraWorld[3]);
    const glm::vec3 forward     = -glm::normalize(glm::vec3(cameraWorld[2]));
    data.cameraForward  = glm::vec4(forward, -glm::dot(forward, cameraPos));

    // Screen-space reflections: on only when there is a picture of a previous
    // frame to read (otherwise the shader would find only black).
    m_viewProj = cameraProjection * cameraView;
    const SceneSettings& rs = scene.settings();
    data.viewProj     = m_viewProj;
    data.prevViewProj = m_historyViewProj;
    if (rs.reflections && rs.reflectionIntensity > 0.0f && m_reflections.ready()) {
        data.ssrParams = glm::vec4(std::clamp(rs.reflectionIntensity, 0.0f, 1.0f),
                                   std::max(rs.reflectionDistance, 1.0f),
                                   std::max(rs.reflectionThickness, 0.01f),
                                   m_reflections.maxMip());
        data.ssrProj   = glm::vec4(cameraProjection[2][2], cameraProjection[3][2], 0.0f, 0.0f);
    }
    data.cameraPosition = glm::vec4(cameraPos, 1.0f);

    const float p22 = cameraProjection[2][2];
    const float p32 = cameraProjection[3][2];
    const bool  perspective = cameraProjection[2][3] == -1.0f && std::abs(p22) > 1e-6f;
    const float camNear = perspective ? p32 / p22 : 0.0f;
    const float camFar  = perspective ? p32 / (p22 + 1.0f) : 0.0f;
    const bool  cameraOk = perspective && camNear > 0.0f && camFar > camNear
                        && std::abs(cameraProjection[0][0]) > 1e-6f
                        && std::abs(cameraProjection[1][1]) > 1e-6f;

    int cascadeLight = -1;
    for (int i = 0; i < data.lightCount.x; ++i)
        if (data.lights[i].spotCone.w > 0.5f) cascadeLight = i;

    // Splits: a blend of an even and a logarithmic spacing between the near
    // plane and the shadow distance (or the far plane, if that is nearer).
    float splits[kShadowCascades] = {};
    glm::vec3 casterCenter(0.0f);
    float     casterRadius = 0.0f;
    bool      cascadesCast = false;

    if (cascadeLight >= 0) {
        cascadesCast = cameraOk && casterSphere(scene, casterCenter, casterRadius);
        if (!cascadesCast) {
            // Nothing to cast, or no usable camera: the shader skips the lookup.
            data.lights[cascadeLight].spotCone.z = -1.0f;
            data.lights[cascadeLight].spotCone.w = 0.0f;
        }
    }

    if (cascadesCast) {
        const float shadowFar = std::min(camFar, kShadowDistance);
        for (uint32_t i = 0; i < kShadowCascades; ++i) {
            const float t       = static_cast<float>(i + 1) / static_cast<float>(kShadowCascades);
            const float uniform = camNear + (shadowFar - camNear) * t;
            const float logarithmic = camNear * std::pow(shadowFar / camNear, t);
            splits[i] = glm::mix(uniform, logarithmic, kSplitLambda);
        }
        splits[kShadowCascades - 1] = shadowFar;
    }
    data.cascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);

    const float tanX = cameraOk ? 1.0f / std::abs(cameraProjection[0][0]) : 0.0f;
    const float tanY = cameraOk ? 1.0f / std::abs(cameraProjection[1][1]) : 0.0f;
    const float cascadeResolution = static_cast<float>(ShadowCascades::kResolution);

    m_shadowCascades.begin(cmd);
    for (uint32_t i = 0; i < kShadowCascades; ++i) {
        glm::mat4 lightViewProj{1.0f};

        if (cascadesCast) {
            // The slice of the view this cascade covers: from the end of the
            // previous one (a little earlier, where that one blends into this
            // one) to its own end.
            const float sliceNear = (i == 0) ? camNear : splits[i - 1] * (1.0f - kCascadeBlend);
            const float sliceFar  = splits[i];

            // Its eight corners, from the view's width at each distance.
            glm::vec3 centre(0.0f);
            glm::vec3 corners[8];
            for (int c = 0; c < 8; ++c) {
                const float d = (c & 4) ? sliceFar : sliceNear;
                const glm::vec4 inView((c & 1) ? d * tanX : -d * tanX,
                                       (c & 2) ? d * tanY : -d * tanY, -d, 1.0f);
                corners[c] = glm::vec3(cameraWorld * inView);
                centre += corners[c] * 0.125f;
            }
            float radius = 0.0f;
            for (const glm::vec3& corner : corners)
                radius = std::max(radius, glm::length(corner - centre));
            radius = std::ceil(radius * 16.0f) / 16.0f;   // a little steadier when the view changes

            float depthRange = 0.0f;
            lightViewProj = cascadeViewProjection(glm::vec3(data.lights[cascadeLight].direction),
                                                  centre, radius, casterCenter, casterRadius,
                                                  cascadeResolution, depthRange);

            const float texelWorld = 2.0f * radius / cascadeResolution;
            data.cascadeParams[i] = glm::vec4(texelWorld, 1.5f * texelWorld, 0.0f, depthRange);
        }
        data.cascadeViewProj[i] = lightViewProj;

        // As with the other maps, every cascade is begun and ended every frame.
        m_shadowCascades.beginCascade(cmd, i);
        if (cascadesCast) drawShadowCasters(cmd, scene, lightViewProj);
        m_shadowCascades.endCascade(cmd);
    }
    m_shadowCascades.end(cmd);

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

    // renderProbes() adds the probes to this and uploads it again.
    m_frameData = data;
}

void SceneRenderer::renderAmbientOcclusion(VkCommandBuffer cmd, const Scene& scene,
                                           const glm::mat4& viewProj, const glm::mat4& projection,
                                           VkExtent2D extent) {
    const SceneSettings& settings = scene.settings();
    m_occlusionValid   = false;
    m_reflectionsDepth = false;

    const bool wantOcclusion  = settings.ambientOcclusion && settings.aoIntensity > 0.0f;
    const bool wantReflection = settings.reflections && settings.reflectionIntensity > 0.0f;
    if (!wantOcclusion && !wantReflection) return;
    if (!m_occlusion.resize(extent)) return;   // empty viewport, or out of memory

    // The reflections look through the same depth. When it changes size they
    // lose their picture, and say so through ready().
    if (wantReflection) m_reflectionsDepth = m_reflections.resize(extent, m_occlusion.depthView());

    // The depth of everything visible, not only what casts shadows.
    m_occlusion.beginDepth(cmd);
    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        const MeshBuffer* mesh = m_meshes.get(renderer.mesh);
        if (!mesh) continue;

        drawDepthOnly(cmd, renderer.mesh, *mesh, overrideOf(scene, entity), viewProj * world.matrix, false);
    }
    m_occlusion.endDepth(cmd);

    if (wantOcclusion) {
        m_occlusion.compute(cmd, projection, std::max(settings.aoRadius, 0.01f), settings.aoIntensity);
        m_occlusionValid = true;
    }
}

void SceneRenderer::render(VkCommandBuffer cmd,
                           const Scene& scene,
                           const glm::mat4& viewProj,
                           VkExtent2D extent,
                           uint32_t frameIndex) {
    if (showDistanceField && m_globalField.ready()) {
        m_globalField.drawDebug(cmd, extent, glm::inverse(viewProj));
        return;
    }

    // The picture of the last frame is only usable once it exists at this size.
    SceneSets sets;
    sets.uniforms    = m_uniforms.set(frameIndex);
    sets.occlusion   = m_occlusionValid ? m_occlusion.resultSet() : m_occlusion.whiteSet();
    sets.reflections = (m_reflectionsDepth && m_reflections.ready()) ? m_reflections.liveSet()
                                                                     : m_reflections.emptySet();
    sets.probes      = m_probes.set();
    sets.distanceField = m_globalField.sampleSet();
    drawScene(cmd, scene, viewProj, extent, sets);
}

void SceneRenderer::drawScene(VkCommandBuffer cmd,
                              const Scene& scene,
                              const glm::mat4& viewProj,
                              VkExtent2D extent,
                              const SceneSets& sets) {
    // Pipeline, dynamic state and the scene's descriptor set are bound once;
    // only the push constants change per object.
    m_pipeline.bind(cmd, extent);
    m_pipeline.bindDescriptorSet(cmd, sets.uniforms);
    const VkDescriptorSet environment = activeEnvironment(scene);
    m_pipeline.bindEnvironment(cmd, environment);
    m_pipeline.bindOcclusion(cmd, sets.occlusion);
    m_pipeline.bindReflections(cmd, sets.reflections);
    m_pipeline.bindProbes(cmd, sets.probes);
    m_pipeline.bindDistanceField(cmd, sets.distanceField);

    // Set 1 must be bound before the first draw; the loop rebinds it only
    // when the texture changes.
    VkDescriptorSet boundTexture = m_textures.fallback();
    m_pipeline.bindMaterial(cmd, boundTexture);

    auto view = scene.registry().view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, renderer, world] : view.each()) {
        const MeshBuffer* mesh = m_meshes.get(renderer.mesh);
        if (!mesh) continue;   // MeshNone, or a model that is unknown or failed to load

        // The material is optional: without one, the mesh keeps its own colours.
        // Textures set on the entity replace the model's own for those slots,
        // its surface values replace the model's when "custom surface" is on,
        // and the colour only tints whatever is drawn.
        glm::vec3        tint(1.0f);
        MaterialOverride surface;
        if (const auto* material = scene.registry().try_get<MaterialComponent>(entity)) {
            tint    = material->baseColor;
            surface = MaterialOverride::from(*material);
        }

        m_pipeline.push(cmd, MeshPushConstants{viewProj * world.matrix, world.matrix, tint});
        drawMesh(cmd, m_pipeline, renderer.mesh, *mesh, surface, true, boundTexture, DrawPass::Solid);
    }

    // The sky goes next: it only fills what no solid object covered.
    const SceneSettings& settings = scene.settings();
    if (settings.showSky)
        m_sky.draw(cmd, extent, environment, viewProj, std::max(settings.environmentIntensity, 0.0f));

    // Translucent surfaces go last, over the sky and the solid objects, since
    // they are mixed with whatever is already in the picture. The objects that
    // have such parts are found first and sorted far to near by the depth of
    // the centre of their bounding box.
    struct Translucent {
        entt::entity entity;
        float        depth;   ///< Distance along the view direction (clip-space w).
    };
    std::vector<Translucent> translucent;
    for (auto [entity, renderer, world] : view.each()) {
        if (!m_meshes.get(renderer.mesh)) continue;
        if (!hasTranslucentParts(renderer.mesh, overrideOf(scene, entity))) continue;

        glm::vec3 centre(0.0f);
        if (const std::optional<Aabb> box = m_meshes.bounds(renderer.mesh))
            centre = 0.5f * (box->min + box->max);
        translucent.push_back({entity, (viewProj * world.matrix * glm::vec4(centre, 1.0f)).w});
    }
    if (translucent.empty()) return;

    std::sort(translucent.begin(), translucent.end(),
              [](const Translucent& a, const Translucent& b) { return a.depth > b.depth; });

    m_translucentPipeline.bind(cmd, extent);
    m_translucentPipeline.bindDescriptorSet(cmd, sets.uniforms);
    m_translucentPipeline.bindEnvironment(cmd, environment);
    // A translucent surface takes no ambient occlusion: the picture holds that of
    // whatever is behind it, not of the glass.
    m_translucentPipeline.bindOcclusion(cmd, m_occlusion.whiteSet());
    m_translucentPipeline.bindReflections(cmd, sets.reflections);
    m_translucentPipeline.bindProbes(cmd, sets.probes);
    m_translucentPipeline.bindDistanceField(cmd, sets.distanceField);
    boundTexture = m_textures.fallback();
    m_translucentPipeline.bindMaterial(cmd, boundTexture);

    for (const Translucent& item : translucent) {
        const auto& renderer = view.get<const MeshRenderer>(item.entity);
        const auto& world    = view.get<const WorldTransform>(item.entity);
        const MeshBuffer* mesh = m_meshes.get(renderer.mesh);

        glm::vec3 tint(1.0f);
        if (const auto* material = scene.registry().try_get<MaterialComponent>(item.entity))
            tint = material->baseColor;

        m_translucentPipeline.push(cmd, MeshPushConstants{viewProj * world.matrix, world.matrix, tint});
        drawMesh(cmd, m_translucentPipeline, renderer.mesh, *mesh, overrideOf(scene, item.entity),
                 true, boundTexture, DrawPass::Translucent);
    }
}

void SceneRenderer::renderSelectionMask(VkCommandBuffer cmd,
                                        const Scene& scene,
                                        entt::entity selected,
                                        const glm::mat4& viewProj,
                                        VkExtent2D extent) {
    m_outlineActive = false;
    if (selected == entt::null || !scene.registry().valid(selected)) return;

    // The entity and everything below it in the hierarchy, that can be drawn.
    struct Item { const MeshBuffer* mesh; glm::mat4 world; };
    std::vector<Item> items;

    std::vector<entt::entity> pending{selected};
    while (!pending.empty()) {
        const entt::entity e = pending.back();
        pending.pop_back();

        if (const auto* renderer = scene.registry().try_get<MeshRenderer>(e)) {
            const auto* world = scene.registry().try_get<WorldTransform>(e);
            const MeshBuffer* mesh = m_meshes.get(renderer->mesh);
            if (world && mesh) items.push_back({mesh, world->matrix});
        }

        const Relationship& rel = scene.registry().get<Relationship>(e);
        for (entt::entity child = rel.firstChild; child != entt::null;
             child = scene.registry().get<Relationship>(child).nextSibling)
            pending.push_back(child);
    }
    if (items.empty()) return;   // a light or a camera, say: no shape to outline

    if (!m_outline.beginMask(cmd, extent)) return;
    for (const Item& item : items) {
        m_outline.pushMask(cmd, OutlineMaskPushConstants{viewProj * item.world});
        item.mesh->draw(cmd);
    }
    m_outline.endMask(cmd);
    m_outlineActive = true;
}

void SceneRenderer::renderPostProcess(VkCommandBuffer cmd, const OffscreenBuffer& target, const Scene& scene) {
    const SceneSettings& settings = scene.settings();

    // Keep this frame's picture for the next frame's reflections, before the
    // bloom and tone mapping change it.
    if (m_reflectionsDepth) {
        m_reflections.capture(cmd, target.hdrImage(), VkExtent2D{target.width(), target.height()});
        m_historyViewProj = m_viewProj;
    } else {
        m_reflections.invalidate();
    }

    PostSettings post;
    post.exposure       = std::clamp(settings.exposure, -6.0f, 6.0f);
    post.toneMapper     = static_cast<ToneMapper>(std::clamp(settings.toneMapper, 0, kToneMapperCount - 1));
    post.bloom          = settings.bloom;
    post.bloomIntensity = std::max(settings.bloomIntensity, 0.0f);
    post.bloomThreshold = std::max(settings.bloomThreshold, 0.0f);

    // The outline goes on the finished picture, after the tone mapping.
    const VkExtent2D extent{target.width(), target.height()};
    m_post.render(cmd, target, post, [this, extent](VkCommandBuffer commands) {
        if (m_outlineActive) m_outline.draw(commands, extent);
    });
}

// ---------------------------------------------------------------------------
// Reflection probes
// ---------------------------------------------------------------------------

namespace {

/// Frames the scene (or a probe) must stay unchanged before a probe is photographed
/// again: long enough not to re-photograph on every step of a drag.
constexpr uint32_t kProbeSettleFrames = 30;

/// Frames after the scene settles before the second photograph, which picks up
/// the textures that were still loading the first time.
constexpr uint32_t kProbeFollowUpFrames = 150;

/// Where the cube maps are photographed to: the far end of the faces' view.
constexpr float kProbeNear = 0.05f;
constexpr float kProbeFar  = 500.0f;

} // namespace

uint64_t SceneRenderer::hashScene(const Scene& scene) const {
    // FNV-1a over the numbers that decide what a photograph would show.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    };
    const auto mixValue = [&mix](const auto& value) { mix(&value, sizeof(value)); };

    const auto& registry = scene.registry();

    for (auto [entity, renderer, world] : registry.view<const MeshRenderer, const WorldTransform>().each()) {
        mixValue(renderer.mesh);
        mixValue(renderer.castShadows);
        mixValue(world.matrix);
        if (const auto* material = registry.try_get<MaterialComponent>(entity)) {
            mixValue(material->baseColor);
            mixValue(material->albedoTexture);
            mixValue(material->customSurface);
            mixValue(material->metallic);
            mixValue(material->roughness);
            mixValue(material->emissiveColor);
            mixValue(material->emissiveIntensity);
            mixValue(material->opacity);
        }
    }

    for (auto [entity, light, world] : registry.view<const LightComponent, const WorldTransform>().each()) {
        mixValue(light.type);
        mixValue(light.color);
        mixValue(light.intensity);
        mixValue(light.range);
        mixValue(light.castShadows);
        mixValue(world.matrix);
    }

    const SceneSettings& settings = scene.settings();
    mixValue(settings.ambientColor);
    mixValue(settings.ambientIntensity);
    mixValue(settings.environment);
    mixValue(settings.environmentIntensity);
    mixValue(settings.showSky);
    mixValue(settings.distanceFieldShadows);
    mixValue(settings.distanceFieldReflections);

    // What the project asks of the distance field changes what the probes see.
    const ProjectSettings& quality = project();
    mixValue(quality.distanceFieldDetail);
    mixValue(quality.distanceFieldResolution);
    mixValue(quality.distanceFieldReflectionQuality);
    mixValue(quality.distanceFieldReflectionReach);
    mixValue(quality.distanceFieldShadowBias);
    mixValue(quality.distanceFieldReflectedShadows);
    return hash;
}

void SceneRenderer::bakeProbe(VkCommandBuffer cmd, const Scene& scene, uint32_t slot,
                              const glm::vec3& position) {
    // The photograph is seen from the probe: the specular highlights need to
    // know where the eye is. Everything else of the frame's data stays (lights,
    // shadows, the sky), and the effects that need a screen or other probes are off.
    // The slot is written by the CPU, so the GPU must be done with its last use.
    vkDeviceWaitIdle(m_ctx->device());

    SceneUniformData data = m_frameData;
    data.cameraPosition = glm::vec4(position, 1.0f);
    data.ssrParams      = glm::vec4(0.0f);
    data.probeCount     = glm::ivec4(0);
    m_uniforms.update(SceneUniforms::kProbeSlot, data);

    SceneSets sets;
    sets.uniforms    = m_uniforms.set(SceneUniforms::kProbeSlot);
    sets.occlusion   = m_occlusion.whiteSet();
    sets.reflections = m_reflections.emptySet();
    sets.probes      = m_probes.set();
    sets.distanceField = m_globalField.sampleSet();

    const VkExtent2D extent{ReflectionProbes::kResolution, ReflectionProbes::kResolution};
    const glm::mat4  projection = ReflectionProbes::faceProjection(kProbeNear, kProbeFar);

    m_probes.beginCapture(cmd);
    for (uint32_t face = 0; face < ReflectionProbes::kFaces; ++face) {
        const glm::mat4 viewProj = projection * ReflectionProbes::faceView(face, position);

        Renderer::beginOffscreenPass(cmd, m_probeTarget);
        drawScene(cmd, scene, viewProj, extent, sets);
        Renderer::endOffscreenPass(cmd, m_probeTarget);

        m_probes.captureFace(cmd, m_probeTarget.hdrImage(), face);
    }
    m_probes.filter(cmd, slot);

    FE_INFO("SceneRenderer: photographed reflection probe %u at (%.1f, %.1f, %.1f)",
            slot, position.x, position.y, position.z);
}

void SceneRenderer::renderProbes(VkCommandBuffer cmd, const Scene& scene, uint32_t frameIndex) {
    // Switched off: nothing is photographed and the frame's data lists no probes.
    // Their slots are kept, so turning them on again finds the pictures it left
    // (and re-photographs them if the scene changed meanwhile).
    if (!scene.settings().reflectionProbes) return;

    // ---- Which probes are in the scene, and which slot each one uses ----
    struct Found {
        uint64_t                          id;
        glm::vec3                         position;
        const ReflectionProbeComponent*   probe;
    };
    std::array<Found, kMaxReflectionProbes> found{};
    uint32_t count = 0;

    auto probes = scene.registry().view<const ReflectionProbeComponent, const WorldTransform, const IDComponent>();
    for (auto [entity, probe, world, id] : probes.each()) {
        if (count >= kMaxReflectionProbes) {
            static bool warned = false;
            if (!warned) {
                FE_WARN("SceneRenderer: more than %u reflection probes in the scene; the extra ones are ignored",
                        kMaxReflectionProbes);
                warned = true;
            }
            break;
        }
        found[count++] = {static_cast<uint64_t>(id.uuid), glm::vec3(world.matrix[3]), &probe};
    }

    // A slot belongs to a probe for as long as it exists (found by its UUID).
    // Slots of probes that are gone are freed; new probes take a free one.
    std::array<int, kMaxReflectionProbes> slotOf{};
    slotOf.fill(-1);
    for (ProbeSlot& slot : m_probeSlots) {
        bool alive = false;
        for (uint32_t i = 0; i < count; ++i) alive |= (found[i].id == slot.id);
        if (!alive) slot = ProbeSlot{};
    }
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t s = 0; s < kMaxReflectionProbes; ++s)
            if (m_probeSlots[s].id == found[i].id) { slotOf[i] = static_cast<int>(s); break; }
        if (slotOf[i] >= 0) continue;
        for (uint32_t s = 0; s < kMaxReflectionProbes; ++s) {
            if (m_probeSlots[s].id != 0) continue;
            m_probeSlots[s] = ProbeSlot{};
            m_probeSlots[s].id           = found[i].id;
            m_probeSlots[s].lastPosition = found[i].position;
            slotOf[i] = static_cast<int>(s);
            break;
        }
    }
    if (count == 0) return;   // the frame's data already lists no probes

    // ---- When the scene has changed, every probe is photographed again, once it settles ----
    const uint64_t hash = hashScene(scene);
    if (hash != m_probeSceneHash) {
        m_probeSceneHash   = hash;
        m_probeSceneStill  = 0;
        m_probeStale       = true;
        m_probeStaleFollow = false;
        m_probeFollowUp    = 0;
    } else if (m_probeSceneStill < kProbeSettleFrames) {
        ++m_probeSceneStill;
    }
    if (m_probeFollowUp > 0 && --m_probeFollowUp == 0) {
        m_probeStale       = true;
        m_probeStaleFollow = true;
    }
    if (m_probeStale && m_probeSceneStill >= kProbeSettleFrames) {
        for (ProbeSlot& slot : m_probeSlots)
            if (slot.id != 0) slot.dirty = true;
        m_probeStale = false;
        if (!m_probeStaleFollow) m_probeFollowUp = kProbeFollowUpFrames;
    }

    // ---- One photograph per frame at most ----
    bool baked = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (slotOf[i] < 0) continue;
        ProbeSlot& slot = m_probeSlots[static_cast<uint32_t>(slotOf[i])];

        // A moved probe waits until it stays still (a drag moves it every frame).
        slot.stillFrames = (found[i].position == slot.lastPosition) ? slot.stillFrames + 1 : 0;
        slot.lastPosition = found[i].position;
        const bool moved = slot.baked && slot.stillFrames >= kProbeSettleFrames &&
                           glm::distance(found[i].position, slot.bakedPosition) > 1e-3f;

        const bool needed = !slot.baked || slot.dirty || moved ||
                            found[i].probe->bakeSerial != slot.bakedSerial;
        if (!needed || baked) continue;

        bakeProbe(cmd, scene, static_cast<uint32_t>(slotOf[i]), found[i].position);
        slot.baked         = true;
        slot.dirty         = false;
        slot.bakedPosition = found[i].position;
        slot.bakedSerial   = found[i].probe->bakeSerial;
        baked = true;
    }

    // ---- The probes that are ready go to the shaders ----
    SceneUniformData data = m_frameData;
    int used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (slotOf[i] < 0) continue;
        const ProbeSlot& slot = m_probeSlots[static_cast<uint32_t>(slotOf[i])];
        if (!slot.baked) continue;

        const ReflectionProbeComponent& probe = *found[i].probe;
        const glm::vec3 half = 0.5f * glm::max(probe.boxSize, glm::vec3(0.5f));
        data.probeBoxMin[used]   = glm::vec4(found[i].position - half, std::max(probe.intensity, 0.0f));
        data.probeBoxMax[used]   = glm::vec4(found[i].position + half, std::max(probe.blendDistance, 0.0f));
        data.probePosition[used] = glm::vec4(slot.bakedPosition, probe.boxProjection ? 1.0f : 0.0f);
        data.probeLayer[used]    = glm::vec4(static_cast<float>(slotOf[i]), 0.0f, 0.0f, 0.0f);
        ++used;
    }
    data.probeCount = glm::ivec4(used, 0, 0, 0);
    m_frameData = data;
    m_uniforms.update(frameIndex, data);
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
