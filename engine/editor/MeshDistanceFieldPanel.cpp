#include "editor/MeshDistanceFieldPanel.h"

#include "gfx/GlobalDistanceField.h"
#include "gfx/MeshDistanceField.h"
#include "gfx/SdfAtlas.h"
#include "gfx/SceneRenderer.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace fe {
namespace {

/// Colour of a cell at signed distance @p d, where @p range is the distance
/// that maps to the strongest colour.
ImU32 colourFor(float d, float voxel, float range) {
    // The surface: within half a cell of zero.
    if (std::abs(d) < 0.5f * voxel) return IM_COL32(255, 255, 255, 255);

    const float t = std::clamp(std::abs(d) / range, 0.0f, 1.0f);

    // Thin dark lines every few cells, like the contour lines of a map.
    const float step  = 4.0f * voxel;
    const float phase = std::abs(d) / step - std::floor(std::abs(d) / step);
    const float line  = (phase < 0.08f) ? 0.55f : 1.0f;

    float r, g, b;
    if (d >= 0.0f) {   // outside: dark blue to light blue
        r = 0.08f + 0.35f * t; g = 0.18f + 0.5f * t; b = 0.35f + 0.6f * t;
    } else {           // inside: dark orange to bright orange
        r = 0.35f + 0.65f * t; g = 0.15f + 0.4f * t; b = 0.05f + 0.1f * t;
    }
    const auto byte = [line](float v) { return static_cast<int>(std::clamp(v * line, 0.0f, 1.0f) * 255.0f); };
    return IM_COL32(byte(r), byte(g), byte(b), 255);
}

} // namespace

void MeshDistanceFieldPanel::onImGui() {
    if (!visible) return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mesh Distance Field", &visible)) {
        ImGui::End();
        return;
    }

    // ---- The scene's field: what it holds and what it costs ----
    if (ImGui::CollapsingHeader("Scene field", ImGuiTreeNodeFlags_DefaultOpen)) {
        const GlobalDistanceField& field = m_renderer.globalDistanceField();
        if (!field.ready()) {
            ImGui::TextDisabled("Not built yet (no shadows or reflections use it).");
        } else {
            const uint32_t n = field.resolution();
            ImGui::Text("Objects: %u of %u", field.instanceCount(), GlobalDistanceField::kMaxInstances);
            ImGui::Text("Resolution: %u cells per side, %u levels", n, GlobalDistanceField::kLevels);
            ImGui::Text("Memory: %.0f MB", static_cast<double>(field.memoryBytes()) / (1024.0 * 1024.0));
            for (uint32_t l = 0; l < GlobalDistanceField::kLevels; ++l)
                ImGui::Text("Level %u: %.0f m across, cells of %.1f cm", l, field.extent(l), field.cellSize(l) * 100.0f);

            const SdfAtlas& atlas = m_renderer.meshes().distanceAtlas();
            ImGui::Text("Mesh fields: %u of %u layers used (%.0f%%)", atlas.used(), SdfAtlas::kDepth,
                        100.0 * atlas.used() / SdfAtlas::kDepth);
            ImGui::ProgressBar(static_cast<float>(atlas.used()) / static_cast<float>(SdfAtlas::kDepth),
                               ImVec2(-1.0f, 0.0f));
        }
    }

    // ---- Which mesh ----
    entt::registry& registry = m_scene.registry();
    const MeshRenderer* meshRenderer =
        registry.valid(m_selected) ? registry.try_get<MeshRenderer>(m_selected) : nullptr;

    if (!meshRenderer) {
        ImGui::TextDisabled("Select an entity with a mesh.");
        ImGui::End();
        return;
    }

    MeshLibrary& meshes = m_renderer.meshes();
    const SdfVolume* volume = meshes.distanceVolume(meshRenderer->mesh);

    if (!volume || !volume->valid()) {
        if (meshes.distanceFieldsPending() > 0) ImGui::TextDisabled("Building the distance field...");
        else                                    ImGui::TextDisabled("This mesh has no distance field.");
        ImGui::End();
        return;
    }

    ImGui::Text("%u x %u x %u cells, %.4f per cell (mesh units)",
                volume->dims.x, volume->dims.y, volume->dims.z, volume->voxelSize);
    ImGui::Text("%.2f MB on the CPU, %.2f MB on the GPU",
                static_cast<double>(volume->distances.size() * sizeof(float)) / (1024.0 * 1024.0),
                static_cast<double>(volume->distances.size() * 2) / (1024.0 * 1024.0));

    // ---- Which slice ----
    const char* axes[] = {"X (looking along x)", "Y (from above)", "Z (looking along z)"};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Plane", &m_axis, axes, 3);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Position", &m_slice, 0.0f, 1.0f, "%.2f");

    // The two axes the picture is made of: u to the right, v downwards (flipped
    // for the vertical axis, so that up is up).
    const int axisU = (m_axis == 0) ? 2 : 0;
    const int axisV = (m_axis == 1) ? 2 : 1;
    const bool flipV = (axisV == 1);

    const uint32_t sizeU = volume->dims[axisU];
    const uint32_t sizeV = volume->dims[axisV];
    const uint32_t layer = std::min(static_cast<uint32_t>(m_slice * static_cast<float>(volume->dims[m_axis])),
                                    volume->dims[m_axis] - 1);

    // ---- The picture ----
    const ImVec2  avail = ImGui::GetContentRegionAvail();
    const float   cell  = std::max(1.0f, std::floor(std::min(avail.x / static_cast<float>(sizeU),
                                                             (avail.y - 24.0f) / static_cast<float>(sizeV))));
    const ImVec2  origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(cell * static_cast<float>(sizeU), cell * static_cast<float>(sizeV)));

    const glm::vec3 extent = volume->boxMax - volume->boxMin;
    const float range = 0.25f * std::max({extent.x, extent.y, extent.z});

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();
    int hoverU = -1, hoverV = -1;

    for (uint32_t v = 0; v < sizeV; ++v) {
        for (uint32_t u = 0; u < sizeU; ++u) {
            uint32_t c[3];
            c[m_axis] = layer;
            c[axisU]  = u;
            c[axisV]  = v;

            const float d = volume->distances[volume->index(c[0], c[1], c[2])];

            const uint32_t row = flipV ? (sizeV - 1 - v) : v;
            const ImVec2 a(origin.x + cell * static_cast<float>(u), origin.y + cell * static_cast<float>(row));
            const ImVec2 b(a.x + cell, a.y + cell);
            draw->AddRectFilled(a, b, colourFor(d, volume->voxelSize, range));

            if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
                hoverU = static_cast<int>(u);
                hoverV = static_cast<int>(v);
            }
        }
    }

    if (ImGui::IsItemHovered() && hoverU >= 0) {
        uint32_t c[3];
        c[m_axis] = layer;
        c[axisU]  = static_cast<uint32_t>(hoverU);
        c[axisV]  = static_cast<uint32_t>(hoverV);

        const float d = volume->distances[volume->index(c[0], c[1], c[2])];
        const glm::vec3 p = volume->boxMin + (glm::vec3(c[0], c[1], c[2]) + 0.5f) * volume->voxelSize;
        ImGui::SetTooltip("cell (%u, %u, %u)\npoint (%.3f, %.3f, %.3f)\ndistance %.4f %s",
                          c[0], c[1], c[2], p.x, p.y, p.z, d, d < 0.0f ? "(inside)" : "(outside)");
    }

    ImGui::TextDisabled("Blue: outside. Orange: inside. White: the surface.");
    ImGui::End();
}

} // namespace fe
