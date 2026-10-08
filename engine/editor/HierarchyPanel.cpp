#include "editor/HierarchyPanel.h"
#include <imgui.h>

namespace fe {

void HierarchyPanel::onImGui() {
    ImGui::Begin("Hierarchy");
    ImGui::Text("Scene: %s", m_scene.name().c_str());
    ImGui::Separator();

    for (entt::entity e = m_scene.firstRoot(); e != entt::null;
         e = m_scene.registry().get<Relationship>(e).nextSibling)
        drawNode(e);

    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        m_selected = entt::null;

    ImGui::End();
}

void HierarchyPanel::drawNode(entt::entity e) {
    const auto& name = m_scene.registry().get<NameComponent>(e).name;
    const auto& rel  = m_scene.registry().get<Relationship>(e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick;

    if (rel.firstChild == entt::null) flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == m_selected)              flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(
            static_cast<intptr_t>(static_cast<std::uint32_t>(e))),
        flags, "%s", name.c_str());

    if (ImGui::IsItemClicked()) m_selected = e;

    if (open) {
        for (entt::entity c = rel.firstChild; c != entt::null;
             c = m_scene.registry().get<Relationship>(c).nextSibling)
            drawNode(c);
        ImGui::TreePop();
    }
}

} // namespace fe
