#include "scene/Scene.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <vector>

namespace fe {
namespace {

/// Splits an affine matrix into position, rotation and scale.
///
/// Shared by setParent() and setWorldMatrix(). Shear and mirroring are not
/// representable by a Transform and are discarded.
void decomposeInto(Transform& t, const glm::mat4& m) {
    t.position = glm::vec3(m[3]);

    const glm::vec3 sx{m[0]}, sy{m[1]}, sz{m[2]};
    t.scale = { glm::length(sx), glm::length(sy), glm::length(sz) };

    // A collapsed axis has no direction, so the rotation cannot be recovered.
    // Keep the previous rotation instead of dividing by zero.
    const float eps = 1e-6f;
    if (t.scale.x > eps && t.scale.y > eps && t.scale.z > eps) {
        const glm::mat3 rot{ sx / t.scale.x, sy / t.scale.y, sz / t.scale.z };
        t.rotation = glm::quat_cast(rot);
    }
}

/// Called by the registry whenever a MeshRenderer is added to an entity: gives
/// the entity a default MaterialComponent if it has none. It covers every way
/// an object can appear (the Create menu, dropping a model, adding the
/// component by hand, loading a scene saved before this rule existed). A
/// material that is already there, or one loaded from the scene file right
/// after, is kept: the loader replaces it.
///
/// A plain function, not a lambda that captures the Scene, so that it stays
/// valid when the Scene is moved.
void ensureMaterial(entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<MaterialComponent>(entity))
        registry.emplace<MaterialComponent>(entity);
}

} // namespace

Scene::Scene() {
    m_registry.on_construct<MeshRenderer>().connect<&ensureMaterial>();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void Scene::attach(entt::entity child, entt::entity parent) {
    auto& rel  = m_registry.get<Relationship>(child);
    rel.parent = parent;

    // Find the tail of the sibling chain and append.
    entt::entity head = (parent != entt::null)
        ? m_registry.get<Relationship>(parent).firstChild
        : m_firstRoot;

    if (head == entt::null) {
        // First child.
        if (parent != entt::null)
            m_registry.get<Relationship>(parent).firstChild = child;
        else
            m_firstRoot = child;
    } else {
        entt::entity last = head;
        while (m_registry.get<Relationship>(last).nextSibling != entt::null)
            last = m_registry.get<Relationship>(last).nextSibling;

        m_registry.get<Relationship>(last).nextSibling  = child;
        m_registry.get<Relationship>(child).prevSibling = last;
    }

    if (parent != entt::null)
        m_registry.get<Relationship>(parent).childCount++;

    // Mark world transform dirty so it is recomputed next frame.
    m_registry.get<WorldTransform>(child).dirty = true;
}

void Scene::detach(entt::entity e) {
    auto& rel         = m_registry.get<Relationship>(e);
    entt::entity prev = rel.prevSibling;
    entt::entity next = rel.nextSibling;

    if (prev != entt::null)
        m_registry.get<Relationship>(prev).nextSibling = next;
    else if (rel.parent != entt::null)
        m_registry.get<Relationship>(rel.parent).firstChild = next;
    else
        m_firstRoot = next;

    if (next != entt::null)
        m_registry.get<Relationship>(next).prevSibling = prev;

    if (rel.parent != entt::null)
        m_registry.get<Relationship>(rel.parent).childCount--;

    rel.parent      = entt::null;
    rel.prevSibling = entt::null;
    rel.nextSibling = entt::null;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

entt::entity Scene::create(const std::string& name, entt::entity parent) {
    return createWithUUID(generateUUID(), name, parent);
}

entt::entity Scene::createWithUUID(UUID uuid, const std::string& name,
                                   entt::entity parent) {
    entt::entity e = m_registry.create();

    m_registry.emplace<IDComponent>(e, uuid);
    m_registry.emplace<NameComponent>(e, name);
    m_registry.emplace<Relationship>(e);
    m_registry.emplace<Transform>(e);
    m_registry.emplace<WorldTransform>(e);

    m_byUUID[uuid] = e;
    attach(e, parent);

    return e;
}

void Scene::destroy(entt::entity e) {
    if (!m_registry.valid(e)) return;

    // Collect children first to avoid modifying the list while iterating.
    std::vector<entt::entity> children;
    for (entt::entity c = m_registry.get<Relationship>(e).firstChild;
         c != entt::null;
         c = m_registry.get<Relationship>(c).nextSibling)
        children.push_back(c);

    for (entt::entity c : children)
        destroy(c);

    detach(e);
    m_byUUID.erase(m_registry.get<IDComponent>(e).uuid);
    m_registry.destroy(e);
}

void Scene::setParent(entt::entity child, entt::entity newParent,
                      bool keepWorldTransform) {
    // Guard against cycles: walk up from newParent and bail if we hit child.
    for (entt::entity p = newParent; p != entt::null;
         p = m_registry.get<Relationship>(p).parent) {
        if (p == child) return;
    }

    // Snapshot world transform before re-parenting.
    if (keepWorldTransform) updateTransforms();
    const glm::mat4 worldSnap = m_registry.get<WorldTransform>(child).matrix;

    detach(child);
    attach(child, newParent);

    if (keepWorldTransform) {
        const glm::mat4 parentWorld = (newParent != entt::null)
            ? m_registry.get<WorldTransform>(newParent).matrix
            : glm::mat4(1.0f);

        decomposeInto(m_registry.get<Transform>(child),
                      glm::inverse(parentWorld) * worldSnap);
    }
}

Transform& Scene::transform(entt::entity e) {
    m_registry.get<WorldTransform>(e).dirty = true;
    return m_registry.get<Transform>(e);
}

void Scene::setWorldMatrix(entt::entity e, const glm::mat4& world) {
    const entt::entity parent = m_registry.get<Relationship>(e).parent;

    const glm::mat4 parentWorld = (parent != entt::null)
        ? m_registry.get<WorldTransform>(parent).matrix
        : glm::mat4(1.0f);

    // transform() flags the world matrix as stale, so it is rebuilt from
    // the new local values on the next updateTransforms().
    decomposeInto(transform(e), glm::inverse(parentWorld) * world);
}

void Scene::updateTransforms() {
    for (entt::entity e = m_firstRoot; e != entt::null;
         e = m_registry.get<Relationship>(e).nextSibling)
        updateRecursive(e, glm::mat4(1.0f), false);
}

void Scene::updateRecursive(entt::entity e, const glm::mat4& parentWorld,
                             bool parentDirty) {
    auto& wt         = m_registry.get<WorldTransform>(e);
    const bool dirty = parentDirty || wt.dirty;

    if (dirty) {
        wt.matrix = parentWorld * m_registry.get<Transform>(e).localMatrix();
        wt.dirty  = false;
    }

    for (entt::entity c = m_registry.get<Relationship>(e).firstChild;
         c != entt::null;
         c = m_registry.get<Relationship>(c).nextSibling)
        updateRecursive(c, wt.matrix, dirty);
}

entt::entity Scene::findByUUID(UUID uuid) const {
    auto it = m_byUUID.find(uuid);
    return it == m_byUUID.end() ? entt::null : it->second;
}

} // namespace fe
