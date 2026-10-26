#pragma once

/**
 * @file SceneFileController.h
 * @brief File menu actions and dialogs for creating, opening and saving scenes.
 */

#include "scene/Scene.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <functional>
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
 * only runs when the user asks for New, Open or to quit. In those cases the
 * Unsaved Changes dialog offers Save, Don't Save and Cancel.
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

    /**
     * @brief Opens the scene stored in @p path, like File > Open Scene.
     *
     * If the current scene has unsaved changes, the Unsaved Changes dialog
     * is shown first and the scene is only opened once the user has saved or
     * discarded them. Used by the Asset Browser (double click on a scene).
     */
    void openScene(const std::filesystem::path& path) { requestOpen(path); }

    /**
     * @brief Asks to close the editor.
     *
     * With nothing unsaved, quitConfirmed() becomes `true` right away.
     * Otherwise the Unsaved Changes dialog opens, and it becomes `true` only
     * once the user has saved or chosen to discard the changes. Cancelling
     * leaves the editor open. Call it from the Exit menu and when the window
     * is asked to close.
     */
    void requestQuit();

    /// @return `true` once the editor may close without losing work.
    bool quitConfirmed() const { return m_quitConfirmed; }

    /// Records the current scene as saved. Call after building a startup scene.
    void markClean();

    /// @return `true` if the scene differs from the last saved version.
    bool isDirty() const;

    /// Sets a function to call right after New or Open has replaced the scene
    /// (the undo history uses it to start over).
    void setSceneReplacedCallback(std::function<void()> callback) { m_onSceneReplaced = std::move(callback); }

private:
    /// Action waiting for the user to confirm discarding unsaved changes.
    enum class Action { None, New, Open, Quit };

    void requestNew();
    void requestOpen(const std::filesystem::path& path);
    void requestSave();

    /// Carries out the action the Unsaved Changes dialog was holding back.
    void runPendingAction();

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

    bool m_quitConfirmed = false;   ///< Set once the editor is allowed to close.

    std::function<void()> m_onSceneReplaced;   ///< Called after New and Open.

    Action                m_pendingAction = Action::None;
    std::filesystem::path m_pendingPath;
};

} // namespace fe
