#include "editor/HierarchyPanel.h"

#include "editor/EditorTheme.h"
#include "editor/Icons.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace fe {
namespace {

/// Icon glyph and colour that represent one entity in the tree.
struct NodeIcon {
    const char* glyph;   ///< UTF-8 icon from Icons.h.
    ImVec4      tint;    ///< Colour the glyph is drawn with.
};

/// @return The icon that tells what kind of object @p e is, decided by the
///         components it has: lights first, then cameras, then meshes.
NodeIcon iconFor(const entt::registry& registry, entt::entity e) {
    if (const auto* light = registry.try_get<LightComponent>(e)) {
        switch (light->type) {
            case LightComponent::Type::Directional: return {FE_ICON_SUN,       theme::tintLight()};
            case LightComponent::Type::Point:       return {FE_ICON_LIGHTBULB, theme::tintLight()};
            case LightComponent::Type::Spot:        return {FE_ICON_SPOTLIGHT, theme::tintLight()};
        }
    }
    if (registry.all_of<CameraComponent>(e))
        return {FE_ICON_VIDEO, theme::tintCamera()};
    if (registry.all_of<ReflectionProbeComponent>(e))
        return {FE_ICON_GLOBE, theme::tintCamera()};
    if (registry.all_of<MeshRenderer>(e))
        return {FE_ICON_CUBOID, theme::tintModel()};

    return {FE_ICON_CIRCLE_DASHED, theme::tintFile()};   // empty / grouping node
}

} // namespace

void HierarchyPanel::createEntity(const char* name, AssetHandle mesh) {
    entt::entity e = m_scene.create(name);

    if (mesh != builtin::MeshNone) {
        MeshRenderer renderer;
        renderer.mesh = mesh;
        m_scene.registry().emplace<MeshRenderer>(e, renderer);
    }

    m_selected = e;
}

void HierarchyPanel::createLight(LightComponent::Type type) {
    LightComponent light;
    light.type = type;

    const char*      name = "Light";
    glm::vec3        position{0.0f, 4.0f, 0.0f};
    glm::vec3        travel {0.0f, -1.0f, 0.0f};   // direction the light shines along

    switch (type) {
        case LightComponent::Type::Directional:
            name = "Directional Light";
            light.intensity      = 0.75f;
            light.shadowSoftness = 1.5f;    // angular radius, in degrees
            travel = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f));
            break;

        case LightComponent::Type::Point:
            name = "Point Light";
            light.intensity = 8.0f;
            light.range     = 8.0f;
            position        = {0.0f, 2.0f, 0.0f};
            break;

        case LightComponent::Type::Spot:
            name = "Spot Light";
            light.intensity = 15.0f;
            light.range     = 8.0f;
            break;
    }

    entt::entity e = m_scene.create(name);
    m_scene.registry().emplace<LightComponent>(e, light);

    Transform& t = m_scene.transform(e);
    t.position = position;

    // A light shines along its local -Z axis. Turn the entity so that axis
    // points along `travel`. The "up" only matters for the roll, but it must
    // not be parallel to the direction, which straight down is.
    const glm::vec3 up = std::abs(travel.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                    : glm::vec3(0.0f, 1.0f, 0.0f);
    t.rotation = glm::quatLookAt(travel, up);

    m_selected = e;
}

void HierarchyPanel::createReflectionProbe() {
    entt::entity e = m_scene.create("Reflection Probe");
    m_scene.registry().emplace<ReflectionProbeComponent>(e);
    m_scene.transform(e).position = {0.0f, 2.0f, 0.0f};
    m_selected = e;
}

void HierarchyPanel::onImGui() {
    ImGui::Begin("Hierarchy");

    // Scene title: the scene icon in the accent colour, the name in semibold.
    ImGui::TextColored(theme::accent(), FE_ICON_LAYERS);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::PushFont(theme::semiBold(), 0.0f);   // 0 = keep the current size
    ImGui::TextUnformatted(m_scene.name().c_str());
    ImGui::PopFont();
    ImGui::Separator();

    for (entt::entity e = m_scene.firstRoot(); e != entt::null;
         e = m_scene.registry().get<Relationship>(e).nextSibling)
        drawNode(e);

    // Left click on empty space deselects.
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered())
        m_selected = entt::null;

    // Right click on empty space: creation menu.
    if (ImGui::BeginPopupContextWindow("##HierarchyContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(FE_ICON_CIRCLE_DASHED "  Create Empty")) createEntity("Empty", builtin::MeshNone);
        if (ImGui::MenuItem(FE_ICON_CUBOID "  Create Cube"))        createEntity("Cube",  builtin::MeshCube);
        if (ImGui::MenuItem(FE_ICON_CUBOID "  Create Plane"))       createEntity("Plane", builtin::MeshPlane);
        if (ImGui::BeginMenu(FE_ICON_LIGHTBULB "  Create Light")) {
            if (ImGui::MenuItem(FE_ICON_SUN "  Directional"))       createLight(LightComponent::Type::Directional);
            if (ImGui::MenuItem(FE_ICON_LIGHTBULB "  Point"))       createLight(LightComponent::Type::Point);
            if (ImGui::MenuItem(FE_ICON_SPOTLIGHT "  Spot"))        createLight(LightComponent::Type::Spot);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(FE_ICON_GLOBE "  Create Reflection Probe")) createReflectionProbe();
        ImGui::EndPopup();
    }

    // Delete key while the panel is focused.
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        m_selected != entt::null)
        m_pendingDelete = m_selected;

    // Apply the deferred deletion now that the tree is no longer being walked.
    if (m_pendingDelete != entt::null) {
        if (m_scene.registry().valid(m_pendingDelete)) {
            // Clear the selection if it is the deleted entity or one of its
            // descendants (destroy() is recursive).
            for (entt::entity p = m_selected; p != entt::null;
                 p = m_scene.registry().get<Relationship>(p).parent) {
                if (p == m_pendingDelete) { m_selected = entt::null; break; }
            }
            m_scene.destroy(m_pendingDelete);
        }
        m_pendingDelete = entt::null;
    }

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

    // The node is drawn without text; the icon and the name follow on the same
    // line so that the icon can have its own colour. (An empty label would
    // also make the ID empty, hence the explicit pointer ID.)
    const bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(
            static_cast<intptr_t>(static_cast<std::uint32_t>(e))),
        flags, "%s", "");

    // These refer to the node, so they must come before anything else is drawn.
    if (ImGui::IsItemClicked()) m_selected = e;

    // Right click on a node: per-entity menu.
    if (ImGui::BeginPopupContextItem()) {
        m_selected = e;
        if (ImGui::MenuItem(FE_ICON_TRASH_2 "  Delete")) m_pendingDelete = e;
        ImGui::EndPopup();
    }

    const NodeIcon icon = iconFor(m_scene.registry(), e);
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextColored(icon.tint, "%s", icon.glyph);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(name.c_str());

    if (open) {
        for (entt::entity c = rel.firstChild; c != entt::null;
             c = m_scene.registry().get<Relationship>(c).nextSibling)
            drawNode(c);
        ImGui::TreePop();
    }
}

} // namespace fe
