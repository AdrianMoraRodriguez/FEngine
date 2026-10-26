#pragma once

/**
 * @file SceneRenderer.h
 * @brief Draws the entities of a Scene into the current render pass.
 */

#include "core/Geometry.h"
#include "gfx/AmbientOcclusion.h"
#include "gfx/Environment.h"
#include "gfx/MeshLibrary.h"
#include "gfx/MeshPipeline.h"
#include "gfx/OutlinePass.h"
#include "gfx/SceneUniforms.h"
#include "gfx/TextureLibrary.h"
#include "gfx/ShadowCascades.h"
#include "gfx/ShadowCube.h"
#include "gfx/ShadowMap.h"
#include "gfx/ShadowPipeline.h"
#include "gfx/SkyPipeline.h"
#include "scene/Scene.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <future>

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
 * old scene is never dark. Surfaces are shaded per pixel with a physically
 * based model (see mesh.frag): Lambert diffuse plus a GGX specular lobe.
 *
 * ### Environment
 * The scene also stands in a sky (SceneSettings::environment): a panorama that
 * lights every surface from all around (the diffuse part) and shows up as
 * reflections on shiny ones, and that is drawn behind the scene. Without a
 * picture chosen, a built-in gradient sky is used. A picture is read and baked
 * on a background thread (see EnvironmentBake) and replaces the sky in use
 * once it is ready, so choosing a big file never stalls the editor.
 *
 * ### Transparency
 * A material is solid, cut-out or translucent (AlphaMode). Solid and cut-out
 * surfaces go through the normal pass, in any order. Once the sky is drawn,
 * the translucent ones are drawn over everything, farthest object first, with
 * their colour mixed into what is behind. They are sorted per object (by the
 * centre of its bounding box), so two translucent objects that interpenetrate,
 * or the parts of one object, can still show the wrong order. Translucent
 * surfaces cast no shadow and do not take part in ambient occlusion; cut-out
 * ones cast the shadow of their shape.
 *
 * ### Shadows
 * Every light with castShadows set casts shadows, while shadow maps last: up
 * to kMaxShadowCasters (4) directional and spot lights, and up to
 * kMaxPointShadowCasters (2) point lights, in the order the lights are
 * found. Every MeshRenderer with castShadows set is drawn from the light's
 * point of view into its shadow map, and the main pass darkens what the map
 * says is hidden from the light.
 *   - The first directional light that casts shadows (normally the sun) uses
 *     cascaded shadow maps: the camera's view is cut by distance into
 *     kShadowCascades slices up to kShadowDistance, each with a map of its
 *     own, so shadows stay sharp near the camera whatever the size of the
 *     scene. Any further directional light gets one map fitted to all the
 *     casters, which gets less sharp as the scene grows.
 *   - A spot light's map covers its cone, up to its range.
 *   - A point light needs six maps, one per direction (a cube), and covers
 *     everything within its range.
 * The default sun used by scenes without lights casts no shadows.
 *
 * Intensity is the light's brightness at about one unit of distance for point
 * and spot lights, which then fade with the square of the distance and reach
 * nothing at all beyond their range. Values between 5 and 30 are typical.
 */
class SceneRenderer {
public:
    /// How far from the camera the sun's shadows reach, in world units. Beyond
    /// it surfaces are lit with no shadow. Farther means each cascade is
    /// stretched over more ground and so is less sharp.
    static constexpr float kShadowDistance = 60.0f;

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
     * @brief Loads every mesh the scene uses that is not on the GPU yet, and
     *        keeps the scene's environment up to date.
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
     * @param cmd              Command buffer from Renderer::currentCommandBuffer().
     * @param scene            Scene to draw.
     * @param frameIndex       Renderer::currentFrameIndex().
     * @param cameraView       View matrix of the camera that will render the scene.
     * @param cameraProjection Its perspective projection (Vulkan depth range, as
     *                         the cameras produce). The sun's cascades follow
     *                         what this camera sees; with any other projection
     *                         the sun casts no shadows.
     */
    void renderShadows(VkCommandBuffer cmd, const Scene& scene, uint32_t frameIndex,
                       const glm::mat4& cameraView, const glm::mat4& cameraProjection);

    /**
     * @brief Computes the ambient occlusion that render() applies.
     *
     * Draws the scene's depth from the camera, estimates how closed each
     * pixel is (see AmbientOcclusion) and blurs it. Call every frame, outside
     * any render pass, before the scene pass. If it is not called (or the
     * scene turns the effect off, or the extent is empty) render() simply
     * applies no occlusion.
     *
     * @param cmd        Command buffer from Renderer::currentCommandBuffer().
     * @param scene      Scene to draw; its settings say whether and how strongly.
     * @param viewProj   Combined view-projection matrix of the camera.
     * @param projection The camera's projection alone (Vulkan clip space).
     * @param extent     Size of the render target; the occlusion has the same size.
     */
    void renderAmbientOcclusion(VkCommandBuffer cmd, const Scene& scene,
                                const glm::mat4& viewProj, const glm::mat4& projection,
                                VkExtent2D extent);

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
     * @brief Draws the selected entity's shape into the outline mask.
     *
     * The first half of the selection outline; see OutlinePass. Call every
     * frame, outside any render pass, before the scene pass. The entity's
     * children are included, so selecting a parent outlines the whole group.
     * Does nothing if @p selected is null or has no mesh to draw, in which
     * case drawSelectionOutline() draws nothing either.
     *
     * @param cmd      Command buffer from Renderer::currentCommandBuffer().
     * @param scene    Scene that holds the entity.
     * @param selected The selected entity, or entt::null.
     * @param viewProj Combined view-projection matrix of the camera.
     * @param extent   Size of the render target, which the mask follows.
     */
    void renderSelectionMask(VkCommandBuffer cmd,
                             const Scene& scene,
                             entt::entity selected,
                             const glm::mat4& viewProj,
                             VkExtent2D extent);

    /**
     * @brief Draws the selection outline over the scene.
     *
     * Call inside the scene pass, after render(). Draws nothing unless
     * renderSelectionMask() found something to outline this frame.
     */
    void drawSelectionOutline(VkCommandBuffer cmd, VkExtent2D extent);

    /**
     * @brief Draws one mesh, framed and lit on its own, into a small target.
     *
     * Used for the asset thumbnails. The mesh is seen from a fixed three-quarter
     * angle, from a distance that fits its bounding sphere in the picture,
     * under a fixed light, with no shadows. The target is cleared first and
     * left ready to be sampled as a texture.
     *
     * The model is drawn with its colour texture (each material's albedo, shrunk
     * to a small copy so that a folder of big models stays cheap), the way it
     * looks in the scene. Textures load in the background, so the first
     * pictures show the plain colours: @p settled tells the caller to draw
     * again later.
     *
     * Call outside any render pass, after renderShadows() (the shadow images
     * must already be in the layout the shader expects). The mesh must be on
     * the GPU: see MeshLibrary::ensureLoaded().
     *
     * @param handle Mesh to draw.
     * @param target Colour + depth target to draw into.
     * @param[out] settled If given, set to `true` when the picture is final, or
     *             `false` when a texture was still loading and a later redraw
     *             will look better.
     * @return `false` if the mesh is not available; nothing was drawn.
     */
    bool renderPreview(VkCommandBuffer cmd, AssetHandle handle, const OffscreenBuffer& target,
                       bool* settled = nullptr);

    /**
     * @brief Draws one mesh through a camera of the caller's choosing, under the preview light.
     *
     * The same pass as renderPreview() (fixed light, no shadows, the target
     * cleared first and left ready to be sampled), but the framing comes from
     * @p viewProj: the model viewer orbits around the mesh with it.
     *
     * @param viewProj Projection * view, with the Vulkan Y flip already applied.
     * @param model    Placement of the mesh in the world (identity by default). The
     *                 model viewer turns the mesh with it and keeps the camera, and
     *                 so the light, fixed.
     * @param textured `true` to draw the model with its textures. Thumbnails turn it off,
     *                 so that a folder full of models does not load all their textures.
     * @return `false` if the mesh is not on the GPU; nothing was drawn.
     */
    bool renderMeshView(VkCommandBuffer cmd, AssetHandle handle,
                        const OffscreenBuffer& target, const glm::mat4& viewProj,
                        const glm::mat4& model = glm::mat4(1.0f),
                        bool textured = true);

    /// @return The outline's settings (colour and thickness).
    OutlinePass& outline() { return m_outline; }

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

    /// The textures that materials use.
    TextureLibrary& textures() { return m_textures; }

private:
    /// What an entity's MaterialComponent puts in place of the model's own
    /// materials. The default replaces nothing.
    struct MaterialOverride {
        /// Asset ids per TextureLibrary::Slot; 0 keeps the model's texture.
        std::array<uint64_t, TextureLibrary::SlotCount> textures{};
        bool      custom      = false;   ///< Whether the numbers below replace the model's.
        float     metallic    = 0.0f;
        float     roughness   = 0.5f;
        float     normalScale = 1.0f;
        glm::vec3 emissive{0.0f};        ///< Linear light, already multiplied by the intensity.

        MaterialComponent::Transparency transparency = MaterialComponent::Transparency::Model;
        float alphaCutoff = 0.5f;        ///< Cutout only.
        float opacity     = 1.0f;
        bool  doubleSided = false;

        /// Applies the override to a material built from the model.
        void applyTo(TextureLibrary::MaterialRequest& request) const;

        /// @return The alpha mode of a part whose model material says @p fromModel.
        AlphaMode alphaModeOf(AlphaMode fromModel) const;

        /// Builds the override from an entity's component.
        static MaterialOverride from(const MaterialComponent& component);
    };

    /// Which parts of an object a drawing pass takes.
    enum class DrawPass {
        Solid,         ///< Everything that is not translucent.
        Translucent,   ///< Only the translucent parts (AlphaMode::Blend).
    };

    /**
     * @brief Draws @p mesh, one range per material of the model.
     *
     * Binds the material of each range (rebinding only when it changes).
     * Push constants must already be set, on the same @p pipeline.
     *
     * @param pipeline       The pipeline that is bound for this pass.
     * @param material       What the entity's MaterialComponent replaces in the
     *                       model's materials (see MaterialOverride).
     * @param textured       `false` draws everything with the default material.
     * @param boundMaterial  The set currently bound; updated.
     * @param pass           Which parts to draw: translucent ones go in a pass of their own.
     */
    void drawMesh(VkCommandBuffer cmd, const MeshPipeline& pipeline, AssetHandle handle,
                  const MeshBuffer& mesh, const MaterialOverride& material, bool textured,
                  VkDescriptorSet& boundMaterial, DrawPass pass);

    /// The material of one part (@p part null: the whole mesh, which has no
    /// material of its own), with the entity's override applied.
    TextureLibrary::MaterialRequest resolvePart(AssetHandle handle, const ModelParts* parts,
                                                const MeshPart* part,
                                                const MaterialOverride& material) const;

    /// @return `true` if some part of the object is translucent.
    bool hasTranslucentParts(AssetHandle handle, const MaterialOverride& material) const;

    /**
     * @brief Draws the solid parts of an object into the depth-only pass that is open.
     *
     * Cut-out parts use the pipeline that reads their alpha; translucent
     * parts are left out (they cast no shadow and do not occlude).
     *
     * @param mvp Transform from the object to the clip space of the pass.
     */
    void drawDepthOnly(VkCommandBuffer cmd, AssetHandle handle, const MeshBuffer& mesh,
                       const MaterialOverride& material, const glm::mat4& mvp);

    /// The material override of an entity (the default one if it has no material).
    static MaterialOverride overrideOf(const Scene& scene, entt::entity entity);

    /// Fills the preview slot of the uniforms with the light renderPreview() uses.
    void setupPreviewLighting();

    /// Starts and finishes the background bakes of the default sky and of the
    /// scene's environment. Called by prepare().
    void updateEnvironment(const Scene& scene);

    /// @return The environment the scene pass uses right now.
    VkDescriptorSet activeEnvironment(const Scene& scene) const;

    /// Collects the scene's lights into the block the shaders read. Also
    /// gives each shadow-casting light a shadow map slot (GpuLight::spotCone.z).
    static SceneUniformData gatherLights(const Scene& scene);

    /// Finds a sphere around every shadow caster in the scene.
    /// @return `false` if there is no caster.
    bool casterSphere(const Scene& scene, glm::vec3& outCenter, float& outRadius) const;

    /// Builds the view-projection of a directional shadow light so that it
    /// covers every shadow caster. @return `false` if there is no caster.
    bool fitDirectionalShadow(const Scene& scene, const glm::vec3& lightDirection,
                              glm::mat4& outViewProj, float& outRadius) const;

    /// Draws every shadow caster into the map that is currently open.
    void drawShadowCasters(VkCommandBuffer cmd, const Scene& scene,
                           const glm::mat4& lightViewProj);

    std::array<ShadowMap,  kMaxShadowCasters>      m_shadowMaps;
    std::array<ShadowCube, kMaxPointShadowCasters> m_shadowCubes;
    ShadowCascades m_shadowCascades;
    ShadowPipeline m_shadowPipeline;
    SceneUniforms  m_uniforms;
    MeshPipeline   m_pipeline;
    MeshPipeline   m_translucentPipeline;   ///< The same shader, blended and without depth writes.
    OutlinePass    m_outline;
    bool           m_outlineActive = false;   ///< A mask was drawn this frame.
    /// Set while a preview is drawn: materials then use a small copy of the
    /// albedo texture only (see resolvePart()).
    bool           m_previewTextures = false;
    MeshLibrary    m_meshes;
    TextureLibrary m_textures;

    // ---- Environment ----
    const VulkanContext*  m_ctx = nullptr;
    VkDescriptorSetLayout m_environmentLayout = VK_NULL_HANDLE;   ///< Set 2 of the mesh pipeline.
    SkyPipeline           m_sky;
    AmbientOcclusion      m_occlusion;            ///< Set 3 of the mesh pipeline.
    bool                  m_occlusionValid = false;   ///< renderAmbientOcclusion() produced this frame's result.
    Environment           m_defaultEnvironment;   ///< The built-in sky: used when the scene has none, and by previews.
    Environment           m_sceneEnvironment;     ///< The picture the scene asked for, once baked.

    std::future<EnvironmentBake> m_defaultBake;   ///< Full-quality bake of the built-in sky, running.
    std::future<EnvironmentBake> m_sceneBake;     ///< Bake of the scene's picture, running.
    AssetHandle m_sceneBakeId       = 0;          ///< The picture that m_sceneBake is baking.
    AssetHandle m_loadedEnvironment = 0;          ///< The picture m_sceneEnvironment holds (0 = none).
    AssetHandle m_failedEnvironment = 0;          ///< A picture that failed to load; not retried.
};

} // namespace fe
