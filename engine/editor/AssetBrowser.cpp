#include "editor/AssetBrowser.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "import/ModelImporter.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace fe {
namespace {

/// @return The extension of @p path in lower case, including the dot.
std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = toUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// Returns true if @p ext is a recognised image format.
bool isTexture(const std::string& ext) {
    return ext == ".png"  || ext == ".jpg" || ext == ".jpeg"
        || ext == ".hdr"  || ext == ".tga" || ext == ".dds";
}

/// Returns true if @p ext is a recognised scene file.
bool isScene(const std::string& ext) {
    return ext == ".fescene";
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

} // namespace

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
        return;
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

        std::error_code typeEc;

        AssetEntry ae;
        ae.path  = it->path();
        ae.name  = filename;
        ae.isDir = it->is_directory(typeEc);
        m_entries.push_back(std::move(ae));
    }

    // Directories first, then files, both alphabetically.
    std::sort(m_entries.begin(), m_entries.end(),
        [](const AssetEntry& a, const AssetEntry& b) {
            if (a.isDir != b.isDir) return a.isDir > b.isDir;
            return a.name < b.name;
        });
}

const char* AssetBrowser::iconFor(const AssetEntry& entry) {
    if (entry.isDir) return "📁";
    if (isSupportedModel(entry.path)) return "🗿";

    const std::string ext = lowerExtension(entry.path);
    if (isTexture(ext)) return "🖼";
    if (isScene(ext))   return "🎬";
    return "📄";
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

    bool open = ImGui::TreeNodeEx(label.c_str(), flags, "📁 %s", label.c_str());

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_currentDir = dir;
        m_selected   = std::filesystem::path{};
        refresh();
    }

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

void AssetBrowser::drawEntry(const AssetEntry& entry) {
    const bool isSelected = (entry.path == m_selected);
    const char* icon      = iconFor(entry);

    // Each entry is a selectable square tile.
    ImGui::PushID(toUtf8(entry.path).c_str());

    const float tileSize = 72.0f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    if (ImGui::Selectable("##tile", isSelected,
                          ImGuiSelectableFlags_None,
                          ImVec2(tileSize, tileSize + 20.0f))) {
        if (entry.isDir)
            m_pendingDir = entry.path;   // navigate after drawing, see onImGui
        else
            m_selected = entry.path;
    }

    // Models can be dragged into the viewport to add them to the scene.
    // The drag source belongs to the last item drawn, so this must stay
    // right after the Selectable.
    if (!entry.isDir && isSupportedModel(entry.path) &&
        ImGui::BeginDragDropSource()) {
        // Sent as UTF-8 so the path survives any character in it.
        const std::u8string path = entry.path.u8string();
        ImGui::SetDragDropPayload("FE_MODEL", path.c_str(), path.size() + 1);
        ImGui::TextUnformatted(entry.name.c_str());   // what follows the cursor
        ImGui::EndDragDropSource();
    }

    // Draw icon and label on top of the selectable.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 iconPos = {cursor.x + tileSize * 0.5f - 12.0f, cursor.y + 10.0f};
    draw->AddText(ImGui::GetFont(), 28.0f, iconPos,
                  IM_COL32(220, 220, 220, 255), icon);

    const std::string displayName = shorten(entry.name, 10);
    const ImVec2 textPos = {cursor.x + 4.0f, cursor.y + tileSize - 4.0f};
    draw->AddText(textPos, IM_COL32(200, 200, 200, 255), displayName.c_str());

    ImGui::PopID();
}

void AssetBrowser::onImGui() {
    ImGui::Begin("Asset Browser");

    // ---- Breadcrumb bar ----
    // Show the path relative to root and allow clicking any segment.
    {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(m_currentDir, m_root, ec);

        if (ImGui::SmallButton("assets")) {
            m_currentDir = m_root;
            m_selected   = std::filesystem::path{};
            refresh();
        }

        if (!ec) {
            std::filesystem::path accumulated = m_root;
            for (const auto& part : rel) {
                if (part == ".") continue;
                accumulated /= part;
                ImGui::SameLine(0, 2);
                ImGui::TextUnformatted("/");
                ImGui::SameLine(0, 2);
                const std::string partText = toUtf8(part);
                if (ImGui::SmallButton(partText.c_str())) {
                    m_currentDir = accumulated;
                    m_selected   = std::filesystem::path{};
                    refresh();
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("↺")) refresh();
    }

    ImGui::Separator();

    // ---- Two-column layout ----
    if (ImGui::BeginTable("##AssetLayout", 2,
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Files",   ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();

        // Left: folder tree.
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##FolderTree", ImVec2(0, 0), false);
        drawFolderTree(m_root);
        ImGui::EndChild();

        // Right: file grid.
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##FileGrid", ImVec2(0, 0), false);

        const float tileSize    = 72.0f;
        const float tilePadding = 8.0f;
        const float cellWidth   = tileSize + tilePadding;
        const float availWidth  = ImGui::GetContentRegionAvail().x;
        int columns = std::max(1, static_cast<int>(availWidth / cellWidth));

        if (ImGui::BeginTable("##Grid", columns, ImGuiTableFlags_None)) {
            for (const auto& entry : m_entries) {
                ImGui::TableNextColumn();
                drawEntry(entry);
            }
            ImGui::EndTable();
        }

        if (m_entries.empty()) {
            ImGui::TextDisabled("(empty folder)");
        }

        ImGui::EndChild();

        ImGui::EndTable();
    }

    // ---- Status bar ----
    ImGui::Separator();
    if (!m_selected.empty()) {
        std::error_code ec;
        const std::filesystem::path shown =
            std::filesystem::relative(m_selected, m_root, ec);
        ImGui::TextDisabled("%s", toUtf8(ec ? m_selected : shown).c_str());
    } else {
        ImGui::TextDisabled("%zu items", m_entries.size());
    }

    // Apply navigation requested while drawing. By now no reference into
    // m_entries is alive, so rebuilding the list is safe.
    if (!m_pendingDir.empty()) {
        m_currentDir = std::move(m_pendingDir);
        m_pendingDir = std::filesystem::path{};
        m_selected   = std::filesystem::path{};
        refresh();
    }

    ImGui::End();
}

} // namespace fe
