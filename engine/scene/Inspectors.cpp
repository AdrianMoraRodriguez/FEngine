#include "scene/Components.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace fe {

void inspect(NameComponent& c) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", c.name.c_str());
    if (ImGui::InputText("##name", buf, sizeof(buf)))
        c.name = buf;
}

void inspect(Transform& t) {
    ImGui::DragFloat3("Position", glm::value_ptr(t.position), 0.1f);

    // Convert to Euler for display — users think in degrees, not quaternions.
    glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(euler), 1.0f))
        t.rotation = glm::quat(glm::radians(euler));

    ImGui::DragFloat3("Scale", glm::value_ptr(t.scale), 0.01f, 0.001f, 100.0f);
}

void inspect(MeshRenderer& m) {
    ImGui::Text("Mesh     : %llu", m.mesh);
    ImGui::Text("Material : %llu", m.material);
    ImGui::Checkbox("Cast Shadows", &m.castShadows);
}

void inspect(CameraComponent& c) {
    ImGui::DragFloat("FOV",   &c.fovDegrees, 0.5f, 10.0f, 170.0f);
    ImGui::DragFloat("Near",  &c.nearPlane,  0.001f, 0.001f, 10.0f);
    ImGui::DragFloat("Far",   &c.farPlane,   1.0f,  1.0f, 10000.0f);
    ImGui::Checkbox("Primary", &c.primary);
}

void inspect(LightComponent& l) {
    const char* types[] = { "Directional", "Point", "Spot" };
    int type = static_cast<int>(l.type);
    if (ImGui::Combo("Type", &type, types, 3))
        l.type = static_cast<LightComponent::Type>(type);

    ImGui::ColorEdit3("Colour",    glm::value_ptr(l.color));
    ImGui::DragFloat("Intensity",  &l.intensity, 0.05f, 0.0f, 100.0f);

    if (l.type != LightComponent::Type::Directional)
        ImGui::DragFloat("Range", &l.range, 0.1f, 0.0f, 1000.0f);
}

} // namespace fe
