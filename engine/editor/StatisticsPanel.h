#pragma once

#include "editor/Panel.h"
#include "scene/Scene.h"
#include "gfx/Renderer.h"
#include <cstdint>

namespace fe {

class StatisticsPanel : public Panel {
public:
    StatisticsPanel(const Scene& scene,
                    const Renderer& renderer,
                    const double& deltaTime,
                    const uint64_t& frameCount)
        : m_scene(scene)
        , m_renderer(renderer)
        , m_deltaTime(deltaTime)
        , m_frameCount(frameCount) {}

    void onImGui() override;

private:
    const Scene&    m_scene;
    const Renderer& m_renderer;
    const double&   m_deltaTime;
    const uint64_t& m_frameCount;
};

} // namespace fe
