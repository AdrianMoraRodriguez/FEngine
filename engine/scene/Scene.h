#pragma once

/**
 * @file Scene.h
 * @brief Scene graph backed by an EnTT registry.
 */

#include "scene/Components.h"

#include <entt/entt.hpp>
#include <unordered_map>
#include <string>

namespace fe {

/**
 * @brief Settings that belong to the scene as a whole, not to any entity.
 *
 * Saved in the scene file. Edited in the Inspector while no entity is selected.
 */
struct SceneSettings {
    /// Colour of the ambient light: the light that reaches every surface from
    /// every direction, so that what no light shines on is not pitch black.
    glm::vec3 ambientColor{1.0f};

    /// Brightness of the ambient light. The ambient light's final colour is
    /// ambientColor * ambientIntensity.
    float ambientIntensity = 0.25f;

    /// The panorama that lights the scene and shows behind it: the id of an
    /// image in the asset folder (a Radiance .hdr gives the best result, as it
    /// can hold the sun's real brightness), or 0 for the built-in sky.
    /// This light comes on top of the ambient light above.
    AssetHandle environment = 0;

    /// Brightness of the environment's light (and of the sky shown behind the
    /// scene). 0 turns the environment's light off.
    float environmentIntensity = 1.0f;

    /// Whether the environment is drawn as the background of the scene. Turning
    /// it off leaves the clear colour; the environment still lights the objects.
    bool showSky = true;

    /// Whether ambient occlusion darkens the ambient and sky light in creases
    /// and under objects (see AmbientOcclusion).
    bool ambientOcclusion = true;

    /// How far from a surface, in world units, nearby geometry still counts
    /// as blocking its light. Small values give tight contact shadows, large
    /// ones broad darkening.
    float aoRadius = 0.5f;

    /// Strength of the occlusion: 1 is the plain estimate, more darkens the
    /// occluded parts further, 0 turns the effect off.
    float aoIntensity = 1.5f;
};

// The WITH_DEFAULT variant fills in any key the file does not have, so a scene
// file saved before a setting existed still loads.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SceneSettings, ambientColor, ambientIntensity,
                                                environment, environmentIntensity, showSky,
                                                ambientOcclusion, aoRadius, aoIntensity)

/**
 * @brief A scene: a collection of entities with components.
 *
 * The scene owns an entt::registry and maintains a hierarchy of entities
 * through Relationship components. It also keeps a UUID to entity map for
 * O(1) lookups by persistent identifier.
 *
 * Typical usage:
 * @code
 * fe::Scene scene;
 * entt::entity camera = scene.create("Main Camera");
 * scene.registry().emplace<fe::CameraComponent>(camera);
 * scene.updateTransforms();
 * @endcode
 *
 * @note Scene is movable but not copyable. Moving lets the editor replace a
 *       scene's contents while panels keep a reference to the same object.
 */
class Scene {
public:
    /// Makes every entity that gets a MeshRenderer get a MaterialComponent too
    /// (see Scene.cpp), so an object is always ready to be tinted or textured.
    Scene();
    ~Scene() = default;

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

    // Movable, so the editor can replace a scene's contents (New / Open) while
    // panels keep their reference to the same Scene object.
    Scene(Scene&&)            = default;
    Scene& operator=(Scene&&) = default;

    /**
     * @brief Creates a new entity with IDComponent, NameComponent,
     *        Relationship, Transform and WorldTransform.
     *
     * @param name   Display name shown in the hierarchy panel.
     * @param parent Parent entity, or entt::null for a root entity.
     * @return The new entity handle.
     */
    entt::entity create(const std::string& name = "Entity",
                        entt::entity parent = entt::null);

    /**
     * @brief Creates a new entity with a specific UUID.
     *
     * Used during deserialisation to restore persistent identifiers.
     *
     * @param uuid   The UUID to assign. Must not already be in use.
     * @param name   Display name.
     * @param parent Parent entity, or entt::null.
     * @return The new entity handle.
     */
    entt::entity createWithUUID(UUID uuid,
                                const std::string& name = "Entity",
                                entt::entity parent = entt::null);

    /**
     * @brief Destroys an entity and all its descendants.
     *
     * The entity is detached from its parent before destruction so the
     * hierarchy stays consistent.
     *
     * @param e The entity to destroy. Must be valid.
     */
    void destroy(entt::entity e);

    /**
     * @brief Moves @p child under @p newParent.
     *
     * Detaches child from its current parent and re-attaches it under
     * newParent. If keepWorldTransform is true, the local transform is
     * adjusted so that the world-space position/rotation/scale does not
     * change.
     *
     * Silently ignores the call if it would create a cycle (i.e. newParent
     * is a descendant of child).
     *
     * @param child              The entity to reparent.
     * @param newParent          The new parent, or entt::null to make it a root.
     * @param keepWorldTransform Whether to preserve world-space transform.
     */
    void setParent(entt::entity child,
                   entt::entity newParent,
                   bool keepWorldTransform = true);

    /**
     * @brief Propagates local transforms down the hierarchy.
     *
     * Must be called once per frame, before the renderer reads WorldTransform.
     * Only recomputes nodes whose dirty flag is set.
     */
    void updateTransforms();

    /**
     * @brief Returns the local transform of an entity and marks it dirty.
     *
     * Use this accessor, rather than registry().get<Transform>(), when
     * modifying a transform so that WorldTransform is recomputed next frame.
     *
     * @param e A valid entity with a Transform component.
     * @return Reference to the mutable local transform.
     */
    Transform& transform(entt::entity e);

    /**
     * @brief Sets an entity's transform from a world-space matrix.
     *
     * Converts @p world into the entity's local space (relative to its
     * parent's current world matrix) and stores it in the Transform
     * component. Used by viewport gizmos, which manipulate world matrices.
     *
     * The parent's WorldTransform must be up to date, which holds as long as
     * updateTransforms() runs once per frame.
     *
     * @param e     A valid entity.
     * @param world The desired world-space matrix.
     */
    void setWorldMatrix(entt::entity e, const glm::mat4& world);

    /**
     * @brief Looks up an entity by its persistent UUID.
     *
     * @param uuid The UUID to search for.
     * @return The matching entity, or entt::null if not found.
     */
    entt::entity findByUUID(UUID uuid) const;

    /// @return The first root entity in the hierarchy, or entt::null.
    entt::entity firstRoot() const { return m_firstRoot; }

    /// @return Mutable reference to the underlying EnTT registry.
    entt::registry& registry() { return m_registry; }

    /// @return Const reference to the underlying EnTT registry.
    const entt::registry& registry() const { return m_registry; }

    /// @return Display name of this scene.
    const std::string& name() const { return m_name; }

    /// @brief Sets the display name of this scene.
    void setName(std::string n) { m_name = std::move(n); }

    /// @return The scene-wide settings (ambient light, ...).
    SceneSettings& settings() { return m_settings; }

    /// @return The scene-wide settings (ambient light, ...).
    const SceneSettings& settings() const { return m_settings; }

private:
    void attach(entt::entity child, entt::entity parent);
    void detach(entt::entity e);
    void updateRecursive(entt::entity e,
                         const glm::mat4& parentWorld,
                         bool parentDirty);

    entt::registry m_registry;
    entt::entity   m_firstRoot = entt::null;

    std::unordered_map<UUID, entt::entity> m_byUUID;
    std::string m_name = "Untitled";
    SceneSettings m_settings;
};

} // namespace fe
