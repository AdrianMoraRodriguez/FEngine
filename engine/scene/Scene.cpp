#include "scene/Scene.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <vector>

namespace fe {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void Scene::attach(entt::entity child, entt::entity parent) {
    auto& rel   = m_registry.get<Relationship>(child);
    rel.parent  = parent;

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
    glm::mat4 worldSnap = m_registry.get<WorldTransform>(child).matrix;

    detach(child);
    attach(child, newParent);

    if (keepWorldTransform) {
        glm::mat4 parentWorld = (newParent != entt::null)
            ? m_registry.get<WorldTransform>(newParent).matrix
            : glm::mat4(1.0f);

        glm::mat4 localM = glm::inverse(parentWorld) * worldSnap;

        auto& t = m_registry.get<Transform>(child);
        t.position = glm::vec3(localM[3]);

        glm::vec3 sx{localM[0]}, sy{localM[1]}, sz{localM[2]};
        t.scale = { glm::length(sx), glm::length(sy), glm::length(sz) };

        glm::mat3 rot{ sx / t.scale.x, sy / t.scale.y, sz / t.scale.z };
        t.rotation = glm::quat_cast(rot);
    }
}

Transform& Scene::transform(entt::entity e) {
    m_registry.get<WorldTransform>(e).dirty = true;
    return m_registry.get<Transform>(e);
}

void Scene::updateTransforms() {
    for (entt::entity e = m_firstRoot; e != entt::null;
         e = m_registry.get<Relationship>(e).nextSibling)
        updateRecursive(e, glm::mat4(1.0f), false);
}

void Scene::updateRecursive(entt::entity e, const glm::mat4& parentWorld,
                             bool parentDirty) {
    auto& wt        = m_registry.get<WorldTransform>(e);
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
