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

    if (m.mesh > builtin::MeshPlane) {
        // Shown by its current path; the scene stores the id, so the file may
        // be moved or renamed freely.
        const std::string path = AssetDatabase::instance().pathOf(m.mesh);
        if (path.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "Missing model (id %llx)",
                               static_cast<unsigned long long>(m.mesh));
        else
            ImGui::TextDisabled("%s", path.c_str());
    }

    ImGui::Text("Material : %llu", m.material);
    changed |= ImGui::Checkbox("Cast Shadows", &m.castShadows);
    return changed;
}

namespace {

/// One texture slot of the material: a wide button with the image's path that
/// an image dragged from the Asset Browser can be dropped on, and a button to
/// empty the slot. The drop target belongs to the last item, so it comes right
/// after the button.
///
/// @param label  Name shown at the left.
/// @param id     Unique ImGui id for the slot's widgets.
/// @param handle The slot's image id; 0 means none.
/// @param empty  Text of an empty slot.
/// @return `true` if the slot changed.
bool textureSlot(const char* label, const char* id, AssetHandle& handle, const char* empty) {
    bool changed = false;
    ImGui::PushID(id);

    ImGui::TextUnformatted(label);
    ImGui::SameLine(96.0f);

    const bool  hasTexture = (handle != 0);
    const float clearWidth = ImGui::GetFrameHeight();
    const float slotWidth  = ImGui::GetContentRegionAvail().x -
                             (hasTexture ? clearWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f);

    // The slot holds an id; what the user sees is where that image is now.
    std::string path;
    if (hasTexture) {
        path = AssetDatabase::instance().pathOf(handle);
        if (path.empty()) path = "(missing image)";
    }

    const std::string text = (hasTexture ? path : std::string(empty)) + "##slot";
    ImGui::Button(text.c_str(), ImVec2(slotWidth, 0.0f));
    if (hasTexture && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", path.c_str());

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FE_IMAGE")) {
            // The Asset Browser sends the path as UTF-8 text.
            const std::filesystem::path file(std::u8string(
                reinterpret_cast<const char8_t*>(payload->Data)));
            const AssetHandle dropped = AssetDatabase::instance().idFor(file);
            if (dropped == 0) {
                FE_WARN("Material: '%s' is not an image inside the asset folder",
                        toUtf8(file).c_str());
            } else {
                handle  = dropped;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (hasTexture) {
        ImGui::SameLine();
        if (ImGui::Button(FE_ICON_X "##clear", ImVec2(clearWidth, 0.0f))) {
            handle  = 0;
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace

bool inspect(MaterialComponent& m) {
    bool changed = false;

    changed |= ImGui::ColorEdit3("Base Color", glm::value_ptr(m.baseColor));

    // A slot left empty keeps the model's own texture for it.
    ImGui::SeparatorText("Textures");
    changed |= textureSlot("Albedo",      "albedo",     m.albedoTexture,     "Model's own (drop an image)");
    changed |= textureSlot("Metal/Rough", "metalrough", m.metalRoughTexture, "Model's own (green: roughness, blue: metal)");
    changed |= textureSlot("Normal",      "normal",     m.normalTexture,     "Model's own (drop a normal map)");
    changed |= textureSlot("Emissive",    "emissive",   m.emissiveTexture,   "Model's own (drop an image)");

    ImGui::SeparatorText("Surface");
    changed |= ImGui::Checkbox("Custom Surface", &m.customSurface);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the model's own metalness, roughness and emissive values.\n"
                          "On: the values below replace them on every part of the object.");

    if (m.customSurface) {
        changed |= ImGui::SliderFloat("Metallic",  &m.metallic,  0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::DragFloat("Normal Strength", &m.normalStrength, 0.01f, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::ColorEdit3("Emissive Colour", glm::value_ptr(m.emissiveColor));
        changed |= ImGui::DragFloat("Emissive Intensity", &m.emissiveIntensity, 0.05f, 0.0f, 100.0f, "%.2f");
        m.normalStrength    = std::max(m.normalStrength, 0.0f);
        m.emissiveIntensity = std::max(m.emissiveIntensity, 0.0f);
    } else {
        ImGui::TextDisabled("Using the model's own values.");
    }

    ImGui::SeparatorText("Transparency");
    const char* modes[] = {"Model's own", "Opaque", "Cutout", "Blend"};
    int mode = static_cast<int>(m.transparency);
    if (ImGui::Combo("Mode", &mode, modes, 4)) {
        m.transparency = static_cast<MaterialComponent::Transparency>(mode);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Model's own: what the model says (solid for the built-in shapes).\n"
                          "Opaque: ignores the albedo's alpha.\n"
                          "Cutout: pixels with little alpha are not drawn (leaves, fences).\n"
                          "Blend: see-through, mixed with what is behind (glass, water).\n"
                          "Translucent objects do not cast shadows or ambient occlusion.");
    if (m.transparency == MaterialComponent::Transparency::Cutout) {
        changed |= ImGui::SliderFloat("Alpha Cutoff", &m.alphaCutoff, 0.0f, 1.0f, "%.2f");
    }
    if (m.transparency != MaterialComponent::Transparency::Opaque) {
        changed |= ImGui::SliderFloat("Opacity", &m.opacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Multiplies the albedo's alpha. It only shows on Blend (and Cutout, where it lowers the alpha the cutoff is compared with).");
    }
    changed |= ImGui::Checkbox("Double Sided", &m.doubleSided);
    m.alphaCutoff = std::clamp(m.alphaCutoff, 0.0f, 1.0f);
    m.opacity     = std::clamp(m.opacity, 0.0f, 1.0f);

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

bool inspect(ReflectionProbeComponent& p) {
    bool changed = false;

    changed |= ImGui::DragFloat("Intensity", &p.intensity, 0.01f, 0.0f, 10.0f, "%.2f");
    p.intensity = std::max(p.intensity, 0.0f);

    changed |= ImGui::DragFloat3("Box Size", glm::value_ptr(p.boxSize), 0.1f, 0.5f, 1000.0f, "%.1f");
    p.boxSize = glm::max(p.boxSize, glm::vec3(0.5f));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the probe applies, centred on the entity.\nIt also stands for the room's walls when correcting the reflections.");

    changed |= ImGui::DragFloat("Blend Distance", &p.blendDistance, 0.05f, 0.0f, 50.0f, "%.2f");
    p.blendDistance = std::max(p.blendDistance, 0.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Soft border inside the box, where the probe fades into\nthe sky or into a neighbouring probe.");

    changed |= ImGui::Checkbox("Box Projection", &p.boxProjection);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Corrects the reflection for the box, so it lines up with the room.\nTurn off for open spaces.");

    if (ImGui::Button("Bake")) {
        ++p.bakeSerial;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Photographs the scene again from here. It also happens by itself\na moment after the scene changes.");

    return changed;
}

} // namespace fe
