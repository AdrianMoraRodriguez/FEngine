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

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

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

    return changed;
}

} // namespace fe
