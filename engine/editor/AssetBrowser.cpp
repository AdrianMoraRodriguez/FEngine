#include "editor/AssetBrowser.h"
#include "editor/ImageThumbnails.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "core/PlatformShell.h"
#include "editor/EditorTheme.h"
#include "editor/Icons.h"
#include "gfx/ThumbnailRenderer.h"
#include "import/AssetDatabase.h"
#include "import/ModelImporter.h"

#include <imgui.h>
#include <imgui_internal.h>   // TableSetColumnSortDirection: keeps the list's header arrows in step with the View menu

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <system_error>

namespace fe {
namespace {

/// @return The extension of @p path in lower case, including the dot.
std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = toUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// @return @p text in lower case (ASCII letters only, which is enough to sort
///         and to search file names without a Unicode library).
std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Returns true if @p ext is a recognised image format.
bool isTexture(const std::string& ext) {
    return ext == ".png"  || ext == ".jpg" || ext == ".jpeg"
        || ext == ".hdr"  || ext == ".tga" || ext == ".dds" || ext == ".bmp";
}

/// Returns true if @p ext is an image ImageLoader can decode (.dds is shown as
/// an image, but cannot be used as a texture yet). A .hdr works as the scene's
/// environment; as an ordinary texture it is clamped to the displayable range.
bool isLoadableImage(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga"
        || ext == ".hdr";
}

/// Returns true if @p ext is a recognised scene file.
bool isScene(const std::string& ext) {
    return ext == ".fescene";
}

/// Returns true if @p ext is a shader file.
bool isCode(const std::string& ext) {
    return ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl"
        || ext == ".spv";
}

/// Shortens UTF-8 text to at most @p maxBytes bytes, adding an ellipsis.
/// Never cuts in the middle of a multi-byte character, which would leave
/// invalid text behind.
std::string shorten(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;

    size_t cut = maxBytes - 1;   // room for the ellipsis
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;                   // step back to the start of a character

    return text.substr(0, cut) + "…";
}

/// Icon glyph and colour that represent one entry in the browser.
struct EntryIcon {
    const char* glyph;   ///< UTF-8 icon from Icons.h.
    ImVec4      tint;    ///< Colour the glyph is drawn with.
};

/// @return The icon that best describes @p entry, chosen by kind and extension.
EntryIcon iconFor(const AssetEntry& entry) {
    if (entry.isDir)                   return {FE_ICON_FOLDER,      theme::tintFolder()};
    if (isSupportedModel(entry.path))  return {FE_ICON_CUBOID,      theme::tintModel()};

    const std::string ext = lowerExtension(entry.path);
    if (isTexture(ext))                return {FE_ICON_IMAGE,       theme::tintTexture()};
    if (isScene(ext))                  return {FE_ICON_CLAPPERBOARD, theme::tintScene()};
    if (isCode(ext))                   return {FE_ICON_FILE_CODE,   theme::tintCode()};
    if (ext == ".json")                return {FE_ICON_FILE_JSON,   theme::tintCode()};
    if (ext == ".txt" || ext == ".md") return {FE_ICON_FILE_TEXT,   theme::tintFile()};
    return {FE_ICON_FILE, theme::tintFile()};
}

/// @return What the list view's Type column says about @p path.
std::string typeNameFor(const std::filesystem::path& path, bool isDir) {
    if (isDir) return "Folder";

    const std::string ext = lowerExtension(path);
    std::string upper = ext.size() > 1 ? ext.substr(1) : std::string{};
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (isSupportedModel(path)) return upper + " model";
    if (isTexture(ext))         return upper + " image";
    if (isScene(ext))           return "Scene";
    if (isCode(ext))            return "Shader";
    if (ext == ".json")         return "JSON file";
    if (ext == ".txt")          return "Text file";
    if (ext == ".md")           return "Markdown file";
    return upper.empty() ? "File" : upper + " file";
}

/// @return A size such as "512 B", "12.4 KB" or "3.1 MB".
std::string formatSize(std::uintmax_t bytes) {
    char text[32];
    if (bytes < 1024)
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024)
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

/// @return Seconds since the Unix epoch for a file time.
std::int64_t toUnixSeconds(std::filesystem::file_time_type time) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

/// @return "2026-10-25 16:20" in the local time zone.
std::string formatTime(std::int64_t unixSeconds) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

/// Draws a button that looks like plain text until hovered, for the
/// path bar and the toolbar.
bool flatButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor();
    return pressed;
}

/// Builds a path from UTF-8 text, such as the contents of a text field.
std::filesystem::path pathFromUtf8(const char* text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text)));
}

/// @return `true` if @p name can be the name of a file or folder on every
///         platform the editor targets (Windows is the strictest).
bool isValidName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.back() == ' ' || name.back() == '.')    return false;
    for (unsigned char c : name)
        if (c < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c))
                            != std::string_view::npos)
            return false;
    return true;
}

/// @return `true` if @p path is @p folder itself or lies inside it.
bool isInsideOrSame(const std::filesystem::path& path,
                    const std::filesystem::path& folder) {
    const std::filesystem::path rel =
        path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

/// @return A path for @p name inside @p dir that does not exist yet: the
///         name itself if it is free, otherwise "name (2)", "name (3)", ...
///         The number goes before the extension of a file.
std::filesystem::path uniquePath(const std::filesystem::path& dir,
                                 const std::filesystem::path& name, bool isDir) {
    std::error_code ec;
    std::filesystem::path candidate = dir / name;
    if (!std::filesystem::exists(candidate, ec)) return candidate;

    const std::u8string stem = isDir ? name.u8string() : name.stem().u8string();
    const std::u8string ext  = isDir ? std::u8string{} : name.extension().u8string();

    for (int n = 2; n < 10000; ++n) {
        const std::string number = std::to_string(n);
        std::u8string numbered = stem + u8" (";
        numbered.append(number.begin(), number.end());
        numbered += u8")";
        numbered += ext;
        candidate = dir / std::filesystem::path(numbered);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return dir / name;   // practically unreachable; the caller reports the failure
}

/// Renames @p source to @p dest; if that is not possible (another drive),
/// copies it and removes the original.
void moveOnDisk(const std::filesystem::path& source, const std::filesystem::path& dest,
                std::error_code& ec) {
    std::filesystem::rename(source, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy(source, dest, std::filesystem::copy_options::recursive, ec);
        if (!ec) std::filesystem::remove_all(source, ec);
    }

    // Scenes refer to assets by id: the id must follow the file.
    if (!ec) AssetDatabase::instance().moved(source, dest);
}

/// @return The paths in a drag payload: UTF-8 text, one path per line.
std::vector<std::filesystem::path> pathsFromPayload(const ImGuiPayload& payload) {
    const std::string text(static_cast<const char*>(payload.Data));
    std::vector<std::filesystem::path> paths;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) paths.push_back(pathFromUtf8(text.substr(start, end - start).c_str()));
        start = end + 1;
    }
    return paths;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction, folder contents, navigation
// ---------------------------------------------------------------------------

AssetBrowser::AssetBrowser(std::filesystem::path assetRoot)
    : m_root(std::move(assetRoot))
    , m_currentDir(m_root) {
    refresh();
}

void AssetBrowser::refresh() {
    m_entries.clear();

    std::error_code ec;
    if (!std::filesystem::exists(m_currentDir, ec)) {
        FE_WARN("AssetBrowser: directory does not exist: %s",
                toUtf8(m_currentDir).c_str());
        m_currentDir = m_root;
    }

    // Advanced with increment(ec): the range-for operator++ throws when one
    // entry cannot be read, and one odd file must not be able to crash the
    // editor.
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(m_currentDir, ec);
         !ec && it != end; it.increment(ec)) {

        // Skip hidden files (starting with '.') like .gitkeep.
        const std::string filename = toUtf8(it->path().filename());
        if (!filename.empty() && filename[0] == '.') continue;

        // The ".meta" sidecars hold asset ids (see AssetDatabase). They travel
        // with their file and are never shown or handled by hand.
        if (AssetDatabase::isMetaFile(it->path())) continue;

        std::error_code entryEc;

        AssetEntry ae;
        ae.path      = it->path();
        ae.name      = filename;
        ae.nameLower = lowerCopy(filename);
        ae.isDir     = it->is_directory(entryEc);

        if (!ae.isDir) {
            const std::uintmax_t bytes = it->file_size(entryEc);
            if (!entryEc) { ae.size = bytes; ae.sizeText = formatSize(bytes); }
        }
        const auto written = it->last_write_time(entryEc);
        if (!entryEc) {
            ae.modified     = toUnixSeconds(written);
            ae.modifiedText = formatTime(ae.modified);
        }
        ae.typeName = typeNameFor(ae.path, ae.isDir);

        m_entries.push_back(std::move(ae));
    }

    std::error_code stampEc;
    m_dirStamp = std::filesystem::last_write_time(m_currentDir, stampEc);

    pruneSelection();
    rebuildVisible();
}

void AssetBrowser::rebuildVisible() {
    const std::string filter = lowerCopy(m_filter);

    m_visible.clear();
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (filter.empty() || m_entries[i].nameLower.find(filter) != std::string::npos)
            m_visible.push_back(i);

    // Items hidden by the search must not stay selected: Delete would act on
    // things the user cannot see.
    for (auto it = m_selection.begin(); it != m_selection.end();) {
        const bool shown = std::any_of(m_visible.begin(), m_visible.end(),
            [&](size_t i) { return m_entries[i].path == *it; });
        it = shown ? std::next(it) : m_selection.erase(it);
    }
    if (!m_selection.count(m_primary)) m_primary.clear();

    // Folders first, whatever the order; then by the chosen column, and by
    // name when two items tie.
    std::sort(m_visible.begin(), m_visible.end(), [this](size_t ia, size_t ib) {
        const AssetEntry& a = m_entries[ia];
        const AssetEntry& b = m_entries[ib];
        if (a.isDir != b.isDir) return a.isDir > b.isDir;

        int order = 0;   // < 0: a comes first when ascending
        switch (m_sortKey) {
            case SortKey::Name:     break;
            case SortKey::Modified: order = (a.modified < b.modified) ? -1 : (a.modified > b.modified) ? 1 : 0; break;
            case SortKey::Type:     order = a.typeName.compare(b.typeName); break;
            case SortKey::Size:     order = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0; break;
        }
        if (order == 0) order = a.nameLower.compare(b.nameLower);
        if (order == 0) return false;
        return m_sortAscending ? order < 0 : order > 0;
    });
}

void AssetBrowser::refreshIfChangedOnDisk() {
    const double now = ImGui::GetTime();
    if (now - m_lastPoll < 1.0) return;
    m_lastPoll = now;

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(m_currentDir, ec);
    if (ec || stamp != m_dirStamp) refresh();   // an error means the folder is gone: refresh() goes to the root
}

void AssetBrowser::navigateTo(const std::filesystem::path& dir) {
    if (dir == m_currentDir) return;
    if (!isInsideOrSame(dir, m_root)) return;

    m_back.push_back(m_currentDir);
    m_forward.clear();

    m_currentDir = dir;
    m_selection.clear();
    m_primary.clear();
    m_anchor.clear();
    m_filter[0] = '\0';   // like a real explorer, a search ends when you leave the folder
    refresh();
}

void AssetBrowser::applyPendingNavigation() {
    if (!m_pendingDir.empty()) {
        const std::filesystem::path target = std::move(m_pendingDir);
        m_pendingDir.clear();
        navigateTo(target);
    }

    const Nav nav = m_pendingNav;
    m_pendingNav = Nav::None;

    // Folders in the history may have been deleted since; skip those.
    const auto goTo = [this](std::vector<std::filesystem::path>& from,
                             std::vector<std::filesystem::path>& to) {
        while (!from.empty()) {
            const std::filesystem::path target = from.back();
            from.pop_back();

            std::error_code ec;
            if (!std::filesystem::is_directory(target, ec)) continue;

            to.push_back(m_currentDir);
            m_currentDir = target;
            m_selection.clear();
            m_primary.clear();
            m_anchor.clear();
            m_filter[0] = '\0';
            refresh();
            return;
        }
    };

    switch (nav) {
        case Nav::Back:    goTo(m_back, m_forward); break;
        case Nav::Forward: goTo(m_forward, m_back); break;
        case Nav::Up:
            if (m_currentDir != m_root) navigateTo(m_currentDir.parent_path());
            break;
        case Nav::None:    break;
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void AssetBrowser::selectOnly(const std::filesystem::path& path) {
    m_selection.clear();
    m_selection.insert(path);
    m_primary = path;
}

void AssetBrowser::selectRange(size_t a, size_t b) {
    if (a > b) std::swap(a, b);
    m_selection.clear();
    for (size_t i = a; i <= b && i < m_visible.size(); ++i)
        m_selection.insert(m_entries[m_visible[i]].path);
}

void AssetBrowser::pruneSelection() {
    for (auto it = m_selection.begin(); it != m_selection.end();) {
        const bool present = std::any_of(m_entries.begin(), m_entries.end(),
            [&](const AssetEntry& e) { return e.path == *it; });
        it = present ? std::next(it) : m_selection.erase(it);
    }
    if (!m_selection.count(m_primary)) m_primary.clear();
    if (!m_selection.count(m_anchor))  m_anchor.clear();
}

// ---------------------------------------------------------------------------
// Drawing: path bar, toolbar, folder tree
// ---------------------------------------------------------------------------

void AssetBrowser::drawPathBar() {
    // Back, Forward and Up. A disabled button cannot be clicked, which is all
    // the feedback needed.
    ImGui::BeginDisabled(m_back.empty());
    if (flatButton(FE_ICON_ARROW_LEFT)) m_pendingNav = Nav::Back;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);

    ImGui::BeginDisabled(m_forward.empty());
    if (flatButton(FE_ICON_ARROW_RIGHT)) m_pendingNav = Nav::Forward;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);

    ImGui::BeginDisabled(m_currentDir == m_root);
    if (flatButton(FE_ICON_ARROW_UP)) m_pendingNav = Nav::Up;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 10);

    // The path relative to the root, every segment clickable (and a place
    // to drop items to move them there).
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(m_currentDir, m_root, ec);

    if (flatButton(FE_ICON_HOUSE " assets")) m_pendingDir = m_root;
    acceptMoveDrop(m_root);

    if (!ec) {
        std::filesystem::path accumulated = m_root;
        for (const auto& part : rel) {
            if (part == ".") continue;
            accumulated /= part;
            ImGui::SameLine(0, 2);
            ImGui::TextColored(theme::textDim(), FE_ICON_CHEVRON_RIGHT);
            ImGui::SameLine(0, 2);
            const std::string partText = toUtf8(part);
            if (flatButton(partText.c_str())) m_pendingDir = accumulated;
            acceptMoveDrop(accumulated);
        }
    }

    ImGui::SameLine();
    if (flatButton(FE_ICON_REFRESH_CW)) refresh();
    ImGui::SetItemTooltip("Refresh (F5)");
}

void AssetBrowser::drawToolbar() {
    const float buttonWidth = ImGui::GetFrameHeight();

    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputTextWithHint("##AssetSearch", FE_ICON_SEARCH "  Search this folder", m_filter, sizeof(m_filter)))
        rebuildVisible();

    // Escape inside the field clears it.
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_filter[0] = '\0';
        rebuildVisible();
    }

    ImGui::SameLine();
    if (ImGui::Button(m_viewMode == ViewMode::Icons ? FE_ICON_LAYOUT_GRID : FE_ICON_LIST,
                      ImVec2(buttonWidth, 0.0f)))
        ImGui::OpenPopup("##AssetViewMenu");
    ImGui::SetItemTooltip("View and sort");

    if (ImGui::BeginPopup("##AssetViewMenu")) {
        drawViewMenu();
        ImGui::EndPopup();
    }
}

void AssetBrowser::drawViewMenu() {
    if (ImGui::MenuItem(FE_ICON_LAYOUT_GRID "  Icons", nullptr, m_viewMode == ViewMode::Icons))
        m_viewMode = ViewMode::Icons;
    if (ImGui::MenuItem(FE_ICON_LIST "  List", nullptr, m_viewMode == ViewMode::List)) {
        m_viewMode      = ViewMode::List;
        m_syncListSort  = true;
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Icon size", &m_tileSize, 48.0f, 128.0f, "%.0f");

    ImGui::Separator();
    ImGui::TextColored(theme::textDim(), "Sort by");

    const struct { SortKey key; const char* label; } keys[] = {
        {SortKey::Name, "Name"}, {SortKey::Modified, "Date modified"},
        {SortKey::Type, "Type"}, {SortKey::Size,     "Size"}};
    for (const auto& k : keys) {
        if (ImGui::MenuItem(k.label, nullptr, m_sortKey == k.key)) {
            m_sortKey      = k.key;
            m_syncListSort = true;
            rebuildVisible();
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Ascending", nullptr, m_sortAscending)) {
        m_sortAscending = true;
        m_syncListSort  = true;
        rebuildVisible();
    }
    if (ImGui::MenuItem("Descending", nullptr, !m_sortAscending)) {
        m_sortAscending = false;
        m_syncListSort  = true;
        rebuildVisible();
    }
}

void AssetBrowser::drawFolderTree(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::string label = toUtf8(dir.filename());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (dir == m_currentDir)
        flags |= ImGuiTreeNodeFlags_Selected;

    // Check whether this directory has any subdirectories to show the arrow.
    const std::filesystem::directory_iterator end;
    bool hasSubdirs = false;
    for (std::filesystem::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec)) {
        std::error_code typeEc;
        if (it->is_directory(typeEc)) { hasSubdirs = true; break; }
    }
    if (!hasSubdirs) flags |= ImGuiTreeNodeFlags_Leaf;

    // The node itself is drawn without text ("##" hides the label, but the
    // text is still the node's ID, so the full path keeps it unique). The
    // icon and the name are drawn right after it on the same line, which
    // allows the icon to have its own colour.
    const std::string nodeId = "##" + toUtf8(dir);
    const bool open = ImGui::TreeNodeEx(nodeId.c_str(), flags);

    // These refer to the node, so read them before drawing anything else.
    const bool clicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    if (ImGui::IsItemHovered()) m_hoveredFolder = dir;   // a drop from the OS lands here
    acceptMoveDrop(dir);

    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextColored(theme::tintFolder(), "%s",
                       open ? FE_ICON_FOLDER_OPEN : FE_ICON_FOLDER);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(label.c_str());

    // Applied after the grid is drawn, like every navigation.
    if (clicked) m_pendingDir = dir;

    if (open) {
        for (std::filesystem::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec)) {
            std::error_code typeEc;
            if (!it->is_directory(typeEc)) continue;

            const std::string name = toUtf8(it->path().filename());
            if (!name.empty() && name[0] != '.')
                drawFolderTree(it->path());
        }
        ImGui::TreePop();
    }
}

// ---------------------------------------------------------------------------
// Drawing: the items
// ---------------------------------------------------------------------------

void AssetBrowser::drawIcons() {
    const float cellWidth  = m_tileSize + 8.0f;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const int   columns    = std::max(1, static_cast<int>(availWidth / cellWidth));

    if (ImGui::BeginTable("##Grid", columns, ImGuiTableFlags_None)) {
        for (size_t i = 0; i < m_visible.size(); ++i) {
            ImGui::TableNextColumn();
            drawTile(m_entries[m_visible[i]], i);
        }
        ImGui::EndTable();
    }
}

void AssetBrowser::drawTile(const AssetEntry& entry, size_t visibleIndex) {
    const bool      selected = isSelected(entry.path);
    const EntryIcon icon     = iconFor(entry);
    const float     tile     = m_tileSize;

    ImGui::PushID(toUtf8(entry.path).c_str());

    const ImVec2 cursor  = ImGui::GetCursorScreenPos();
    const ImVec2 tileMax = {cursor.x + tile, cursor.y + tile + 8.0f};

    // The tile's background is drawn below with rounded corners, so the
    // Selectable itself is made invisible.
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick,
                      ImVec2(tile, tile + 8.0f));
    ImGui::PopStyleColor(3);

    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("%s", entry.name.c_str());   // names are shortened below
    m_itemRects.push_back({visibleIndex, cursor, tileMax});
    interact(entry, visibleIndex);

    // An item that was cut is shown faded until it is pasted.
    const bool  dimmed = m_clipboardCut &&
        std::find(m_clipboard.begin(), m_clipboard.end(), entry.path) != m_clipboard.end();
    const float fade   = dimmed ? 0.4f : 1.0f;

    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Background: accent when selected, a faint highlight when hovered.
    if (selected) {
        draw->AddRectFilled(cursor, tileMax,
                            ImGui::GetColorU32(theme::accent(0.28f)), 6.0f);
        draw->AddRect(cursor, tileMax,
                      ImGui::GetColorU32(theme::accent(0.80f)), 6.0f);
    } else if (hovered) {
        draw->AddRectFilled(cursor, tileMax, IM_COL32(255, 255, 255, 14), 6.0f);
    }

    // A model shows a picture of itself once its thumbnail is ready; asking
    // for it only while the tile is on screen is what queues it. Everything
    // else (and a model whose picture is not ready) shows its icon.
    VkDescriptorSet picture = VK_NULL_HANDLE;
    if (m_thumbnails && !entry.isDir && isSupportedModel(entry.path) &&
        ImGui::IsRectVisible(cursor, tileMax))
        picture = m_thumbnails->request(entry.path);

    // An image shows itself, shrunk to fit the square the model pictures use.
    float pictureAspect = 1.0f;   // width / height
    if (m_imageThumbnails && !entry.isDir && isLoadableImage(lowerExtension(entry.path)) &&
        ImGui::IsRectVisible(cursor, tileMax)) {
        ImageThumbnails::Picture image;
        if (m_imageThumbnails->request(entry.path, image)) {
            picture       = image.set;
            pictureAspect = static_cast<float>(image.width) / static_cast<float>(image.height);
        }
    }

    if (picture != VK_NULL_HANDLE) {
        const float  side = tile * 0.72f;
        const float  w    = pictureAspect >= 1.0f ? side : side * pictureAspect;
        const float  h    = pictureAspect >= 1.0f ? side / pictureAspect : side;
        const ImVec2 min  = {cursor.x + (tile - w) * 0.5f, cursor.y + tile * 0.055f + (side - h) * 0.5f};
        const ImVec2 max  = {min.x + w, min.y + h};
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(picture), min, max,
                              ImVec2(0, 0), ImVec2(1, 1),
                              ImGui::GetColorU32(ImVec4(1, 1, 1, fade)), 5.0f);
    } else {
        // Icon, centred in the upper part of the tile.
        const float  iconSize  = tile * 0.47f;
        ImFont*      font      = ImGui::GetFont();
        const ImVec2 glyphSize = font->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon.glyph);
        const ImVec2 iconPos   = {cursor.x + (tile - glyphSize.x) * 0.5f,
                                  cursor.y + tile * 0.17f};
        ImVec4 iconTint = icon.tint;
        iconTint.w *= fade;
        draw->AddText(font, iconSize, iconPos,
                      ImGui::GetColorU32(iconTint), icon.glyph);
    }

    // Name, centred under the icon. Bigger tiles have room for more letters.
    const size_t maxBytes = std::max<size_t>(8, static_cast<size_t>(tile / 6.5f));
    const std::string displayName = shorten(entry.name, maxBytes);
    const ImVec2 nameSize = ImGui::CalcTextSize(displayName.c_str());
    const ImVec2 textPos  = {cursor.x + (tile - nameSize.x) * 0.5f,
                             cursor.y + tile * 0.80f};
    draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text, fade), displayName.c_str());

    ImGui::PopID();
}

void AssetBrowser::drawList() {
    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_RowBg    | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##List", 4, flags)) return;

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f,
                            static_cast<ImGuiID>(SortKey::Name));
    ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthFixed, 130.0f,
                            static_cast<ImGuiID>(SortKey::Modified));
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f,
                            static_cast<ImGuiID>(SortKey::Type));
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f,
                            static_cast<ImGuiID>(SortKey::Size));

    // The sort chosen in the View menu (or in the icon view) must show in the
    // header arrows too.
    if (m_syncListSort) {
        ImGui::TableSetColumnSortDirection(static_cast<int>(m_sortKey),
            m_sortAscending ? ImGuiSortDirection_Ascending : ImGuiSortDirection_Descending, false);
        m_syncListSort = false;
    }

    ImGui::TableHeadersRow();

    // Clicking a header changes the order.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            m_sortKey       = static_cast<SortKey>(specs->Specs[0].ColumnUserID);
            m_sortAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            rebuildVisible();
        }
        specs->SpecsDirty = false;
    }

    for (size_t i = 0; i < m_visible.size(); ++i)
        drawRow(m_entries[m_visible[i]], i);

    ImGui::EndTable();
}

void AssetBrowser::drawRow(const AssetEntry& entry, size_t visibleIndex) {
    const bool      selected = isSelected(entry.path);
    const EntryIcon icon     = iconFor(entry);

    ImGui::PushID(toUtf8(entry.path).c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    const float  rowHeight = ImGui::GetFrameHeight();
    const ImVec2 pos       = ImGui::GetCursorScreenPos();
    const float  columnW   = ImGui::GetContentRegionAvail().x;

    // One selectable covers the whole row; the texts are drawn over it.
    ImGui::Selectable("##row", selected,
                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                      ImVec2(0.0f, rowHeight));
    m_itemRects.push_back({visibleIndex, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    interact(entry, visibleIndex);

    const bool  dimmed = m_clipboardCut &&
        std::find(m_clipboard.begin(), m_clipboard.end(), entry.path) != m_clipboard.end();
    const float fade   = dimmed ? 0.4f : 1.0f;

    // Icon and name, kept inside the first column.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(pos, {pos.x + columnW, pos.y + rowHeight}, true);

    const float iconSize = ImGui::GetFontSize();
    ImVec4 tint = icon.tint;
    tint.w *= fade;
    draw->AddText(ImGui::GetFont(), iconSize, {pos.x + 4.0f, pos.y + (rowHeight - iconSize) * 0.5f},
                  ImGui::GetColorU32(tint), icon.glyph);
    draw->AddText({pos.x + iconSize + 12.0f, pos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f},
                  ImGui::GetColorU32(ImGuiCol_Text, fade), entry.name.c_str());
    draw->PopClipRect();

    // The details. Text is placed at the row's top, so nudge it to its middle.
    const float lift = (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::TableSetColumnIndex(1);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
    ImGui::TextColored(theme::textDim(), "%s", entry.modifiedText.c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
    ImGui::TextColored(theme::textDim(), "%s", entry.typeName.c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
    ImGui::TextColored(theme::textDim(), "%s", entry.sizeText.c_str());

    ImGui::PopID();
}

void AssetBrowser::interact(const AssetEntry& entry, size_t visibleIndex) {
    const ImGuiIO& io      = ImGui::GetIO();
    const bool     hovered = ImGui::IsItemHovered();
    if (hovered && entry.isDir) m_hoveredFolder = entry.path;   // a drop from the OS lands here

    // ---- Selecting (on press, like the OS explorer) ----
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (io.KeyShift && !m_anchor.empty()) {
            // A range from the anchor to here, over what is on screen.
            const auto anchor = std::find_if(m_visible.begin(), m_visible.end(),
                [&](size_t i) { return m_entries[i].path == m_anchor; });
            if (anchor != m_visible.end())
                selectRange(static_cast<size_t>(anchor - m_visible.begin()), visibleIndex);
            else
                selectOnly(entry.path);
        } else if (io.KeyCtrl) {
            if (!m_selection.erase(entry.path)) m_selection.insert(entry.path);
            m_anchor = entry.path;
        } else if (isSelected(entry.path)) {
            // Part of a selection: it may be the start of dragging the whole
            // group, so the others stay selected until the button comes up
            // without a drag (see below).
            m_collapseTo = entry.path;
            m_anchor     = entry.path;
        } else {
            selectOnly(entry.path);
            m_anchor = entry.path;
        }
        m_primary = entry.path;
    }

    // Released on the same item without dragging: it becomes the only one.
    if (m_collapseTo == entry.path && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        if (hovered && drag.x * drag.x + drag.y * drag.y < 16.0f)
            selectOnly(entry.path);
        m_collapseTo.clear();
    }

    // Right-click: select what is under the cursor, unless it already belongs
    // to the selection, so that the menu acts on what the user sees.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!isSelected(entry.path)) selectOnly(entry.path);
        m_primary = entry.path;
    }

    // ---- Opening ----
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift) {
        if (entry.isDir)
            // Not now: this runs while the list is being walked, and
            // refresh() rebuilds it. onImGui() applies it afterwards.
            m_pendingDir = entry.path;
        else if (isScene(lowerExtension(entry.path)))
            m_sceneToOpen = entry.path;
        else if (isLoadableImage(lowerExtension(entry.path)))
            m_imageToOpen = entry.path;
        else if (isSupportedModel(entry.path))
            m_modelToOpen = entry.path;
    }

    // ---- Dragging ----
    // Every item can be dragged. What it carries decides where it can be
    // dropped: a model goes into the viewport, an image onto a material's
    // texture slot, and anything onto a folder to move it there. An item has
    // one drag source, so the payload type is chosen here, and the folders
    // accept all the types (see acceptMoveDrop()).
    if (ImGui::BeginDragDropSource()) {
        // Dragging something that is not selected drags only that item.
        if (!isSelected(entry.path)) selectOnly(entry.path);
        m_collapseTo.clear();

        std::string text;
        for (const auto& path : m_selection) {
            if (!text.empty()) text += '\n';
            text += toUtf8(path);
        }

        const std::string ext = lowerExtension(entry.path);
        const char* type = "FE_ASSET";
        if (m_selection.size() == 1 && !entry.isDir) {
            if (isSupportedModel(entry.path)) type = "FE_MODEL";
            else if (isLoadableImage(ext))    type = "FE_IMAGE";
        }

        ImGui::SetDragDropPayload(type, text.c_str(), text.size() + 1);
        if (m_selection.size() > 1)
            ImGui::Text("%zu items", m_selection.size());
        else
            ImGui::TextUnformatted(entry.name.c_str());   // what follows the cursor
        ImGui::EndDragDropSource();
    }

    // A folder is a place to drop things on.
    if (entry.isDir) acceptMoveDrop(entry.path);

    // ---- Right-click menu. It belongs to the last item, so it stays here. ----
    if (ImGui::BeginPopupContextItem("##EntryMenu")) {
        drawEntryMenu(entry);
        ImGui::EndPopup();
    }
}

void AssetBrowser::handleRubberBand() {
    // Clicking or dragging on empty space.
    const ImVec2 mouse  = ImGui::GetIO().MousePos;
    const ImVec2 scroll = {ImGui::GetScrollX(), ImGui::GetScrollY()};

    const bool overEmptySpace = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();

    if (overEmptySpace && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::GetIO().KeyCtrl)
        m_selection.clear();   // the menu that follows is the folder's, not an item's

    if (!m_bandActive) {
        if (overEmptySpace && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_bandActive = true;
            m_bandStart  = {mouse.x + scroll.x, mouse.y + scroll.y};
            m_bandBase   = ImGui::GetIO().KeyCtrl ? m_selection : std::set<std::filesystem::path>{};
            if (!ImGui::GetIO().KeyCtrl) { m_selection.clear(); m_primary.clear(); }
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_bandActive = false;
    } else {
        // The rectangle, in screen coordinates of this frame (the list may
        // have scrolled since the drag began).
        const ImVec2 start = {m_bandStart.x - scroll.x, m_bandStart.y - scroll.y};
        const ImVec2 a = {std::min(start.x, mouse.x), std::min(start.y, mouse.y)};
        const ImVec2 b = {std::max(start.x, mouse.x), std::max(start.y, mouse.y)};

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(a, b, ImGui::GetColorU32(theme::accent(0.18f)));
        draw->AddRect(a, b, ImGui::GetColorU32(theme::accent(0.70f)));

        m_selection = m_bandBase;
        for (const ItemRect& r : m_itemRects)
            if (r.max.x >= a.x && r.min.x <= b.x && r.max.y >= a.y && r.min.y <= b.y)
                m_selection.insert(m_entries[m_visible[r.visible]].path);
    }

    // A press that did not become a click on an item is over once the button is up.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) m_collapseTo.clear();
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void AssetBrowser::requestOp(Op op, std::vector<std::filesystem::path> targets) {
    m_op        = op;
    m_opTargets = std::move(targets);
}

void AssetBrowser::fail(std::string message) {
    m_error           = std::move(message);
    m_openErrorDialog = true;
}

void AssetBrowser::drawEntryMenu(const AssetEntry& entry) {
    // The menu acts on the selection, which includes the item it was opened
    // from (a right-click selects it first).
    const std::vector<std::filesystem::path> targets(m_selection.begin(), m_selection.end());
    const bool single = targets.size() == 1;

    if (single && entry.isDir && ImGui::MenuItem(FE_ICON_FOLDER_OPEN "  Open"))
        m_pendingDir = entry.path;
    if (single && !entry.isDir && isScene(lowerExtension(entry.path)) &&
        ImGui::MenuItem(FE_ICON_CLAPPERBOARD "  Open Scene"))
        m_sceneToOpen = entry.path;
    if (single && !entry.isDir && isLoadableImage(lowerExtension(entry.path)) &&
        ImGui::MenuItem(FE_ICON_EXTERNAL_LINK "  Open in Viewer"))
        m_imageToOpen = entry.path;
    if (single && !entry.isDir && isSupportedModel(entry.path) &&
        ImGui::MenuItem(FE_ICON_EXTERNAL_LINK "  Open in Viewer"))
        m_modelToOpen = entry.path;
    if (single && (entry.isDir || isScene(lowerExtension(entry.path)) ||
                   isLoadableImage(lowerExtension(entry.path)) || isSupportedModel(entry.path)))
        ImGui::Separator();

    if (ImGui::MenuItem(FE_ICON_COPY "  Copy", "Ctrl+C"))
        requestOp(Op::Copy, targets);
    if (ImGui::MenuItem(FE_ICON_SCISSORS "  Cut", "Ctrl+X"))
        requestOp(Op::Cut, targets);

    // Pasting onto a folder puts the items inside it; onto a file, next to it.
    if (ImGui::MenuItem(FE_ICON_CLIPBOARD_PASTE "  Paste", "Ctrl+V", false, !m_clipboard.empty()))
        requestOp(Op::Paste, {single && entry.isDir ? entry.path : m_currentDir});

    ImGui::Separator();
    if (ImGui::MenuItem(FE_ICON_PENCIL "  Rename", "F2", false, single))
        requestOp(Op::Rename, targets);
    if (ImGui::MenuItem(FE_ICON_TRASH_2 "  Delete", "Del"))
        requestOp(Op::Delete, targets);

    ImGui::Separator();
    if (ImGui::MenuItem(FE_ICON_EXTERNAL_LINK "  Show in File Explorer", nullptr, false, single))
        revealInFileManager(entry.path);
}

void AssetBrowser::drawGridMenu() {
    // Opens only on empty space: right-clicking a tile opens its own menu.
    if (!ImGui::BeginPopupContextWindow("##GridMenu",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        return;

    if (ImGui::MenuItem(FE_ICON_FOLDER_PLUS "  New Folder", "Ctrl+Shift+N"))
        requestOp(Op::NewFolder, {m_currentDir});
    if (ImGui::MenuItem(FE_ICON_CLIPBOARD_PASTE "  Paste", "Ctrl+V", false, !m_clipboard.empty()))
        requestOp(Op::Paste, {m_currentDir});

    ImGui::Separator();
    if (ImGui::MenuItem(FE_ICON_REFRESH_CW "  Refresh", "F5"))
        refresh();
    if (ImGui::MenuItem(FE_ICON_EXTERNAL_LINK "  Show in File Explorer"))
        revealInFileManager(m_currentDir);

    ImGui::EndPopup();
}

void AssetBrowser::handleShortcuts() {
    // Only while this panel is the active one, and not while typing or while
    // a dialog is open.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel)) return;

    // Navigation, like the OS explorer.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false))              m_pendingNav = Nav::Back;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_RightArrow)) m_pendingNav = Nav::Forward;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_UpArrow))    m_pendingNav = Nav::Up;

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))                     refresh();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))        m_focusSearch = true;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        m_selection.clear();
        for (size_t i : m_visible) m_selection.insert(m_entries[i].path);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_selection.clear();

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) {
        requestOp(Op::NewFolder, {m_currentDir});
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        requestOp(Op::Paste, {m_currentDir});
        return;
    }

    if (m_selection.empty()) return;
    const std::vector<std::filesystem::path> targets(m_selection.begin(), m_selection.end());

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))      requestOp(Op::Copy,   targets);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) requestOp(Op::Cut,    targets);
    else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && targets.size() == 1)
                                                                   requestOp(Op::Rename, targets);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))          requestOp(Op::Delete, targets);
    else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && targets.size() == 1) {
        // Enter opens, like a double click.
        std::error_code ec;
        if (std::filesystem::is_directory(targets[0], ec))   m_pendingDir  = targets[0];
        else if (isScene(lowerExtension(targets[0])))        m_sceneToOpen = targets[0];
        else if (isLoadableImage(lowerExtension(targets[0]))) m_imageToOpen = targets[0];
        else if (isSupportedModel(targets[0]))               m_modelToOpen = targets[0];
    }
}

void AssetBrowser::applyPendingOp() {
    const Op op = m_op;
    m_op = Op::None;
    if (op == Op::None) return;

    std::vector<std::filesystem::path> targets = std::move(m_opTargets);
    m_opTargets.clear();
    if (targets.empty()) return;

    switch (op) {
        case Op::Copy:
        case Op::Cut:
            m_clipboard    = std::move(targets);
            m_clipboardCut = (op == Op::Cut);
            break;

        case Op::Paste:
            paste(targets[0]);
            break;

        case Op::Rename: {
            m_dialogTarget          = targets[0];
            m_nameDialogIsNewFolder = false;
            const std::string name  = toUtf8(targets[0].filename());
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", name.c_str());
            m_openNameDialog = true;
            m_focusNameField = true;
            break;
        }

        case Op::NewFolder:
            m_dialogTarget          = targets[0];   // the folder to create it in
            m_nameDialogIsNewFolder = true;
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "New Folder");
            m_openNameDialog = true;
            m_focusNameField = true;
            break;

        case Op::Delete:
            m_deleteTargets    = std::move(targets);
            m_openDeleteDialog = true;
            break;

        case Op::None:
            break;
    }
}

void AssetBrowser::paste(const std::filesystem::path& destDir) {
    if (m_clipboard.empty()) return;

    std::vector<std::filesystem::path> created;
    std::string problems;

    for (const std::filesystem::path& source : m_clipboard) {
        std::error_code ec;
        const std::string name = toUtf8(source.filename());

        if (!std::filesystem::exists(source, ec)) {
            problems += "'" + name + "' no longer exists.\n";
            continue;
        }

        const bool isDir = std::filesystem::is_directory(source, ec);
        if (isDir && isInsideOrSame(destDir, source)) {
            problems += "'" + name + "' cannot be pasted into itself.\n";
            continue;
        }

        if (m_clipboardCut) {
            // Cutting and pasting into the same folder changes nothing.
            if (source.parent_path() == destDir) continue;

            const std::filesystem::path dest = uniquePath(destDir, source.filename(), isDir);
            moveOnDisk(source, dest, ec);
            if (ec) problems += "'" + name + "': " + ec.message() + "\n";
            else    created.push_back(dest);
        } else {
            const std::filesystem::path dest = uniquePath(destDir, source.filename(), isDir);
            std::filesystem::copy(source, dest, std::filesystem::copy_options::recursive, ec);
            if (ec) problems += "'" + name + "': " + ec.message() + "\n";
            else {
                created.push_back(dest);
                AssetDatabase::instance().scan(dest);   // the copy is a new asset: new id
            }
        }
    }

    // A cut is done once: pasting again would have nothing left to move.
    if (m_clipboardCut) {
        m_clipboard.clear();
        m_clipboardCut = false;
    }

    refresh();

    // Show what arrived, if it arrived in the folder being shown.
    if (!created.empty() && destDir == m_currentDir) {
        m_selection.clear();
        for (const auto& path : created) m_selection.insert(path);
        m_primary = created.back();
    }

    if (!problems.empty()) fail("Could not paste some items:\n" + problems);
}

void AssetBrowser::acceptMoveDrop(const std::filesystem::path& destDir) {
    if (!ImGui::BeginDragDropTarget()) return;

    // The items may travel as any of the three payload types (see interact()).
    for (const char* type : {"FE_ASSET", "FE_MODEL", "FE_IMAGE"}) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
            // Not moved right now: the grid is being drawn, and moving
            // rebuilds the very list it is walking. onImGui() applies it.
            m_moveSources = pathsFromPayload(*payload);
            m_moveDest    = destDir;
            break;
        }
    }
    ImGui::EndDragDropTarget();
}

void AssetBrowser::moveItems(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& destDir) {
    std::string problems;
    bool changed = false;

    for (const std::filesystem::path& source : sources) {
        std::error_code ec;
        const std::string name = toUtf8(source.filename());

        if (!std::filesystem::exists(source, ec)) {
            problems += "'" + name + "' no longer exists.\n";
            continue;
        }

        // Dropped on itself, or on the folder it is already in: nothing to do.
        if (source == destDir || source.parent_path() == destDir) continue;

        const bool isDir = std::filesystem::is_directory(source, ec);
        if (isDir && isInsideOrSame(destDir, source)) {
            problems += "'" + name + "' cannot be moved into itself.\n";
            continue;
        }

        const std::filesystem::path dest = uniquePath(destDir, source.filename(), isDir);
        moveOnDisk(source, dest, ec);
        if (ec) {
            problems += "'" + name + "': " + ec.message() + "\n";
            continue;
        }
        changed = true;

        // Keep what pointed at the old location pointing at something real.
        m_selection.erase(source);
        for (auto it = m_clipboard.begin(); it != m_clipboard.end();)
            it = isInsideOrSame(*it, source) ? m_clipboard.erase(it) : std::next(it);
        if (isDir && isInsideOrSame(m_currentDir, source)) m_currentDir = destDir;
    }

    if (changed || !problems.empty()) refresh();
    if (!problems.empty()) fail("Could not move some items:\n" + problems);
}

void AssetBrowser::applyDrop() {
    if (m_dropped.empty()) return;

    std::vector<std::filesystem::path> files;
    files.swap(m_dropped);

    // Only a drop on this panel is ours. Anywhere else the files are ignored,
    // so dropping on the viewport does not fill the project with copies.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        FE_INFO("Dropped files ignored: drop them on the Asset Browser");
        return;
    }

    const std::filesystem::path destDir = m_hoveredFolder.empty() ? m_currentDir
                                                                  : m_hoveredFolder;
    std::string problems;
    std::vector<std::filesystem::path> created;

    for (const std::filesystem::path& source : files) {
        std::error_code ec;
        if (!std::filesystem::exists(source, ec)) continue;

        const bool isDir = std::filesystem::is_directory(source, ec);
        const std::string name = toUtf8(source.filename());

        // Copying a folder into itself (or below itself) would never end.
        if (isDir && isInsideOrSame(destDir, source)) {
            problems += "'" + name + "' cannot be copied into itself.\n";
            continue;
        }

        const std::filesystem::path dest = uniquePath(destDir, source.filename(), isDir);
        std::filesystem::copy(source, dest, std::filesystem::copy_options::recursive, ec);
        if (ec) problems += "'" + name + "': " + ec.message() + "\n";
        else {
            created.push_back(dest);
            AssetDatabase::instance().scan(dest);   // new asset(s): new id(s)
        }
    }

    if (!created.empty()) {
        FE_INFO("AssetBrowser: copied %zu item(s) into '%s'", created.size(), toUtf8(destDir).c_str());
        refresh();
        if (destDir == m_currentDir) {
            m_selection.clear();
            for (const auto& path : created) m_selection.insert(path);
            m_primary = created.back();
        }
    }
    if (!problems.empty()) fail("Could not copy some items:\n" + problems);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

void AssetBrowser::drawDialogs() {
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();

    // ---- Rename / New Folder ----
    if (m_openNameDialog) {
        ImGui::OpenPopup("###AssetNameDialog");
        m_openNameDialog = false;
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const char* nameTitle = m_nameDialogIsNewFolder ? "New Folder###AssetNameDialog"
                                                    : "Rename###AssetNameDialog";
    if (ImGui::BeginPopupModal(nameTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_focusNameField) {
            ImGui::SetKeyboardFocusHere();
            m_focusNameField = false;
        }
        ImGui::SetNextItemWidth(300.0f);
        const bool enter = ImGui::InputText("##Name", m_nameBuf, sizeof(m_nameBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_AutoSelectAll);

        const std::string name = m_nameBuf;
        const std::filesystem::path dest = m_nameDialogIsNewFolder
            ? m_dialogTarget / pathFromUtf8(m_nameBuf)
            : m_dialogTarget.parent_path() / pathFromUtf8(m_nameBuf);

        std::error_code ec;
        const bool valid = isValidName(name);
        const bool taken = valid && dest != m_dialogTarget &&
                           std::filesystem::exists(dest, ec);

        if (!name.empty() && !valid)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                               "A name cannot contain  < > : \" / \\ | ? *");
        else if (taken)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                               "Something with that name already exists.");

        ImGui::BeginDisabled(!valid || taken);
        const bool ok = ImGui::Button("OK", ImVec2(100, 0)) || (enter && valid && !taken);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(100, 0)) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (ok) {
            if (m_nameDialogIsNewFolder) {
                std::filesystem::create_directory(dest, ec);
            } else if (dest != m_dialogTarget) {
                std::filesystem::rename(m_dialogTarget, dest, ec);
                if (!ec) {
                    AssetDatabase::instance().moved(m_dialogTarget, dest);
                    for (auto& item : m_clipboard)
                        if (item == m_dialogTarget) item = dest;
                }
            }
            if (ec) {
                fail("Could not complete the operation:\n" + ec.message());
                refresh();
            } else {
                refresh();
                // The new or renamed item is the one the user is looking for.
                const std::filesystem::path shownIn = m_nameDialogIsNewFolder ? m_dialogTarget
                                                                              : m_dialogTarget.parent_path();
                if (shownIn == m_currentDir) {
                    selectOnly(dest);
                    m_anchor = dest;
                }
            }
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Delete ----
    if (m_openDeleteDialog) {
        ImGui::OpenPopup("Delete###AssetDeleteDialog");
        m_openDeleteDialog = false;
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete###AssetDeleteDialog", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        std::error_code ec;
        bool anyFolder = false;
        for (const auto& path : m_deleteTargets)
            anyFolder = anyFolder || std::filesystem::is_directory(path, ec);

        if (m_deleteTargets.size() == 1)
            ImGui::Text("Delete \"%s\"?", toUtf8(m_deleteTargets[0].filename()).c_str());
        else
            ImGui::Text("Delete these %zu items?", m_deleteTargets.size());
        if (anyFolder) ImGui::TextUnformatted("Folders and everything inside them will be deleted.");
        ImGui::TextColored(theme::textDim(), "This cannot be undone.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.32f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.20f, 0.20f, 1.0f));
        const bool confirmed = ImGui::Button(FE_ICON_TRASH_2 "  Delete", ImVec2(110, 0));
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(110, 0)) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (confirmed) {
            std::string problems;
            for (const auto& path : m_deleteTargets) {
                std::filesystem::remove_all(path, ec);
                if (ec) problems += "'" + toUtf8(path.filename()) + "': " + ec.message() + "\n";
                else    AssetDatabase::instance().removed(path);

                // Forget anything that pointed at what was deleted.
                m_selection.erase(path);
                for (auto it = m_clipboard.begin(); it != m_clipboard.end();)
                    it = isInsideOrSame(*it, path) ? m_clipboard.erase(it) : std::next(it);
            }
            if (m_clipboard.empty()) m_clipboardCut = false;
            m_deleteTargets.clear();

            refresh();
            if (!problems.empty()) fail("Could not delete some items:\n" + problems);
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            m_deleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Error ----
    if (m_openErrorDialog) {
        ImGui::OpenPopup("Error###AssetErrorDialog");
        m_openErrorDialog = false;
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Error###AssetErrorDialog", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(m_error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

void AssetBrowser::onImGui() {
    ImGui::Begin("Asset Browser");

    m_hoveredFolder.clear();   // set again by the tree and the items below
    refreshIfChangedOnDisk();

    drawPathBar();
    drawToolbar();
    ImGui::Separator();

    handleShortcuts();

    // The side buttons of the mouse: back and forward, as in a web browser.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        if (ImGui::IsMouseClicked(3)) m_pendingNav = Nav::Back;
        if (ImGui::IsMouseClicked(4)) m_pendingNav = Nav::Forward;
    }

    // ---- Two-column layout ----
    // Room left below for the status bar: the columns would otherwise take
    // all the height there is and push it out of the window.
    const float footerHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

    if (ImGui::BeginTable("##AssetLayout", 2,
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Files",   ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();

        // Left: folder tree.
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##FolderTree", ImVec2(0, ImGui::GetContentRegionAvail().y - footerHeight), false);
        drawFolderTree(m_root);
        ImGui::EndChild();

        // Right: the items.
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##FileGrid", ImVec2(0, ImGui::GetContentRegionAvail().y - footerHeight), false);

        m_itemRects.clear();
        if (m_viewMode == ViewMode::List) drawList();
        else                              drawIcons();

        if (m_visible.empty())
            ImGui::TextDisabled(m_filter[0] ? "Nothing matches your search." : "(empty folder)");

        handleRubberBand();
        drawGridMenu();

        // Apply what the items asked for now that the loop is over. All of
        // these rebuild m_entries, which the loop above was walking.
        applyPendingOp();

        if (!m_moveSources.empty()) {
            const std::vector<std::filesystem::path> sources = std::move(m_moveSources);
            const std::filesystem::path dest = std::move(m_moveDest);
            m_moveSources.clear();
            m_moveDest.clear();
            moveItems(sources, dest);
        }

        applyPendingNavigation();

        ImGui::EndChild();

        ImGui::EndTable();
    }

    applyDrop();
    drawDialogs();

    // ---- Status bar ----
    ImGui::Separator();
    if (m_selection.size() == 1) {
        std::error_code ec;
        const std::filesystem::path shown =
            std::filesystem::relative(*m_selection.begin(), m_root, ec);
        ImGui::TextDisabled("%s", toUtf8(ec ? *m_selection.begin() : shown).c_str());
    } else if (m_selection.size() > 1) {
        ImGui::TextDisabled("%zu of %zu items selected", m_selection.size(), m_visible.size());
    } else if (m_filter[0]) {
        ImGui::TextDisabled("%zu of %zu items", m_visible.size(), m_entries.size());
    } else {
        ImGui::TextDisabled("%zu items", m_entries.size());
    }

    ImGui::End();
}

} // namespace fe
