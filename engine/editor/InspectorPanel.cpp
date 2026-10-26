#include "editor/InspectorPanel.h"

#include "editor/EditorTheme.h"
#include "editor/Icons.h"
#include "core/Log.h"
#include "core/PathUtil.h"
#include "import/AssetDatabase.h"
#include "scene/ComponentRegistry.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace fe {
namespace {

/// @return The icon for a component section, chosen from the component's
///         name so that new components get a neutral icon without any change.
const char* iconForComponent(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto has = [&](const char* word) { return lower.find(word) != std::string::npos; };
    if (has("transform")) return FE_ICON_MOVE_3D;
    if (has("material"))  return FE_ICON_PALETTE;
    if (has("mesh"))      return FE_ICON_CUBOID;
    if (has("camera"))    return FE_ICON_VIDEO;
    if (has("light"))     return FE_ICON_LIGHTBULB;
    return FE_ICON_SLIDERS_HORIZONTAL;
}

} // namespace

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

        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSettings& settings = m_scene.settings();

            // The environment slot: a wide button with the file name that an
            // image dragged from the Asset Browser can be dropped on. A
            // Radiance .hdr gives the best result; any image works.
            const bool  hasEnvironment = (settings.environment != 0);
            const float clearWidth     = ImGui::GetFrameHeight();
            const float slotWidth      = ImGui::GetContentRegionAvail().x -
                                         (hasEnvironment ? clearWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f);

            std::string path;
            if (hasEnvironment) {
                path = AssetDatabase::instance().pathOf(settings.environment);
                if (path.empty()) path = "(missing image)";
            }
            const std::string label = (hasEnvironment ? path : std::string("Default sky (drop an image)")) + "##environment";
            ImGui::Button(label.c_str(), ImVec2(slotWidth, 0.0f));
            if (hasEnvironment && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FE_IMAGE")) {
                    const std::filesystem::path file(std::u8string(
                        reinterpret_cast<const char8_t*>(payload->Data)));
                    const AssetHandle id = AssetDatabase::instance().idFor(file);
                    if (id == 0) {
                        FE_WARN("Environment: '%s' is not an image inside the asset folder",
                                toUtf8(file).c_str());
                    } else {
                        settings.environment = id;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (hasEnvironment) {
                ImGui::SameLine();
                if (ImGui::Button(FE_ICON_X "##clearEnvironment", ImVec2(clearWidth, 0.0f)))
                    settings.environment = 0;
            }

            ImGui::DragFloat("Sky Intensity", &settings.environmentIntensity,
                             0.01f, 0.0f, 20.0f, "%.2f");
            settings.environmentIntensity = std::max(settings.environmentIntensity, 0.0f);
            ImGui::Checkbox("Show Sky", &settings.showSky);
        }

        if (ImGui::CollapsingHeader("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSettings& settings = m_scene.settings();
            ImGui::Checkbox("Enabled##ao", &settings.ambientOcclusion);
            ImGui::BeginDisabled(!settings.ambientOcclusion);
            ImGui::DragFloat("Radius##ao", &settings.aoRadius, 0.01f, 0.05f, 5.0f, "%.2f");
            settings.aoRadius = std::clamp(settings.aoRadius, 0.05f, 5.0f);
            ImGui::DragFloat("Intensity##ao", &settings.aoIntensity, 0.01f, 0.0f, 4.0f, "%.2f");
            settings.aoIntensity = std::clamp(settings.aoIntensity, 0.0f, 4.0f);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Darkens the ambient and sky light where nearby objects block it.\nDirect lights are not affected.");
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

        // Text after "###" is the section's ID, so the icon in front of the
        // name never changes which header is which.
        const std::string header = std::string(iconForComponent(meta.name))
                                 + "  " + meta.name + "###" + meta.name;
        // Section headers use neutral greys: the accent colour is kept for
        // selection, so the headers do not compete with it.
        ImGui::PushStyleColor(ImGuiCol_Header,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
        ImGui::PushFont(theme::semiBold(), 0.0f);
        const bool open = ImGui::CollapsingHeader(header.c_str(),
                              ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);

        if (meta.removable) {
            const float buttonWidth = ImGui::CalcTextSize(FE_ICON_X).x
                                    + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth - 4.0f);
            if (ImGui::SmallButton(FE_ICON_X))
                meta.remove(m_scene.registry(), m_selected);
            ImGui::SetItemTooltip("Remove component");
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

    if (ImGui::Button(FE_ICON_PLUS "  Add Component", ImVec2(-1, 0)))
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
