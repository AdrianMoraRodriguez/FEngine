#pragma once

/**
 * @file AssetBrowser.h
 * @brief File system browser panel for the project's asset directory.
 *
 * Displays the contents of the project asset root in a two-column layout:
 * a folder tree on the left and a grid of file icons on the right.
 * The user navigates folders by clicking them and can select individual
 * files.
 *
 * Model files the engine can import (see isSupportedModel()) can be dragged
 * out of the grid and dropped
 * into the viewport to add them to the scene. The drag carries the file's
 * absolute path, as UTF-8 text, in a payload named "FE_MODEL".
 *
 * Each entry is drawn with a coloured icon that depends on its kind (folder,
 * model, image, scene, source file); see EditorTheme.h for the colours.
 *
 * Right-clicking a tile (or the empty part of the grid) opens a context menu
 * with Copy, Cut, Paste, Rename, Delete and New Folder, which work on the real
 * files on disk. The same actions have keyboard shortcuts while the browser is
 * focused: Ctrl+C, Ctrl+X, Ctrl+V, F2 and Delete, acting on the selected file.
 * Delete always asks for confirmation and is permanent (no recycle bin).
 *
 * File operations never run in the middle of drawing the grid, because they
 * rebuild the list the grid is walking. Menus and shortcuts only record what
 * was asked for, and the browser carries it out once the grid is finished.
 */

#include "editor/Panel.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fe {

class ThumbnailRenderer;

/**
 * @brief Metadata for a single entry shown in the asset grid.
 */
struct AssetEntry {
    std::filesystem::path path;  ///< Absolute path to the file or directory.
    std::string           name;  ///< Display name (filename only).
    bool                  isDir; ///< True if this entry is a directory.
};

/**
 * @brief ImGui panel that browses the project asset directory.
 */
class AssetBrowser : public Panel {
public:
    /**
     * @brief Constructs the browser rooted at @p assetRoot.
     *
     * @param assetRoot Absolute or relative path to the project asset
     *                  directory. The browser never navigates above this root.
     */
    explicit AssetBrowser(std::filesystem::path assetRoot);

    /**
     * @brief Draws the Asset Browser panel for the current frame.
     *
     * Must be called between ImGui::NewFrame() and ImGui::Render().
     * Opens its own ImGui::Begin/End window named "Asset Browser".
     */
    void onImGui() override;

    /**
     * @brief Returns the currently selected file, or an empty path if none.
     *
     * The selection changes when the user single-clicks a file entry.
     * Directories are never reported as the selected file: clicking a
     * directory navigates into it instead.
     */
    const std::filesystem::path& selectedPath() const { return m_selected; }

    /**
     * @brief Lets model tiles show a small picture of the model.
     *
     * Without it (or while a picture is not ready) a model tile shows its
     * plain icon. The renderer must outlive the browser.
     */
    void setThumbnails(ThumbnailRenderer* thumbnails) { m_thumbnails = thumbnails; }

    /**
     * @brief Returns the scene file the user double-clicked, once.
     *
     * Call after onImGui(). The path is handed over and forgotten, so the
     * next call returns an empty path until another scene is double-clicked.
     * The browser does not open the scene itself: the caller decides what
     * opening means (see SceneFileController::openScene()).
     */
    std::filesystem::path takeSceneToOpen() {
        std::filesystem::path path = std::move(m_sceneToOpen);
        m_sceneToOpen.clear();
        return path;
    }

private:
    /// Reads m_currentDir and populates m_entries. Called on navigation.
    void refresh();

    /// Draws the left-hand folder tree rooted at @p dir, recursively.
    void drawFolderTree(const std::filesystem::path& dir);

    /// Draws one icon+label tile for @p entry in the right-hand grid.
    void drawEntry(const AssetEntry& entry);

    /// Something asked for by a menu item or a shortcut, applied later.
    enum class Op { None, Copy, Cut, Paste, Rename, NewFolder, Delete };

    /// Records @p op on @p target; applyPendingOp() carries it out.
    void requestOp(Op op, std::filesystem::path target);

    /// Right-click menu of one tile.
    void drawEntryMenu(const AssetEntry& entry);

    /// Right-click menu of the empty part of the grid.
    void drawGridMenu();

    /// Ctrl+C / Ctrl+X / Ctrl+V / F2 / Delete on the selected file.
    void handleShortcuts();

    /// Carries out the recorded operation. Call once the grid is drawn.
    void applyPendingOp();

    /// Pastes the clipboard (copied or cut item) into @p destDir.
    void paste(const std::filesystem::path& destDir);

    /// Draws the Rename / New Folder, Delete and Error dialogs.
    void drawDialogs();

    /// Shows the Error dialog with @p message.
    void fail(std::string message);

    std::filesystem::path   m_root;        ///< Asset root (never navigated above).
    std::filesystem::path   m_currentDir;  ///< Directory currently shown in the grid.
    std::filesystem::path   m_selected;    ///< Currently selected file, or empty.
    ThumbnailRenderer*      m_thumbnails = nullptr;   ///< Source of model pictures, or null.
    std::filesystem::path   m_sceneToOpen; ///< Scene double-clicked this frame, or empty.
    std::filesystem::path   m_pendingDir;  ///< Folder to open once the grid is drawn, or empty.
    std::vector<AssetEntry> m_entries;     ///< Cached contents of m_currentDir.

    // ---- File operations ----
    Op                    m_op = Op::None;  ///< Operation waiting to be applied.
    std::filesystem::path m_opTarget;       ///< File/folder it applies to (destination for Paste).

    std::filesystem::path m_clipboard;      ///< Item copied or cut, or empty.
    bool                  m_clipboardCut = false;

    // ---- Dialogs ----
    // Popups must be opened in the ID scope where they are drawn, so menus
    // only raise a flag and drawDialogs() opens them.
    bool                  m_openNameDialog   = false;
    bool                  m_openDeleteDialog = false;
    bool                  m_openErrorDialog  = false;
    bool                  m_nameDialogIsNewFolder = false;
    bool                  m_focusNameField   = false;
    std::filesystem::path m_dialogTarget;   ///< Item being renamed/deleted; parent for New Folder.
    char                  m_nameBuf[256]     = {};
    std::string           m_error;          ///< Text of the Error dialog.
};

} // namespace fe
