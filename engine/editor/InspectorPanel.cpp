#include "editor/InspectorPanel.h"

#include "scene/ComponentRegistry.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

namespace fe {

void InspectorPanel::onImGui() {
    ImGui::Begin("Inspector");

    if (m_selected == entt::null ||
        !m_scene.registry().valid(m_selected)) {
        // With nothing selected, the Inspector shows the scene's own settings.
        ImGui::TextDisabled("No entity selected.");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Scene Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSettings& settings = m_scene.settings();
            ImGui::ColorEdit3("Ambient Colour", glm::value_ptr(settings.ambientColor));
            ImGui::DragFloat("Ambient Intensity", &settings.ambientIntensity,
                             0.005f, 0.0f, 5.0f, "%.3f");
            settings.ambientIntensity = std::max(settings.ambientIntensity, 0.0f);
        }

        ImGui::End();
        return;
    }

    // Editable name + UUID. A rename cannot affect any derived data.
    auto& name = m_scene.registry().get<NameComponent>(m_selected);
    inspect(name);
    ImGui::SameLine();
    ImGui::TextDisabled("  UUID: %llu",
        m_scene.registry().get<IDComponent>(m_selected).uuid);

    ImGui::Separator();

    // One collapsing section per registered component.
    bool changed = false;
    for (const auto& meta : ComponentRegistry::instance().all()) {
        if (meta.name == "Name") continue;
        if (!meta.has(m_scene.registry(), m_selected)) continue;

        ImGui::PushID(meta.name.c_str());

        const bool open = ImGui::CollapsingHeader(meta.name.c_str(),
                              ImGuiTreeNodeFlags_DefaultOpen);

        if (meta.removable) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize("Remove").x - 8.0f);
            if (ImGui::SmallButton("Remove"))
                meta.remove(m_scene.registry(), m_selected);
        }

        if (open)
            changed |= meta.inspect(m_scene.registry(), m_selected);

        ImGui::PopID();
    }

    // The inspector edits components straight through the registry, which
    // does not flag the cached world matrix as stale. Do it only when a
    // widget reported an actual edit; Scene::updateTransforms() then
    // recomputes this entity and its children, and nothing else.
    if (changed)
        m_scene.registry().get<WorldTransform>(m_selected).dirty = true;

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
