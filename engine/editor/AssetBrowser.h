#pragma once

/**
 * @file AssetBrowser.h
 * @brief File system browser panel for the project's asset directory.
 *
 * Displays the contents of the project asset root in a two-column layout:
 * a folder tree on the left and a grid of file icons on the right.
 * The user navigates folders by clicking them and can select individual
 * files (selection drives future drag-and-drop into the viewport or
 * inspector).
 *
 * The browser is read-only for now: it reflects the file system but does
 * not create, rename or delete files.
 */

#include <filesystem>
#include <vector>
#include <string>

#include "editor/Panel.h"

namespace fe {

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
 *
 * Typical use — call once per frame inside an ImGui::Begin/End pair:
 * @code
 * assetBrowser.onImGui();
 * @endcode
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
    void onImGui();

    /**
     * @brief Returns the currently selected file, or an empty path if none.
     *
     * The selection changes when the user single-clicks a file entry.
     * Directories are never reported as the selected file — clicking a
     * directory navigates into it instead.
     */
    const std::filesystem::path& selectedPath() const { return m_selected; }

private:
    /// Reads m_currentDir and populates m_entries. Called on navigation.
    void refresh();

    /// Draws the left-hand folder tree rooted at @p dir, recursively.
    void drawFolderTree(const std::filesystem::path& dir);

    /// Draws one icon+label tile for @p entry in the right-hand grid.
    void drawEntry(const AssetEntry& entry);

    /// Returns a short icon string for @p entry based on its extension.
    static const char* iconFor(const AssetEntry& entry);

    std::filesystem::path        m_root;        ///< Asset root (never navigated above).
    std::filesystem::path        m_currentDir;  ///< Directory currently shown in the grid.
    std::filesystem::path        m_selected;    ///< Currently selected file, or empty.
    std::vector<AssetEntry>      m_entries;     ///< Cached contents of m_currentDir.
};

} // namespace fe
