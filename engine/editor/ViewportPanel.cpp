#include "editor/ViewportPanel.h"
#include <imgui.h>

namespace fe {

void ViewportPanel::onImGui() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    ImVec2 size = ImGui::GetContentRegionAvail();
    uint32_t w  = static_cast<uint32_t>(size.x);
    uint32_t h  = static_cast<uint32_t>(size.y);

    if (w > 0 && h > 0)
        m_offscreen.resize(w, h);

    ImGui::Image(
        reinterpret_cast<ImTextureID>(m_offscreen.imguiDescriptorSet()),
        size);

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace fe
