/**
 * @file Inspectors.cpp
 * @brief ImGui widgets that edit each built-in component.
 *
 * Every inspect() overload draws the widgets for one component and returns
 * `true` if the user changed a value during this frame. The inspector panel
 * uses that result to know when cached data derived from the components
 * (such as the world matrix) must be recomputed.
 */

#include "scene/Components.h"

#include "core/AssetPaths.h"
#include "core/Log.h"
#include "core/PathUtil.h"
#include "editor/Icons.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cstdio>

namespace fe {

bool inspect(NameComponent& c) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", c.name.c_str());

    if (ImGui::InputText("##name", buf, sizeof(buf))) {
        c.name = buf;
        return true;
    }
    return false;
}

bool inspect(Transform& t) {
    bool changed = false;

    changed |= ImGui::DragFloat3("Position", glm::value_ptr(t.position), 0.1f);

    // Rotation is stored as a quaternion but edited as Euler angles in
    // degrees, which is how people think about it. It is only written back
    // when the user actually drags, so the conversion cannot drift the value.
    glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(euler), 1.0f)) {
        t.rotation = glm::quat(glm::radians(euler));
        changed = true;
    }

    changed |= ImGui::DragFloat3("Scale", glm::value_ptr(t.scale),
                                 0.01f, 0.001f, 100.0f);
    return changed;
}

bool inspect(MeshRenderer& m) {
    static_assert(builtin::MeshCube == 1 && builtin::MeshPlane == 2,
                  "The list below maps indices directly to handles");

    bool changed = false;

    const char* names[] = { "None", "Cube", "Plane" };

    // Built-in meshes have small handles; anything larger is an imported model.
    const char* preview = (m.mesh <= builtin::MeshPlane) ? names[m.mesh]
                                                         : "Imported model";

    if (ImGui::BeginCombo("Mesh", preview)) {
        for (AssetHandle i = 0; i < 3; ++i) {
            const bool selected = (m.mesh == i);
            if (ImGui::Selectable(names[i], selected)) {
                m.mesh  = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (m.mesh > builtin::MeshPlane)
        ImGui::TextDisabled("Asset id: %llx", static_cast<unsigned long long>(m.mesh));

    ImGui::Text("Material : %llu", m.material);
    changed |= ImGui::Checkbox("Cast Shadows", &m.castShadows);
    return changed;
}

bool inspect(MaterialComponent& m) {
    bool changed = false;

    changed |= ImGui::ColorEdit3("Base Color", glm::value_ptr(m.baseColor));

    // ---- Albedo texture slot ----
    // A wide button that shows the file name. Images dragged from the Asset
    // Browser can be dropped on it. The drop target belongs to the last item,
    // so it comes right after the button.
    ImGui::TextUnformatted("Albedo");
    ImGui::SameLine();

    const bool  hasTexture = !m.albedoTexture.empty();
    const float clearWidth = ImGui::GetFrameHeight();
    const float slotWidth  = ImGui::GetContentRegionAvail().x - (hasTexture ? clearWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f);

    std::string label = hasTexture ? m.albedoTexture : std::string("None (drop an image)");
    label += "##albedo";
    ImGui::Button(label.c_str(), ImVec2(slotWidth, 0.0f));

    if (hasTexture && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", m.albedoTexture.c_str());

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FE_IMAGE")) {
            // The Asset Browser sends the path as UTF-8 text.
            const std::filesystem::path file(std::u8string(
                reinterpret_cast<const char8_t*>(payload->Data)));
            const std::string relative = assets::toRelative(file);
            if (relative.empty()) {
                FE_WARN("Material: '%s' is not inside the asset folder", toUtf8(file).c_str());
            } else {
                m.albedoTexture = relative;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (hasTexture) {
        ImGui::SameLine();
        if (ImGui::Button(FE_ICON_X "##clearAlbedo", ImVec2(clearWidth, 0.0f))) {
            m.albedoTexture.clear();
            changed = true;
        }
    }

    return changed;
}

bool inspect(CameraComponent& c) {
    bool changed = false;

    changed |= ImGui::DragFloat("FOV",   &c.fovDegrees, 0.5f,   10.0f,  170.0f);
    changed |= ImGui::DragFloat("Near",  &c.nearPlane,  0.001f, 0.001f, 10.0f);
    changed |= ImGui::DragFloat("Far",   &c.farPlane,   1.0f,   1.0f,   10000.0f);
    changed |= ImGui::Checkbox("Primary", &c.primary);
    return changed;
}

bool inspect(LightComponent& l) {
    bool changed = false;

    const char* types[] = { "Directional", "Point", "Spot" };
    int type = static_cast<int>(l.type);
    if (ImGui::Combo("Type", &type, types, 3)) {
        l.type  = static_cast<LightComponent::Type>(type);
        changed = true;
    }

    changed |= ImGui::ColorEdit3("Colour",   glm::value_ptr(l.color));
    changed |= ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0.0f, 100.0f);

    if (l.type != LightComponent::Type::Directional)
        changed |= ImGui::DragFloat("Range", &l.range, 0.1f, 0.0f, 1000.0f);

    changed |= ImGui::Checkbox("Cast Shadows", &l.castShadows);
    if (l.castShadows) {
        // The same number means two things: the light's radius in world units
        // for point and spot lights, its angular radius for the sun.
        if (l.type == LightComponent::Type::Directional)
            changed |= ImGui::DragFloat("Light Angle", &l.shadowSoftness, 0.05f, 0.0f, 10.0f, "%.2f deg");
        else
            changed |= ImGui::DragFloat("Light Size", &l.shadowSoftness, 0.005f, 0.0f, 3.0f, "%.3f");
        l.shadowSoftness = std::max(l.shadowSoftness, 0.0f);
    }

    if (l.type == LightComponent::Type::Spot) {
        // Half-angles of the cone, in degrees. The light is full strength
        // inside the inner cone and fades to nothing at the outer one, so
        // the inner angle can never be wider than the outer: whichever of
        // the two is being dragged pushes the other.
        const bool outerChanged = ImGui::DragFloat("Outer angle", &l.outerConeDegrees,
                                                   0.5f, 1.0f, 89.0f, "%.1f deg");
        const bool innerChanged = ImGui::DragFloat("Inner angle", &l.innerConeDegrees,
                                                   0.5f, 0.0f, 89.0f, "%.1f deg");
        if (outerChanged) l.innerConeDegrees = std::min(l.innerConeDegrees, l.outerConeDegrees);
        if (innerChanged) l.outerConeDegrees = std::max(l.outerConeDegrees, l.innerConeDegrees);
        changed |= outerChanged || innerChanged;
    }

    return changed;
}

} // namespace fe
