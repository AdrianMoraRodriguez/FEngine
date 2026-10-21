#pragma once

/**
 * @file SceneFileController.h
 * @brief File menu actions and dialogs for creating, opening and saving scenes.
 */

#include "scene/Scene.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fe {

/**
 * @brief Drives New / Open / Save / Save As for the editor's active scene.
 *
 * Scenes are stored as `.fescene` files inside a single scenes directory
 * (normally `assets/scenes`). The controller replaces the contents of the
 * scene object it was given, so panels that hold a reference to that scene
 * keep working across New and Open.
 *
 * Unsaved changes are detected by comparing the serialised scene with the
 * text of the last save, so no hook is needed on every edit. The comparison
 * only runs when the user asks for New or Open.
 *
 * Usage, once per frame:
 * @code
 * if (ImGui::BeginMenu("File")) {
 *     files.drawMenuItems();
 *     ImGui::EndMenu();
 * }
 * // ... after the panels:
 * files.onImGui();   // shortcuts and dialogs
 * @endcode
 */
class SceneFileController {
public:
    /**
     * @param scene          The active scene. Its contents are replaced by New and Open.
     * @param selectedEntity The editor selection, cleared whenever the scene is replaced.
     * @param scenesDir      Directory where scene files are saved and listed.
     */
    SceneFileController(Scene& scene,
                        entt::entity& selectedEntity,
                        std::filesystem::path scenesDir);

    /// Draws the New / Open / Save / Save As entries. Call inside the File menu.
    void drawMenuItems();

    /**
     * @brief Handles keyboard shortcuts and draws the modal dialogs.
     *
     * Call once per frame between ImGui::NewFrame() and ImGui::Render(),
     * outside any other window, so the dialogs share one ID scope.
     */
    void onImGui();

    /// Records the current scene as saved. Call after building a startup scene.
    void markClean();

    /// @return `true` if the scene differs from the last saved version.
    bool isDirty() const;

private:
    /// Action waiting for the user to confirm discarding unsaved changes.
    enum class Action { None, New, Open };

    void requestNew();
    void requestOpen(const std::filesystem::path& path);
    void requestSave();

    void doNew();
    void doOpen(const std::filesystem::path& path);
    bool doSave(const std::filesystem::path& path);

    void handleShortcuts();
    void drawOpenModal();
    void drawSaveAsModal();
    void drawUnsavedModal();

    void refreshSceneList();

    /// Builds the target file for a user-typed name, or an empty path if invalid.
    std::filesystem::path pathForName(const char* name) const;

    Scene&                m_scene;
    entt::entity&         m_selected;
    std::filesystem::path m_scenesDir;
    std::filesystem::path m_currentPath;   ///< Empty until the scene is saved or opened.
    std::string           m_savedSnapshot; ///< Serialised scene at the last save/open.

    std::vector<std::filesystem::path> m_sceneFiles; ///< Cached list for the Open dialog.
    char m_nameBuf[128] = {};                        ///< Text field of the Save As dialog.

    // Dialog requests. Menu items cannot open popups directly: a popup must
    // be opened in the same ImGui ID scope where it is drawn, so the menu
    // raises a flag and onImGui() opens the popup.
    bool m_requestOpenDialog   = false;
    bool m_requestSaveAsDialog = false;
    bool m_requestUnsavedDialog = false;

    Action                m_pendingAction = Action::None;
    std::filesystem::path m_pendingPath;
};

} // namespace fe
