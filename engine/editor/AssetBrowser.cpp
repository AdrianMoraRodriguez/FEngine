#include "editor/AssetBrowser.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "editor/EditorTheme.h"
#include "editor/Icons.h"
#include "gfx/ThumbnailRenderer.h"
#include "import/ModelImporter.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <string>
#include <string_view>
#include <system_error>
#include <cstdio>

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

/// Icon glyph and colour that represent one entry in the browser.
struct EntryIcon {
    const char* glyph;   ///< UTF-8 icon from Icons.h.
    ImVec4      tint;    ///< Colour the glyph is drawn with.
};

/// Returns true if @p ext is a shader file.
bool isCode(const std::string& ext) {
    return ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl"
        || ext == ".spv";
}

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

/// Draws a button that looks like plain text until hovered, for the
/// breadcrumb bar.
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

    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextColored(theme::tintFolder(), "%s",
                       open ? FE_ICON_FOLDER_OPEN : FE_ICON_FOLDER);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(label.c_str());

    if (clicked) {
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
    const bool      isSelected = (entry.path == m_selected);
    const EntryIcon icon       = iconFor(entry);

    // Each entry is a selectable square tile.
    ImGui::PushID(toUtf8(entry.path).c_str());

    const float tileSize = 72.0f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // The tile's background is drawn below with rounded corners, so the
    // Selectable itself is made invisible.
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Selectable("##tile", isSelected,
                                           ImGuiSelectableFlags_None,
                                           ImVec2(tileSize, tileSize + 8.0f));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();

    ImGui::SetItemTooltip("%s", entry.name.c_str());   // names are shortened below

    // Right click selects a file, so the menu and the highlight agree.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !entry.isDir)
        m_selected = entry.path;

    // Double click on a scene file asks the owner to open it.
    if (hovered && !entry.isDir && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        isScene(lowerExtension(entry.path)))
        m_sceneToOpen = entry.path;

    if (pressed) {
        if (entry.isDir) {
            // Do not navigate yet: this function is called while onImGui()
            // loops over m_entries, and refresh() rebuilds that very vector.
            // `entry` would then refer to a destroyed element for the rest
            // of this function. onImGui() applies the move after the loop.
            m_pendingDir = entry.path;
        } else {
            m_selected = entry.path;
        }
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

    // Right-click menu. It belongs to the last item, so it stays here.
    if (ImGui::BeginPopupContextItem("##EntryMenu")) {
        drawEntryMenu(entry);
        ImGui::EndPopup();
    }

    // An item that was cut is shown faded until it is pasted.
    const bool  dimmed = m_clipboardCut && entry.path == m_clipboard;
    const float fade   = dimmed ? 0.4f : 1.0f;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 tileMax = {cursor.x + tileSize, cursor.y + tileSize + 8.0f};

    // Background: accent when selected, a faint highlight when hovered.
    if (isSelected) {
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

    if (picture != VK_NULL_HANDLE) {
        const float side = 52.0f;
        const ImVec2 min = {cursor.x + (tileSize - side) * 0.5f, cursor.y + 4.0f};
        const ImVec2 max = {min.x + side, min.y + side};
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(picture), min, max,
                              ImVec2(0, 0), ImVec2(1, 1),
                              ImGui::GetColorU32(ImVec4(1, 1, 1, fade)), 5.0f);
    } else {
        // Icon, centred in the upper part of the tile.
        const float  iconSize = 34.0f;
        ImFont*      font     = ImGui::GetFont();
        const ImVec2 glyphSize = font->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon.glyph);
        const ImVec2 iconPos  = {cursor.x + (tileSize - glyphSize.x) * 0.5f,
                                 cursor.y + 12.0f};
        ImVec4 iconTint = icon.tint;
        iconTint.w *= fade;
        draw->AddText(font, iconSize, iconPos,
                      ImGui::GetColorU32(iconTint), icon.glyph);
    }

    // Name, centred under the icon.
    const std::string displayName = shorten(entry.name, 11);
    const ImVec2 nameSize = ImGui::CalcTextSize(displayName.c_str());
    const ImVec2 textPos  = {cursor.x + (tileSize - nameSize.x) * 0.5f,
                             cursor.y + 58.0f};
    draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text, fade), displayName.c_str());

    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void AssetBrowser::requestOp(Op op, std::filesystem::path target) {
    m_op       = op;
    m_opTarget = std::move(target);
}

void AssetBrowser::fail(std::string message) {
    m_error           = std::move(message);
    m_openErrorDialog = true;
}

void AssetBrowser::drawEntryMenu(const AssetEntry& entry) {
    // Folders cannot be selected (clicking one opens it), so the menu acts on
    // the entry it was opened from, not on the selection.
    if (entry.isDir && ImGui::MenuItem(FE_ICON_FOLDER_OPEN "  Open"))
        m_pendingDir = entry.path;
    if (entry.isDir) ImGui::Separator();

    if (ImGui::MenuItem(FE_ICON_COPY "  Copy", "Ctrl+C"))
        requestOp(Op::Copy, entry.path);
    if (ImGui::MenuItem(FE_ICON_SCISSORS "  Cut", "Ctrl+X"))
        requestOp(Op::Cut, entry.path);

    // Pasting onto a folder puts the item inside it; onto a file, next to it.
    if (ImGui::MenuItem(FE_ICON_CLIPBOARD_PASTE "  Paste", "Ctrl+V", false,
                        !m_clipboard.empty()))
        requestOp(Op::Paste, entry.isDir ? entry.path : m_currentDir);

    ImGui::Separator();
    if (ImGui::MenuItem(FE_ICON_PENCIL "  Rename", "F2"))
        requestOp(Op::Rename, entry.path);
    if (ImGui::MenuItem(FE_ICON_TRASH_2 "  Delete", "Del"))
        requestOp(Op::Delete, entry.path);
}

void AssetBrowser::drawGridMenu() {
    // Opens only on empty space: right-clicking a tile opens its own menu.
    if (!ImGui::BeginPopupContextWindow("##GridMenu",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        return;

    if (ImGui::MenuItem(FE_ICON_FOLDER_PLUS "  New Folder"))
        requestOp(Op::NewFolder, m_currentDir);
    if (ImGui::MenuItem(FE_ICON_CLIPBOARD_PASTE "  Paste", "Ctrl+V", false,
                        !m_clipboard.empty()))
        requestOp(Op::Paste, m_currentDir);

    ImGui::EndPopup();
}

void AssetBrowser::handleShortcuts() {
    // Only while this panel is the active one, and not while typing or while
    // a dialog is open.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel)) return;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        requestOp(Op::Paste, m_currentDir);
        return;
    }

    if (m_selected.empty()) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))   requestOp(Op::Copy,   m_selected);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) requestOp(Op::Cut, m_selected);
    else if (ImGui::IsKeyPressed(ImGuiKey_F2, false))           requestOp(Op::Rename, m_selected);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))       requestOp(Op::Delete, m_selected);
}

void AssetBrowser::applyPendingOp() {
    const Op op = m_op;
    m_op = Op::None;
    if (op == Op::None) return;

    const std::filesystem::path target = std::move(m_opTarget);
    m_opTarget.clear();

    switch (op) {
        case Op::Copy:
        case Op::Cut:
            m_clipboard    = target;
            m_clipboardCut = (op == Op::Cut);
            break;

        case Op::Paste:
            paste(target);
            break;

        case Op::Rename: {
            m_dialogTarget          = target;
            m_nameDialogIsNewFolder = false;
            const std::string name  = toUtf8(target.filename());
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", name.c_str());
            m_openNameDialog = true;
            m_focusNameField = true;
            break;
        }

        case Op::NewFolder:
            m_dialogTarget          = target;   // the folder to create it in
            m_nameDialogIsNewFolder = true;
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "New Folder");
            m_openNameDialog = true;
            m_focusNameField = true;
            break;

        case Op::Delete:
            m_dialogTarget     = target;
            m_openDeleteDialog = true;
            break;

        case Op::None:
            break;
    }
}

void AssetBrowser::paste(const std::filesystem::path& destDir) {
    if (m_clipboard.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(m_clipboard, ec)) {
        m_clipboard.clear();
        fail("The item no longer exists.");
        return;
    }

    const bool isDir = std::filesystem::is_directory(m_clipboard, ec);
    if (isDir && isInsideOrSame(destDir, m_clipboard)) {
        fail("A folder cannot be pasted into itself.");
        return;
    }

    const std::filesystem::path name = m_clipboard.filename();

    if (m_clipboardCut) {
        // Cutting and pasting into the same folder changes nothing.
        if (m_clipboard.parent_path() == destDir) {
            m_clipboard.clear();
            m_clipboardCut = false;
            return;
        }

        const std::filesystem::path dest = uniquePath(destDir, name, isDir);
        std::filesystem::rename(m_clipboard, dest, ec);
        if (ec) {
            // A rename cannot cross drives: copy, then remove the original.
            ec.clear();
            std::filesystem::copy(m_clipboard, dest,
                                  std::filesystem::copy_options::recursive, ec);
            if (!ec) std::filesystem::remove_all(m_clipboard, ec);
        }
        if (ec) {
            fail("Could not move the item:\n" + ec.message());
        } else {
            if (m_selected == m_clipboard) m_selected = dest;
            m_clipboard.clear();
            m_clipboardCut = false;
        }
    } else {
        const std::filesystem::path dest = uniquePath(destDir, name, isDir);
        std::filesystem::copy(m_clipboard, dest,
                              std::filesystem::copy_options::recursive, ec);
        if (ec) fail("Could not copy the item:\n" + ec.message());
        else if (!isDir) m_selected = dest;
    }

    refresh();
}

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
                    if (m_selected  == m_dialogTarget) m_selected  = dest;
                    if (m_clipboard == m_dialogTarget) m_clipboard = dest;
                }
            }
            if (ec) fail("Could not complete the operation:\n" + ec.message());
            refresh();
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
        const bool isDir = std::filesystem::is_directory(m_dialogTarget, ec);

        ImGui::Text("Delete \"%s\"?", toUtf8(m_dialogTarget.filename()).c_str());
        if (isDir) ImGui::TextUnformatted("The folder and everything inside it will be deleted.");
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
            std::filesystem::remove_all(m_dialogTarget, ec);
            if (ec) fail("Could not delete the item:\n" + ec.message());

            // Forget anything that pointed at what was deleted.
            if (!m_selected.empty()  && isInsideOrSame(m_selected,  m_dialogTarget)) m_selected.clear();
            if (!m_clipboard.empty() && isInsideOrSame(m_clipboard, m_dialogTarget)) {
                m_clipboard.clear();
                m_clipboardCut = false;
            }
            refresh();
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
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

void AssetBrowser::onImGui() {
    ImGui::Begin("Asset Browser");

    // ---- Breadcrumb bar ----
    // Show the path relative to root and allow clicking any segment.
    {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(m_currentDir, m_root, ec);

        if (flatButton(FE_ICON_HOUSE " assets")) {
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
                ImGui::TextColored(theme::textDim(), FE_ICON_CHEVRON_RIGHT);
                ImGui::SameLine(0, 2);
                const std::string partText = toUtf8(part);
                if (flatButton(partText.c_str())) {
                    m_currentDir = accumulated;
                    m_selected   = std::filesystem::path{};
                    refresh();
                }
            }
        }

        ImGui::SameLine();
        if (flatButton(FE_ICON_REFRESH_CW)) refresh();
        ImGui::SetItemTooltip("Refresh");
    }

    ImGui::Separator();

    handleShortcuts();

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

        drawGridMenu();

        // Apply what the grid asked for now that the loop is over. Both
        // rebuild m_entries, which the loop above was walking.
        applyPendingOp();

        if (!m_pendingDir.empty()) {
            m_currentDir = std::move(m_pendingDir);
            m_pendingDir.clear();
            m_selected = std::filesystem::path{};
            refresh();
        }

        ImGui::EndChild();

        ImGui::EndTable();
    }

    drawDialogs();

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

    ImGui::End();
}

} // namespace fe
