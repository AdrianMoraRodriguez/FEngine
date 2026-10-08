#pragma once

#include "editor/Panel.h"
#include "scene/Scene.h"
#include <entt/entt.hpp>

namespace fe {

class InspectorPanel : public Panel {
public:
    InspectorPanel(Scene& scene, entt::entity& selectedEntity)
        : m_scene(scene), m_selected(selectedEntity) {}

    void onImGui() override;

private:
    Scene&        m_scene;
    entt::entity& m_selected;
};

} // namespace fe
