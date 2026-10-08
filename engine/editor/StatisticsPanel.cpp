#include "editor/StatisticsPanel.h"
#include <imgui.h>

namespace fe {

void StatisticsPanel::onImGui() {
    ImGui::Begin("Statistics");
    ImGui::Text("Frame time : %.3f ms", m_deltaTime * 1000.0);
    ImGui::Text("FPS        : %.1f",
                m_deltaTime > 0.0 ? 1.0 / m_deltaTime : 0.0);
    ImGui::Text("Frame #    : %llu",   m_frameCount);
    ImGui::Text("Entities   : %zu",
                m_scene.registry().storage<entt::entity>()->size());
    ImGui::Text("Viewport   : %u x %u",
                m_renderer.swapchainExtent().width,
                m_renderer.swapchainExtent().height);
    ImGui::End();
}

} // namespace fe
