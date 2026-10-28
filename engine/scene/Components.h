#pragma once

/**
 * @file Components.h
 * @brief All built-in ECS components and their JSON serialisation helpers.
 *
 * Every component that should appear in the editor's Add Component menu and
 * be saved to disk is registered in ComponentRegistry (see ComponentRegistry.h).
 * Adding a new component requires:
 *   1. Declaring the struct here.
 *   2. Providing NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (or manual overloads).
 *   3. Declaring an inspect() overload (implemented in Inspectors.cpp).
 *   4. Calling ComponentRegistry::instance().add<T>() in registerBuiltinComponents().
 */

#include "core/Log.h"
#include "import/AssetDatabase.h"
#include "scene/UUID.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <cstdint>


// GLM serialisation overloads must live in the glm namespace so that
// nlohmann's ADL lookup finds them when serialising glm types embedded
// in component structs.
namespace glm {
    void to_json(nlohmann::json& j, const vec3& v);
    void from_json(const nlohmann::json& j, vec3& v);
    void to_json(nlohmann::json& j, const vec4& v);
    void from_json(const nlohmann::json& j, vec4& v);
    void to_json(nlohmann::json& j, const quat& q);
    void from_json(const nlohmann::json& j, quat& q);
} // namespace glm

namespace fe {

using json         = nlohmann::json;
using AssetHandle  = std::uint64_t;

/// Reserved asset handles for the primitive meshes built into the engine.
/// Temporary: a real asset manager will replace this.
namespace builtin {
constexpr AssetHandle MeshNone  = 0;
constexpr AssetHandle MeshCube  = 1;
constexpr AssetHandle MeshPlane = 2;
} // namespace builtin

// ---------------------------------------------------------------------------
// Identity and hierarchy
// ---------------------------------------------------------------------------

/**
 * @brief Persistent identifier for an entity.
 *
 * Unlike an entt::entity handle, a UUID survives save/load cycles and
 * copy-paste operations. Every entity has exactly one IDComponent.
 */
struct IDComponent {
    UUID uuid = 0;  ///< Non-zero unique identifier.
};

/**
 * @brief Human-readable label shown in the scene hierarchy.
 *
 * Every entity has exactly one NameComponent; it is created automatically
 * by Scene::create().
 */
struct NameComponent {
    std::string name = "Entity";  ///< Display name (not necessarily unique).
};

/**
 * @brief Intrusive linked-list node for the scene hierarchy.
 *
 * Together these four handles form a doubly-linked tree. The root level
 * entities are chained through nextSibling/prevSibling with parent ==
 * entt::null.
 *
 * @note This component is managed exclusively by Scene. Do not modify
 *       it directly; use Scene::setParent() instead.
 */
struct Relationship {
    entt::entity parent      = entt::null;  ///< Parent entity, or null for roots.
    entt::entity firstChild  = entt::null;  ///< First child in the sibling chain.
    entt::entity nextSibling = entt::null;  ///< Next sibling, or null.
    entt::entity prevSibling = entt::null;  ///< Previous sibling, or null.
    std::uint32_t childCount = 0;           ///< Number of direct children.
};

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

/**
 * @brief Local position, rotation and scale of an entity.
 *
 * Edited by the user through the inspector or gizmos. The scene propagates
 * these values into WorldTransform once per frame.
 *
 * @see WorldTransform
 */
struct Transform {
    glm::vec3 position{0.0f};                        ///< Local position.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};     ///< Local rotation.
    glm::vec3 scale{1.0f};                           ///< Local scale.

    /**
     * @brief Computes the local-space transformation matrix.
     * @return T * R * S matrix combining position, rotation and scale.
     */
    glm::mat4 localMatrix() const {
        return glm::translate(glm::mat4(1.0f), position)
             * glm::mat4_cast(rotation)
             * glm::scale(glm::mat4(1.0f), scale);
    }
};

/**
 * @brief Cached world-space transformation matrix.
 *
 * Recomputed by Scene::updateTransforms() whenever dirty is true.
 * Do not write to this component directly; modify Transform instead and
 * let the scene propagate the change.
 *
 * @see Transform
 */
struct WorldTransform {
    glm::mat4 matrix{1.0f};  ///< World-space TRS matrix.
    bool dirty = true;        ///< True when the local transform has changed.
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/**
 * @brief Requests rendering of a static mesh with a given material.
 *
 * The renderer looks for this component when building the draw list.
 * Both handles are zero (invalid) until the user assigns an asset.
 */
struct MeshRenderer {
    AssetHandle mesh     = 0;         ///< Built-in mesh (fe::builtin) or the id of a model asset (see AssetDatabase).
    AssetHandle material = 0;         ///< Handle to a Material asset.
    bool        castShadows = true;   ///< Whether this mesh casts shadows.
};

/**
 * @brief How the surface of an entity's mesh looks.
 *
 * Every entity with a MeshRenderer has one (the scene adds it), and its
 * defaults change nothing: the mesh is drawn as its own materials say. What
 * it holds takes the place of what the model says, never the other way round.
 *
 * ### Colour
 * The final colour of a pixel is `vertex colour * baseColor * albedo texture`.
 *
 * ### Textures
 * Each slot is the id of an image asset (see AssetDatabase), or 0 to keep the
 * model's own texture for that slot (none, for the built-in shapes). An id,
 * not a path, so moving or renaming the image does not break the material.
 *
 * ### Surface values
 * Metalness, roughness, normal strength and emissive light come from the
 * model's materials (a built-in shape has plain defaults). With
 * @c customSurface on, the numbers below replace them on every part of the
 * object. The textures multiply whichever numbers apply, as in glTF.
 */
struct MaterialComponent {
    /// Tint, as sRGB colour (what the colour picker shows). Multiplies the
    /// vertex colour and the texture.
    glm::vec3 baseColor{1.0f};

    AssetHandle albedoTexture     = 0;   ///< Colour (sRGB).
    AssetHandle metalRoughTexture = 0;   ///< Roughness in green, metalness in blue (linear), as glTF.
    AssetHandle normalTexture     = 0;   ///< Tangent-space normal map (linear).
    AssetHandle emissiveTexture   = 0;   ///< Light the surface gives off (sRGB).

    /// Whether the values below are used. When off, the model's own are.
    bool customSurface = false;

    float     metallic       = 0.0f;   ///< 0 plastic, wood, stone... 1 bare metal.
    float     roughness      = 0.5f;   ///< 0 mirror, 1 completely matte.
    float     normalStrength = 1.0f;   ///< Scales how much the normal map bends the surface; 0 turns it off.
    glm::vec3 emissiveColor{0.0f};     ///< sRGB colour of the light the surface gives off.
    float     emissiveIntensity = 1.0f;///< Multiplies emissiveColor.

    /// How the transparency of the object is decided.
    enum class Transparency : int {
        Model  = 0,   ///< The model's own setting (solid, for the built-in shapes).
        Opaque = 1,   ///< Solid: the albedo's alpha is ignored.
        Cutout = 2,   ///< Pixels with alpha below @c alphaCutoff are not drawn; the rest are solid.
        Blend  = 3,   ///< Translucent: mixed with what is behind, by alpha and @c opacity.
    };

    Transparency transparency = Transparency::Model;
    float alphaCutoff = 0.5f;   ///< Cutout only: alpha below this is dropped.
    float opacity     = 1.0f;   ///< Multiplies the albedo's alpha (0 invisible, 1 as the texture says). Only has an effect on translucent surfaces.

    /// Draw the back faces as well (leaves, cloth, glass seen from inside).
    /// The model's own double-sided setting applies even when this is off.
    bool  doubleSided = false;
};

/**
 * @brief Marks an entity as the scene's primary camera.
 *
 * The renderer uses the first entity it finds with primary == true.
 */
struct CameraComponent {
    float fovDegrees = 60.0f;   ///< Vertical field of view in degrees.
    float nearPlane  = 0.1f;    ///< Near clipping plane distance.
    float farPlane   = 1000.0f; ///< Far clipping plane distance.
    bool  primary    = true;    ///< Whether this is the active camera.
};

/**
 * @brief A light source that contributes to scene illumination.
 *
 * Directional and spot lights shine along the entity's local -Z axis, like
 * a camera looks, so rotating the entity aims them. Point and spot lights
 * use the entity's position.
 */
struct LightComponent {
    /// Light source type.
    enum class Type : int {
        Directional = 0,  ///< Parallel rays from an infinite distance.
        Point       = 1,  ///< Omnidirectional point source.
        Spot        = 2   ///< Cone-shaped point source.
    };

    Type      type      = Type::Point;   ///< Light type.
    glm::vec3 color     {1.0f};          ///< Linear RGB emission colour.
    float     intensity = 1.0f;          ///< Brightness multiplier.
    float     range     = 10.0f;         ///< Maximum influence radius (Point/Spot).

    /// Spot only. Half-angle, in degrees, of the cone that is fully lit.
    float     innerConeDegrees = 25.0f;

    /// Spot only. Half-angle, in degrees, where the light has faded to nothing.
    /// Never smaller than innerConeDegrees.
    float     outerConeDegrees = 30.0f;

    /// Whether this light casts shadows. The scene has room for
    /// kMaxShadowCasters directional and spot lights and kMaxPointShadowCasters
    /// point lights that do.
    bool      castShadows = true;

    /// Size of the light, which sets how soft the edges of its shadows are.
    /// For a point or spot light it is the radius of the light itself, in
    /// world units (a bulb is around 0.05 to 0.2). For a directional light it
    /// is the angular radius in degrees (the real sun is about 0.27; larger
    /// values give softer shadows). The edge is sharp where a shadow starts at
    /// the object casting it and widens with the distance from it, and 0 gives
    /// the sharpest edge the shadow map can draw.
    float     shadowSoftness = 0.15f;
};

/**
 * @brief A reflection probe: a point in the scene that photographs its surroundings
 *        so shiny surfaces nearby can reflect them.
 *
 * Screen-space reflections (see ScreenReflections) only know what is on screen.
 * A probe stores what is all around its position in a small cube map, found by
 * rendering the scene six times from there. Shiny surfaces inside its box read
 * that cube map, with the lookup corrected so the reflection lines up with the
 * room (the box stands for the walls), instead of reflecting the sky.
 *
 * The probe takes its position from the entity. It re-photographs the scene on
 * its own a moment after the scene changes (moved objects, lights, textures
 * finishing loading) and on the Bake button.
 */
struct ReflectionProbeComponent {
    /// Brightness multiplier of what the probe reflects.
    float intensity = 1.0f;

    /// Size of the box where the probe applies, in world units, centred on the
    /// entity (it is not rotated or scaled with the entity). It is also the
    /// stand-in for the room's walls when correcting the reflections' parallax.
    glm::vec3 boxSize{10.0f, 6.0f, 10.0f};

    /// Width of the soft border, inside the box, over which the probe fades
    /// into the sky's reflection (or into a neighbouring probe).
    float blendDistance = 1.0f;

    /// Whether the reflection is corrected for the box (true) or read as if the
    /// environment were infinitely far away (false, like the sky).
    bool boxProjection = true;

    /// Raised by the Inspector's Bake button; the renderer re-photographs the
    /// probe when it changes. Not saved.
    uint32_t bakeSerial = 0;
};

// ---------------------------------------------------------------------------
// JSON serialisation helpers for GLM types
//
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE cannot be used for GLM types because
// they are not our own structs. We provide explicit to_json/from_json
// overloads in the fe namespace so that ADL finds them when serialising
// any component that contains GLM members.
// ---------------------------------------------------------------------------

// Component serialisation — generates to_json/from_json for each struct.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NameComponent, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transform, position, rotation, scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ReflectionProbeComponent, intensity, boxSize, blendDistance, boxProjection)

// MeshRenderer and MaterialComponent are written by hand: scenes saved before
// assets had stable ids hold a hash of the model's path and the texture's path
// as text, and loading must turn those into ids (see AssetDatabase).
inline void to_json(json& j, const MeshRenderer& m) {
    j = json{{"mesh", m.mesh}, {"material", m.material}, {"castShadows", m.castShadows}};
}
inline void from_json(const json& j, MeshRenderer& m) {
    m.mesh = AssetDatabase::instance().upgradeMeshHandle(j.at("mesh").get<AssetHandle>());
    j.at("material").get_to(m.material);
    j.at("castShadows").get_to(m.castShadows);
}

inline void to_json(json& j, const MaterialComponent& m) {
    j = json{{"baseColor", m.baseColor},
             {"albedoTexture", m.albedoTexture},
             {"metalRoughTexture", m.metalRoughTexture},
             {"normalTexture", m.normalTexture},
             {"emissiveTexture", m.emissiveTexture},
             {"customSurface", m.customSurface},
             {"metallic", m.metallic},
             {"roughness", m.roughness},
             {"normalStrength", m.normalStrength},
             {"emissiveColor", m.emissiveColor},
             {"emissiveIntensity", m.emissiveIntensity},
             {"transparency", static_cast<int>(m.transparency)},
             {"alphaCutoff", m.alphaCutoff},
             {"opacity", m.opacity},
             {"doubleSided", m.doubleSided}};
}

/// Reads a texture id stored under @p key: a number, or (scenes saved before
/// assets had ids) a path relative to the asset folder. 0 if absent.
inline AssetHandle readTextureId(const json& j, const char* key) {
    const auto texture = j.find(key);
    if (texture == j.end()) return 0;

    if (texture->is_string()) {
        const std::string path = texture->get<std::string>();
        if (path.empty()) return 0;
        const AssetHandle id = AssetDatabase::instance().idFromLegacyPath(path);
        if (id == 0) FE_WARN("Material: texture '%s' was not found", path.c_str());
        return id;
    }
    if (texture->is_number_unsigned()) return texture->get<AssetHandle>();
    return 0;
}

inline void from_json(const json& j, MaterialComponent& m) {
    // Every key is optional: scenes saved before a field existed still load.
    const MaterialComponent defaults;
    m.baseColor         = j.value("baseColor", defaults.baseColor);
    m.albedoTexture     = readTextureId(j, "albedoTexture");
    m.metalRoughTexture = readTextureId(j, "metalRoughTexture");
    m.normalTexture     = readTextureId(j, "normalTexture");
    m.emissiveTexture   = readTextureId(j, "emissiveTexture");
    m.customSurface     = j.value("customSurface", defaults.customSurface);
    m.metallic          = j.value("metallic", defaults.metallic);
    m.roughness         = j.value("roughness", defaults.roughness);
    m.normalStrength    = j.value("normalStrength", defaults.normalStrength);
    m.emissiveColor     = j.value("emissiveColor", defaults.emissiveColor);
    m.emissiveIntensity = j.value("emissiveIntensity", defaults.emissiveIntensity);
    const int transparency = j.value("transparency", static_cast<int>(defaults.transparency));
    m.transparency = (transparency >= 0 && transparency <= 3)
                   ? static_cast<MaterialComponent::Transparency>(transparency)
                   : defaults.transparency;
    m.alphaCutoff       = j.value("alphaCutoff", defaults.alphaCutoff);
    m.opacity           = j.value("opacity", defaults.opacity);
    m.doubleSided       = j.value("doubleSided", defaults.doubleSided);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraComponent, fovDegrees, nearPlane, farPlane, primary)

// The WITH_DEFAULT variant fills in any key the file does not have. Scenes
// saved before the cone angles were in degrees hold "innerCone" and
// "outerCone" instead; they still load, with the default angles.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LightComponent, type, color, intensity, range,
                                                innerConeDegrees, outerConeDegrees, castShadows,
                                                shadowSoftness)

// ---------------------------------------------------------------------------
// Inspector forward declarations
//
// Each component has an inspect() overload that draws its ImGui widgets.
// Implementations live in engine/scene/Inspectors.cpp so that ImGui
// headers are not pulled into every translation unit that includes this file.
// ---------------------------------------------------------------------------

bool inspect(NameComponent&);
bool inspect(Transform&);
bool inspect(MeshRenderer&);
bool inspect(MaterialComponent&);
bool inspect(CameraComponent&);
bool inspect(LightComponent&);
bool inspect(ReflectionProbeComponent&);

} // namespace fe
