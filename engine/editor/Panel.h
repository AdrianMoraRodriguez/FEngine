#pragma once

/**
 * @file Panel.h
 * @brief Abstract base class for all editor panels.
 *
 * Every panel in the editor inherits from Panel and implements onImGui(),
 * which is called once per frame between ImGui::NewFrame() and ImGui::Render().
 *
 * Dependencies are injected through each panel's constructor rather than
 * through onImGui() parameters. This keeps the call site in main() uniform:
 *
 * @code
 * for (auto& panel : m_panels)
 *     panel->onImGui();
 * @endcode
 */

namespace fe {

class Panel {
public:
    virtual ~Panel() = default;

    /**
     * @brief Draws this panel for the current frame.
     *
     * Must be called between ImGui::NewFrame() and ImGui::Render().
     * Each implementation opens and closes its own ImGui window.
     */
    virtual void onImGui() = 0;

    Panel(const Panel&)            = delete;
    Panel& operator=(const Panel&) = delete;

protected:
    Panel() = default;
};

} // namespace fe
