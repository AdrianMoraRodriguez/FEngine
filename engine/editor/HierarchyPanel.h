#pragma once

#include "editor/Panel.h"
#include "scene/Scene.h"
#include <entt/entt.hpp>

namespace fe {

/**
 * @brief Scene tree panel.
 *
 * - Click a node to select it; click empty space to deselect.
 * - Right-click empty space to create entities.
 * - Right-click a node, or press Delete while the panel is focused, to
 *   delete the selected entity and its children.
 */
class HierarchyPanel : public Panel {
public:
    /**
     * @param scene          The active scene to display and edit.
     * @param selectedEntity Reference to the editor's current selection.
     */
    HierarchyPanel(Scene& scene, entt::entity& selectedEntity)
        : m_scene(scene), m_selected(selectedEntity) {}

    void onImGui() override;

private:
    void drawNode(entt::entity e);

    /// Creates a root entity, optionally with a built-in mesh, and selects it.
    void createEntity(const char* name, AssetHandle mesh);

    Scene&        m_scene;
    entt::entity& m_selected;

    /// Deletions are deferred until the tree has been fully drawn, because
    /// destroying an entity while iterating the tree would invalidate it.
    entt::entity m_pendingDelete = entt::null;
};

} // namespace fe
