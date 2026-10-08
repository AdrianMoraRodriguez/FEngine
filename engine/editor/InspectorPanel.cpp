#include "editor/InspectorPanel.h"
#include "scene/ComponentRegistry.h"
#include <imgui.h>

namespace fe {

void InspectorPanel::onImGui() {
    ImGui::Begin("Inspector");

    if (m_selected == entt::null ||
        !m_scene.registry().valid(m_selected)) {
        ImGui::TextDisabled("No entity selected.");
        ImGui::End();
        return;
    }

    auto& name = m_scene.registry().get<NameComponent>(m_selected);
    inspect(name);
    ImGui::SameLine();
    ImGui::TextDisabled("  UUID: %llu",
        m_scene.registry().get<IDComponent>(m_selected).uuid);

    ImGui::Separator();

    for (const auto& meta : ComponentRegistry::instance().all()) {
        if (meta.name == "Name") continue;
        if (!meta.has(m_scene.registry(), m_selected)) continue;

        ImGui::PushID(meta.name.c_str());

        bool open = ImGui::CollapsingHeader(meta.name.c_str(),
                        ImGuiTreeNodeFlags_DefaultOpen);

        if (meta.removable) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize("Remove").x - 8.0f);
            if (ImGui::SmallButton("Remove"))
                meta.remove(m_scene.registry(), m_selected);
        }

        if (open)
            meta.inspect(m_scene.registry(), m_selected);

        ImGui::PopID();
    }

    ImGui::Separator();

    if (ImGui::Button("Add Component", ImVec2(-1, 0)))
        ImGui::OpenPopup("##AddComponent");

    if (ImGui::BeginPopup("##AddComponent")) {
        for (const auto& meta : ComponentRegistry::instance().all()) {
            if (!meta.removable) continue;
            if (meta.has(m_scene.registry(), m_selected)) continue;
            if (ImGui::MenuItem(meta.name.c_str()))
                meta.add(m_scene.registry(), m_selected);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace fe
