#include "editor/ProjectSettingsPanel.h"

#include <imgui.h>

#include <algorithm>

namespace fe {

void ProjectSettingsPanel::onImGui() {
    if (!visible) return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &visible)) {
        ImGui::End();
        return;
    }

    ProjectSettings& s = m_settings;

    ImGui::TextDisabled("Quality settings shared by every scene. Saved in %s",
                        m_file.filename().string().c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Distance Fields", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Used by distance field shadows and reflections.");

        static const char* const kDetail[] = {"Low", "Medium", "High"};
        s.distanceFieldDetail = std::clamp(s.distanceFieldDetail, 0, 2);
        m_dirty |= ImGui::Combo("Detail", &s.distanceFieldDetail, kDetail, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How finely the scene's field follows what is near the camera.\n"
                              "Cells of the finest level at 128 cells: Low 25 cm, Medium 12 cm, High 6 cm.\n"
                              "Higher detail sharpens edges, but the fine levels cover less space around\n"
                              "the camera (8 m across at High) and are rebuilt more often when it moves.");

        static const char* const kResolution[] = {"128 cells (38 MB)", "192 cells (127 MB)", "256 cells (302 MB)"};
        s.distanceFieldResolution = std::clamp(s.distanceFieldResolution, 0, 2);
        m_dirty |= ImGui::Combo("Scene field resolution", &s.distanceFieldResolution, kResolution, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cells along each side of every level of the scene's field. More cells make\n"
                              "the cells smaller everywhere without covering less space, for more video\n"
                              "memory and a slower build. Changing it pauses the GPU for a moment.");

        static const char* const kMesh[] = {"32 cells", "64 cells", "96 cells", "128 cells"};
        s.meshFieldResolution = std::clamp(s.meshFieldResolution, 0, ProjectSettings::kMeshFieldChoices - 1);
        m_dirty |= ImGui::Combo("Mesh field resolution", &s.meshFieldResolution, kMesh,
                                ProjectSettings::kMeshFieldChoices);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cells along the longest side of each mesh's own field. The reflections use\n"
                              "these to put a hit exactly on the surface, so more cells give sharper\n"
                              "corners on large models. The atlas holding them grows with this value\n"
                              "(16 MB at 64, 67 MB at 128), and fewer different meshes fit in it:\n"
                              "about 2048 / cells of them at full size.");
        if (s.meshFieldResolution != m_atStart.meshFieldResolution)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Restart the editor to apply the mesh field resolution.");
    }

    if (ImGui::CollapsingHeader("Distance Field Reflections", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* const kQuality[] = {"Fast (32 steps)", "Normal (64 steps)", "High (128 steps)"};
        s.distanceFieldReflectionQuality = std::clamp(s.distanceFieldReflectionQuality, 0, 2);
        m_dirty |= ImGui::Combo("Quality", &s.distanceFieldReflectionQuality, kQuality, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Steps a reflected ray may take. More steps reach farther and find thin\n"
                              "objects, at a higher cost per shiny pixel.");

        static const char* const kReach[] = {"Near (sharp)", "Far (blurrier)"};
        s.distanceFieldReflectionReach = std::clamp(s.distanceFieldReflectionReach, 0, 1);
        m_dirty |= ImGui::Combo("Reach", &s.distanceFieldReflectionReach, kReach, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Near: only the finest level of the field is used, so reflections are sharp\n"
                              "but only of what is close to the camera.\n"
                              "Far: the next level is used too, reaching four times farther with softer edges.");

        m_dirty |= ImGui::Checkbox("Shadows in reflections", &s.distanceFieldReflectedShadows);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What a reflection shows is shadowed too (one more ray per reflected pixel).\n"
                              "Off: it is lit by the sun everywhere, which is cheaper but flatter.");
    }

    if (ImGui::CollapsingHeader("Distance Field Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        m_dirty |= ImGui::DragFloat("Bias", &s.distanceFieldShadowBias, 0.01f, 0.25f, 4.0f, "%.2f cells");
        s.distanceFieldShadowBias = std::clamp(s.distanceFieldShadowBias, 0.25f, 4.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far above a surface a shadow ray starts, in cells of the field.\n"
                              "Too low: surfaces shadow themselves (speckle, stripes).\n"
                              "Too high: shadows detach from the objects that cast them.");
    }

    // Written once nothing is being dragged.
    if (m_dirty && !ImGui::IsAnyItemActive()) {
        m_settings.save(m_file);
        m_dirty = false;
    }

    ImGui::End();
}

} // namespace fe
