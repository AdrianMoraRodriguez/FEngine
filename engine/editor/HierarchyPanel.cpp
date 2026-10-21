#include "editor/HierarchyPanel.h"

#include <imgui.h>

namespace fe {

void HierarchyPanel::createEntity(const char* name, AssetHandle mesh) {
    entt::entity e = m_scene.create(name);

    if (mesh != builtin::MeshNone) {
        MeshRenderer renderer;
        renderer.mesh = mesh;
        m_scene.registry().emplace<MeshRenderer>(e, renderer);
    }

    m_selected = e;
}

void HierarchyPanel::onImGui() {
    ImGui::Begin("Hierarchy");
    ImGui::Text("Scene: %s", m_scene.name().c_str());
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
        if (ImGui::MenuItem("Create Empty")) createEntity("Empty", builtin::MeshNone);
        if (ImGui::MenuItem("Create Cube"))  createEntity("Cube",  builtin::MeshCube);
        if (ImGui::MenuItem("Create Plane")) createEntity("Plane", builtin::MeshPlane);
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

    bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(
            static_cast<intptr_t>(static_cast<std::uint32_t>(e))),
        flags, "%s", name.c_str());

    if (ImGui::IsItemClicked()) m_selected = e;

    // Right click on a node: per-entity menu.
    if (ImGui::BeginPopupContextItem()) {
        m_selected = e;
        if (ImGui::MenuItem("Delete")) m_pendingDelete = e;
        ImGui::EndPopup();
    }

    if (open) {
        for (entt::entity c = rel.firstChild; c != entt::null;
             c = m_scene.registry().get<Relationship>(c).nextSibling)
            drawNode(c);
        ImGui::TreePop();
    }
}

} // namespace fe
