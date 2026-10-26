#pragma once

/**
 * @file AssetBrowser.h
 * @brief File explorer panel for the project's asset directory.
 *
 * The panel works like the file explorer of the operating system:
 *
 *  - A folder tree on the left and the contents of the current folder on the
 *    right, shown as icons or as a list with details (modified date, type and
 *    size). In the list, clicking a column header sorts by it.
 *  - A single click selects, a double click opens: a folder is entered and a
 *    scene is opened (see takeSceneToOpen()). Ctrl+click toggles an item,
 *    Shift+click selects a range, and dragging a rectangle over empty space
 *    selects everything it touches.
 *  - Back, Forward and Up buttons (also Alt+Left, Alt+Right, Alt+Up and the
 *    mouse's side buttons) with a history, a clickable path bar and a search
 *    field that filters the current folder by name.
 *  - Right-click menus and the usual shortcuts: Ctrl+C, Ctrl+X, Ctrl+V,
 *    Ctrl+A, F2 (rename), Delete, F5 (refresh), Ctrl+F (search) and Enter
 *    (open). They act on the real files on disk and on the whole selection.
 *    Delete always asks for confirmation and is permanent (no recycle bin).
 *  - Items can be dragged onto a folder (tile, tree entry or path segment) to
 *    move them, and files can be dropped on the panel from the OS file
 *    explorer to copy them in (see onFilesDropped()).
 *  - The folder being shown is checked for changes made outside the editor
 *    about once a second, so new files appear by themselves.
 *
 * ### Drag payloads
 * Every item carries its absolute path as UTF-8 text. A single model is sent
 * as "FE_MODEL" (the viewport accepts it), a single loadable image as
 * "FE_IMAGE" (a material's texture slot accepts it) and everything else, and
 * any selection of several items, as "FE_ASSET" with one path per line.
 *
 * ### File operations and drawing
 * File operations never run in the middle of drawing the grid, because they
 * rebuild the list the grid is walking. Menus, shortcuts and drops only record
 * what was asked for, and the browser carries it out once the grid is finished.
 */

#include "editor/Panel.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

namespace fe {

class ThumbnailRenderer;
class ImageThumbnails;

/**
 * @brief Metadata for a single entry shown in the asset grid.
 */
struct AssetEntry {
    std::filesystem::path path;      ///< Absolute path to the file or directory.
    std::string           name;      ///< Display name (filename only).
    std::string           nameLower; ///< name in lower case, for sorting and searching.
    bool                  isDir = false; ///< True if this entry is a directory.

    // Shown in the list view and used for sorting. Read once by refresh().
    std::uintmax_t        size = 0;      ///< Bytes (0 for a folder).
    std::int64_t          modified = 0;  ///< Last write time, seconds since the Unix epoch.
    std::string           typeName;      ///< "Folder", "PNG image", "Scene"...
    std::string           sizeText;      ///< "12.4 KB", or empty for a folder.
    std::string           modifiedText;  ///< Local date and time, "2026-10-25 16:20".
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
     * @brief Returns the item the user clicked last, or an empty path if none.
     *
     * With several items selected this is the one clicked last; use
     * selection() for all of them.
     */
    const std::filesystem::path& selectedPath() const { return m_primary; }

    /// @return Every selected item (absolute paths), in path order.
    const std::set<std::filesystem::path>& selection() const { return m_selection; }

    /**
     * @brief Lets model tiles show a small picture of the model.
     *
     * Without it (or while a picture is not ready) a model tile shows its
     * plain icon. The renderer must outlive the browser.
     */
    void setThumbnails(ThumbnailRenderer* thumbnails) { m_thumbnails = thumbnails; }

    /// Source of the pictures shown on image tiles. Optional; without it they show an icon.
    void setImageThumbnails(ImageThumbnails* thumbnails) { m_imageThumbnails = thumbnails; }

    /**
     * @brief Returns the model the user double-clicked (or opened with Enter), once.
     *
     * Same hand-over as takeSceneToOpen(): the editor shows it in the model viewer.
     */
    std::filesystem::path takeModelToOpen() {
        std::filesystem::path path = std::move(m_modelToOpen);
        m_modelToOpen.clear();
        return path;
    }

    /**
     * @brief Returns the image the user double-clicked (or opened with Enter), once.
     *
     * Same hand-over as takeSceneToOpen(): the caller decides what opening
     * means (the editor shows it in the image viewer).
     */
    std::filesystem::path takeImageToOpen() {
        std::filesystem::path path = std::move(m_imageToOpen);
        m_imageToOpen.clear();
        return path;
    }

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

    /**
     * @brief Hands over files or folders dropped from the OS file explorer.
     *
     * They are copied into the folder under the mouse cursor: a folder tile
     * or tree entry if the cursor is over one, otherwise the folder being
     * shown. Dropped anywhere else in the editor, they are ignored. The copy
     * happens during the next onImGui(), when the cursor's position is known
     * in terms of the panel.
     */
    void onFilesDropped(std::vector<std::filesystem::path> files) {
        for (auto& file : files) m_dropped.push_back(std::move(file));
    }

private:
    // ---- What can be sorted by, and how the grid is laid out ----
    enum class SortKey { Name, Modified, Type, Size };
    enum class ViewMode { Icons, List };

    /// Something asked for by a menu item or a shortcut, applied later.
    enum class Op { None, Copy, Cut, Paste, Rename, NewFolder, Delete };

    /// A navigation asked for by a button or shortcut, applied later.
    enum class Nav { None, Back, Forward, Up };

    // ---- Folder contents ----

    /// Reads m_currentDir into m_entries and rebuilds the visible list.
    void refresh();

    /// Filters and sorts m_entries into m_visible. Cheap: no disk access.
    void rebuildVisible();

    /// Refreshes if the folder was changed by someone else. Polls once a second.
    void refreshIfChangedOnDisk();

    // ---- Navigation ----

    /// Makes @p dir the shown folder and remembers where we came from.
    void navigateTo(const std::filesystem::path& dir);

    /// Carries out the recorded navigation. Call once the grid is drawn.
    void applyPendingNavigation();

    // ---- Selection ----
    bool isSelected(const std::filesystem::path& path) const { return m_selection.count(path) > 0; }
    void selectOnly(const std::filesystem::path& path);

    /// Selects the visible items from index @p a to @p b (either order).
    void selectRange(size_t a, size_t b);

    /// Forgets selected items that are no longer in the folder.
    void pruneSelection();

    // ---- Drawing ----

    /// Back / Forward / Up and the path bar.
    void drawPathBar();

    /// Search field and the view menu.
    void drawToolbar();

    /// The View menu: icons or list, icon size, sort order.
    void drawViewMenu();

    /// Draws the left-hand folder tree rooted at @p dir, recursively.
    void drawFolderTree(const std::filesystem::path& dir);

    /// The contents of the folder as a grid of tiles.
    void drawIcons();

    /// The contents of the folder as a table with details.
    void drawList();

    /// Draws one icon+label tile for @p entry in the grid.
    void drawTile(const AssetEntry& entry, size_t visibleIndex);

    /// Draws one row of the list view.
    void drawRow(const AssetEntry& entry, size_t visibleIndex);

    /// Click, double click, drag and context menu of the item just drawn.
    /// Shared by tiles and rows; it must come right after the item.
    void interact(const AssetEntry& entry, size_t visibleIndex);

    /// Rectangle selection over empty space. Call after all items are drawn.
    void handleRubberBand();

    // ---- Menus, shortcuts and operations ----

    /// Records @p op on @p targets; applyPendingOp() carries it out.
    void requestOp(Op op, std::vector<std::filesystem::path> targets);

    /// Right-click menu of the selection (after a tile was right-clicked).
    void drawEntryMenu(const AssetEntry& entry);

    /// Right-click menu of the empty part of the grid.
    void drawGridMenu();

    /// Keyboard shortcuts of the panel.
    void handleShortcuts();

    /// Carries out the recorded operation. Call once the grid is drawn.
    void applyPendingOp();

    /// Pastes the clipboard (copied or cut items) into @p destDir.
    void paste(const std::filesystem::path& destDir);

    /// Copies the files waiting in m_dropped into their target folder.
    void applyDrop();

    /// Makes the last item drawn a place where assets dragged from the grid
    /// can be dropped: they are moved into @p destDir (see moveItems()).
    void acceptMoveDrop(const std::filesystem::path& destDir);

    /// Moves files and folders into @p destDir. Call once the grid is drawn.
    void moveItems(const std::vector<std::filesystem::path>& sources,
                   const std::filesystem::path& destDir);

    /// Draws the Rename / New Folder, Delete and Error dialogs.
    void drawDialogs();

    /// Shows the Error dialog with @p message.
    void fail(std::string message);

    // ---- State ----
    std::filesystem::path   m_root;        ///< Asset root (never navigated above).
    std::filesystem::path   m_currentDir;  ///< Directory currently shown.
    ThumbnailRenderer*      m_thumbnails = nullptr;   ///< Source of model pictures, or null.
    ImageThumbnails*        m_imageThumbnails = nullptr; ///< Source of image pictures, or null.
    std::filesystem::path   m_sceneToOpen; ///< Scene double-clicked this frame, or empty.
    std::filesystem::path   m_imageToOpen; ///< Image double-clicked this frame, or empty.
    std::filesystem::path   m_modelToOpen; ///< Model double-clicked this frame, or empty.
    std::filesystem::path   m_pendingDir;  ///< Folder to open once the grid is drawn, or empty.
    Nav                     m_pendingNav = Nav::None;
    std::vector<AssetEntry> m_entries;     ///< Everything in m_currentDir.
    std::vector<size_t>     m_visible;     ///< Indices into m_entries: filtered and sorted.

    // ---- History ----
    std::vector<std::filesystem::path> m_back;      ///< Folders visited before this one (last = most recent).
    std::vector<std::filesystem::path> m_forward;   ///< Folders left by going back.

    // ---- Selection ----
    std::set<std::filesystem::path> m_selection;    ///< Selected items.
    std::filesystem::path           m_primary;      ///< Clicked last.
    std::filesystem::path           m_anchor;       ///< Where Shift+click ranges start.
    std::filesystem::path           m_collapseTo;   ///< A selected item pressed without Ctrl/Shift: becomes the only one on release unless dragged.

    // ---- Rectangle selection ----
    struct ItemRect { size_t visible; ImVec2 min; ImVec2 max; };
    std::vector<ItemRect>           m_itemRects;    ///< Where the items were drawn this frame.
    bool                            m_bandActive = false;
    ImVec2                          m_bandStart{0.0f, 0.0f};  ///< Start, in content coordinates (scrolls with the list).
    std::set<std::filesystem::path> m_bandBase;     ///< Selection kept while Ctrl is held.

    // ---- View ----
    ViewMode m_viewMode      = ViewMode::Icons;
    float    m_tileSize      = 72.0f;      ///< Side of a tile in the icon view.
    SortKey  m_sortKey       = SortKey::Name;
    bool     m_sortAscending = true;
    char     m_filter[128]   = {};         ///< Search text.
    bool     m_focusSearch   = false;      ///< Ctrl+F was pressed.
    bool     m_syncListSort  = true;       ///< The list's header arrows must be set from m_sortKey.

    // ---- Changes made outside the editor ----
    double                          m_lastPoll = 0.0;
    std::filesystem::file_time_type m_dirStamp{};   ///< Last write time of m_currentDir when it was read.

    // ---- Files dropped from the OS ----
    std::vector<std::filesystem::path> m_dropped;       ///< Waiting to be copied in.
    std::filesystem::path              m_hoveredFolder; ///< Folder under the cursor this frame, or empty.

    // ---- Drag and drop inside the browser ----
    std::vector<std::filesystem::path> m_moveSources;   ///< Items dropped on a folder this frame.
    std::filesystem::path              m_moveDest;      ///< The folder they were dropped on.

    // ---- File operations ----
    Op                                 m_op = Op::None;  ///< Operation waiting to be applied.
    std::vector<std::filesystem::path> m_opTargets;      ///< Items it applies to (the destination folder for Paste).

    std::vector<std::filesystem::path> m_clipboard;      ///< Items copied or cut, or empty.
    bool                               m_clipboardCut = false;

    // ---- Dialogs ----
    // Popups must be opened in the ID scope where they are drawn, so menus
    // only raise a flag and drawDialogs() opens them.
    bool                  m_openNameDialog   = false;
    bool                  m_openDeleteDialog = false;
    bool                  m_openErrorDialog  = false;
    bool                  m_nameDialogIsNewFolder = false;
    bool                  m_focusNameField   = false;
    std::filesystem::path m_dialogTarget;   ///< Item being renamed; parent for New Folder.
    std::vector<std::filesystem::path> m_deleteTargets;   ///< Items the Delete dialog asks about.
    char                  m_nameBuf[256]     = {};
    std::string           m_error;          ///< Text of the Error dialog.
};

} // namespace fe
