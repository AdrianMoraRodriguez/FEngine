#pragma once

/**
 * @file ProjectSettingsPanel.h
 * @brief The window where the project's quality settings are edited.
 */

#include "editor/Panel.h"

#include "core/ProjectSettings.h"

#include <filesystem>
#include <utility>

namespace fe {

/**
 * @brief Edits a ProjectSettings and keeps its file up to date.
 *
 * Each change is written to the file once the control that made it is let go
 * (so dragging a slider does not write on every frame). The renderer reads the
 * same object every frame, so changes show immediately, except the ones the
 * window marks as needing a restart.
 *
 * The window is hidden at first; Edit > Project Settings shows it.
 */
class ProjectSettingsPanel : public Panel {
public:
    /**
     * @param settings The settings to edit (owned by the caller, shared with the renderer).
     * @param file     Where they are saved.
     */
    ProjectSettingsPanel(ProjectSettings& settings, std::filesystem::path file)
        : m_settings(settings), m_file(std::move(file)), m_atStart(settings) {}

    void onImGui() override;

    /// Whether the window is shown. The Edit menu toggles it.
    bool visible = false;

private:
    ProjectSettings&      m_settings;
    std::filesystem::path m_file;

    /// The settings as the editor started with them: what the ones that need
    /// a restart are compared against.
    ProjectSettings m_atStart;

    bool m_dirty = false;   ///< Something changed and is not saved yet.
};

} // namespace fe
