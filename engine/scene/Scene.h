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
 * @brief A scene: a collection of entities with components.
 *
 * The scene owns an entt::registry and maintains a hierarchy of entities
 * through Relationship components. It also keeps a UUID → entity map for
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
 * @note Scene is not copyable. Each scene owns its registry and all the
 *       entities within it.
 */
class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

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
     * Use this accessor — rather than registry().get<Transform>() — when
     * modifying a transform so that WorldTransform is recomputed next frame.
     *
     * @param e A valid entity with a Transform component.
     * @return Reference to the mutable local transform.
     */
    Transform& transform(entt::entity e);

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

    /// @return Display name of this scene (shown in the title bar).
    const std::string& name() const { return m_name; }

    /// @brief Sets the display name of this scene.
    void setName(std::string n) { m_name = std::move(n); }

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
};

} // namespace fe
