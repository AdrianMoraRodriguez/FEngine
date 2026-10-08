#pragma once

#include "editor/Panel.h"
#include "scene/Scene.h"
#include <entt/entt.hpp>

namespace fe {

class HierarchyPanel : public Panel {
public:
    /**
     * @brief Constructs the panel with its required dependencies.
     * @param scene          The active scene to display.
     * @param selectedEntity Reference to the editor's current selection.
     */
    HierarchyPanel(Scene& scene, entt::entity& selectedEntity)
        : m_scene(scene), m_selected(selectedEntity) {}

    void onImGui() override;

private:
    void drawNode(entt::entity e);

    Scene&        m_scene;
    entt::entity& m_selected;
};

} // namespace fe
