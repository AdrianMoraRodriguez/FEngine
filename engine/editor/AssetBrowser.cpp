#include "editor/AssetBrowser.h"
#include "editor/Panel.h"
#include "core/Log.h"

#include <imgui.h>
#include <algorithm>

namespace fe {
namespace {

/// Returns true if @p ext is a recognised mesh format.
bool isMesh(const std::string& ext) {
    return ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".fbx";
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
                m_currentDir.string().c_str());
        m_currentDir = m_root;
        return;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(m_currentDir, ec)) {
        if (ec) break;

        // Skip hidden files (starting with '.') like .gitkeep.
        const std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename[0] == '.') continue;

        AssetEntry ae;
        ae.path  = entry.path();
        ae.name  = filename;
        ae.isDir = entry.is_directory(ec);
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

    const std::string ext = entry.path.extension().string();
    if (isMesh(ext))    return "🗿";
    if (isTexture(ext)) return "🖼";
    if (isScene(ext))   return "🎬";
    return "📄";
}

void AssetBrowser::drawFolderTree(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::string label = dir.filename().string();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (dir == m_currentDir)
        flags |= ImGuiTreeNodeFlags_Selected;

    // Check whether this directory has any subdirectories to show the arrow.
    bool hasSubdirs = false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) { hasSubdirs = true; break; }
    }
    if (!hasSubdirs) flags |= ImGuiTreeNodeFlags_Leaf;

    bool open = ImGui::TreeNodeEx(label.c_str(), flags, "📁 %s", label.c_str());

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_currentDir = dir;
        m_selected   = std::filesystem::path{};
        refresh();
    }

    if (open) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.is_directory(ec)) {
                const std::string name = e.path().filename().string();
                if (!name.empty() && name[0] != '.')
                    drawFolderTree(e.path());
            }
        }
        ImGui::TreePop();
    }
}

void AssetBrowser::drawEntry(const AssetEntry& entry) {
    const bool isSelected = (entry.path == m_selected);
    const char* icon      = iconFor(entry);

    // Each entry is a selectable square tile.
    ImGui::PushID(entry.path.string().c_str());

    const float tileSize = 72.0f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    if (ImGui::Selectable("##tile", isSelected,
                          ImGuiSelectableFlags_None,
                          ImVec2(tileSize, tileSize + 20.0f))) {
        if (entry.isDir) {
            m_currentDir = entry.path;
            m_selected   = std::filesystem::path{};
            refresh();
        } else {
            m_selected = entry.path;
        }
    }

    // Draw icon and label on top of the selectable.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 iconPos = {cursor.x + tileSize * 0.5f - 12.0f, cursor.y + 10.0f};
    draw->AddText(ImGui::GetFont(), 28.0f, iconPos,
                  IM_COL32(220, 220, 220, 255), icon);

    // Truncate long names with ellipsis.
    std::string displayName = entry.name;
    if (displayName.size() > 10)
        displayName = displayName.substr(0, 9) + "…";

    const ImVec2 textPos = {cursor.x + 4.0f, cursor.y + tileSize - 4.0f};
    draw->AddText(textPos, IM_COL32(200, 200, 200, 255), displayName.c_str());

    ImGui::PopID();
}

void AssetBrowser::onImGui() {
    ImGui::Begin("Asset Browser");

    // ---- Breadcrumb bar ----
    // Show the path relative to root and allow clicking any segment.
    {
        std::filesystem::path rel = std::filesystem::relative(m_currentDir, m_root);
        std::string crumb = "assets";
        if (ImGui::SmallButton(crumb.c_str())) {
            m_currentDir = m_root;
            m_selected   = std::filesystem::path{};
            refresh();
        }

        std::filesystem::path accumulated = m_root;
        for (const auto& part : rel) {
            if (part == ".") continue;
            accumulated /= part;
            ImGui::SameLine(0, 2);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0, 2);
            const std::string partStr = part.string();
            if (ImGui::SmallButton(partStr.c_str())) {
                m_currentDir = accumulated;
                m_selected   = std::filesystem::path{};
                refresh();
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
        ImGui::TextDisabled("%s",
            std::filesystem::relative(m_selected, m_root).string().c_str());
    } else {
        ImGui::TextDisabled("%zu items", m_entries.size());
    }

    ImGui::End();
}

} // namespace fe
