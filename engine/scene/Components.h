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

#include "scene/UUID.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <cstdint>

namespace fe {

using json         = nlohmann::json;
using AssetHandle  = std::uint64_t;

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
    AssetHandle mesh     = 0;         ///< Handle to a Mesh asset.
    AssetHandle material = 0;         ///< Handle to a Material asset.
    bool        castShadows = true;   ///< Whether this mesh casts shadows.
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
    float     innerCone = 0.9f;          ///< Cosine of the inner cone angle (Spot).
    float     outerCone = 0.7f;          ///< Cosine of the outer cone angle (Spot).
};

// ---------------------------------------------------------------------------
// JSON serialisation helpers for GLM types
//
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE cannot be used for GLM types because
// they are not our own structs. We provide explicit to_json/from_json
// overloads in the fe namespace so that ADL finds them when serialising
// any component that contains GLM members.
// ---------------------------------------------------------------------------

void to_json(json& j, const glm::vec3& v);
void from_json(const json& j, glm::vec3& v);
void to_json(json& j, const glm::vec4& v);
void from_json(const json& j, glm::vec4& v);
void to_json(json& j, const glm::quat& q);
void from_json(const json& j, glm::quat& q);

// Component serialisation — generates to_json/from_json for each struct.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NameComponent, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transform, position, rotation, scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MeshRenderer, mesh, material, castShadows)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraComponent, fovDegrees, nearPlane, farPlane, primary)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LightComponent, type, color, intensity, range, innerCone, outerCone)

// ---------------------------------------------------------------------------
// Inspector forward declarations
//
// Each component has an inspect() overload that draws its ImGui widgets.
// Implementations live in engine/scene/Inspectors.cpp so that ImGui
// headers are not pulled into every translation unit that includes this file.
// ---------------------------------------------------------------------------

void inspect(NameComponent&);
void inspect(Transform&);
void inspect(MeshRenderer&);
void inspect(CameraComponent&);
void inspect(LightComponent&);

} // namespace fe
