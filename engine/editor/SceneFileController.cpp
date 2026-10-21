#include "editor/SceneFileController.h"

#include "scene/SceneSerializer.h"
#include "core/Log.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace fe {
namespace {

constexpr const char* kSceneExtension = ".fescene";

/// Centres the next popup on the main viewport and gives it a minimum width.
void prepareModal(float minWidth) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(minWidth, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
}

} // namespace

SceneFileController::SceneFileController(Scene& scene,
                                         entt::entity& selectedEntity,
                                         std::filesystem::path scenesDir)
    : m_scene(scene)
    , m_selected(selectedEntity)
    , m_scenesDir(std::move(scenesDir)) {}

// ---------------------------------------------------------------------------
// Dirty tracking
// ---------------------------------------------------------------------------

void SceneFileController::markClean() {
    m_savedSnapshot = SceneSerializer::serialize(m_scene);
}

bool SceneFileController::isDirty() const {
    return SceneSerializer::serialize(m_scene) != m_savedSnapshot;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void SceneFileController::requestNew() {
    if (isDirty()) {
        m_pendingAction         = Action::New;
        m_requestUnsavedDialog  = true;
    } else {
        doNew();
    }
}

void SceneFileController::requestOpen(const std::filesystem::path& path) {
    if (isDirty()) {
        m_pendingAction         = Action::Open;
        m_pendingPath           = path;
        m_requestUnsavedDialog  = true;
    } else {
        doOpen(path);
    }
}

void SceneFileController::requestSave() {
    if (m_currentPath.empty())
        m_requestSaveAsDialog = true;   // never saved: ask for a name first
    else
        doSave(m_currentPath);
}

void SceneFileController::doNew() {
    m_scene = Scene{};
    m_selected = entt::null;
    m_currentPath = std::filesystem::path{};
    markClean();
}

void SceneFileController::doOpen(const std::filesystem::path& path) {
    // Load into a temporary scene first: if the file is invalid, the scene
    // the user is working on stays untouched.
    Scene loaded;
    if (!SceneSerializer::loadFromFile(loaded, path)) return;

    m_scene = std::move(loaded);
    m_selected = entt::null;
    m_currentPath = path;
    markClean();
}

bool SceneFileController::doSave(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    if (!SceneSerializer::saveToFile(m_scene, path)) return false;

    m_currentPath = path;
    markClean();
    return true;
}

// ---------------------------------------------------------------------------
// Menu, shortcuts and per-frame update
// ---------------------------------------------------------------------------

void SceneFileController::drawMenuItems() {
    if (ImGui::MenuItem("New Scene", "Ctrl+N"))
        requestNew();
    if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
        m_requestOpenDialog = true;

    ImGui::Separator();

    if (ImGui::MenuItem("Save", "Ctrl+S"))
        requestSave();
    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
        m_requestSaveAsDialog = true;
}

void SceneFileController::handleShortcuts() {
    // Do not steal keys while typing, or while a dialog is already open.
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel)) return;

    // Ctrl+Shift+S must be tested before Ctrl+S.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
        m_requestSaveAsDialog = true;
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        requestSave();
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
        m_requestOpenDialog = true;
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
        requestNew();
}

void SceneFileController::onImGui() {
    handleShortcuts();

    // Open the popups requested by the menu or the shortcuts.
    if (m_requestOpenDialog) {
        refreshSceneList();
        ImGui::OpenPopup("Open Scene");
        m_requestOpenDialog = false;
    }
    if (m_requestSaveAsDialog) {
        const std::string suggestion = m_currentPath.empty()
            ? m_scene.name()
            : m_currentPath.stem().string();
        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", suggestion.c_str());
        ImGui::OpenPopup("Save Scene As");
        m_requestSaveAsDialog = false;
    }
    if (m_requestUnsavedDialog) {
        ImGui::OpenPopup("Unsaved Changes");
        m_requestUnsavedDialog = false;
    }

    drawOpenModal();
    drawSaveAsModal();
    drawUnsavedModal();
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

void SceneFileController::refreshSceneList() {
    m_sceneFiles.clear();

    std::error_code ec;
    if (!std::filesystem::exists(m_scenesDir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(m_scenesDir, ec)) {
        if (entry.is_regular_file(ec) &&
            entry.path().extension().string() == kSceneExtension)
            m_sceneFiles.push_back(entry.path());
    }
    std::sort(m_sceneFiles.begin(), m_sceneFiles.end());
}

std::filesystem::path SceneFileController::pathForName(const char* name) const {
    // Keep only the file-name part, so a typed name can never place the file
    // outside the scenes directory.
    std::filesystem::path file = std::filesystem::path(name).filename();
    if (file.empty() || file == "." || file == "..")
        return std::filesystem::path{};

    if (file.extension().string() != kSceneExtension)
        file += kSceneExtension;

    return m_scenesDir / file;
}

void SceneFileController::drawOpenModal() {
    prepareModal(320.0f);
    if (!ImGui::BeginPopupModal("Open Scene", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    if (m_sceneFiles.empty())
        ImGui::TextDisabled("No scenes found in this project yet.");

    for (const auto& path : m_sceneFiles) {
        const std::string label = path.stem().string();
        if (ImGui::Selectable(label.c_str(), path == m_currentPath)) {
            requestOpen(path);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void SceneFileController::drawSaveAsModal() {
    prepareModal(320.0f);
    if (!ImGui::BeginPopupModal("Save Scene As", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted("Scene name:");

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();   // focus the text field below
    const bool enterPressed = ImGui::InputText(
        "##name", m_nameBuf, sizeof(m_nameBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);

    const std::filesystem::path target = pathForName(m_nameBuf);
    const bool valid = !target.empty();

    if (valid) {
        ImGui::TextDisabled("Will be saved as: %s",
                            target.filename().string().c_str());

        std::error_code ec;
        if (std::filesystem::exists(target, ec) && target != m_currentPath)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                               "A scene with this name already exists and "
                               "will be overwritten.");
    }

    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Save") || (enterPressed && valid)) {
        m_scene.setName(target.stem().string());
        doSave(target);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void SceneFileController::drawUnsavedModal() {
    prepareModal(320.0f);
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted("The current scene has unsaved changes.");
    ImGui::TextUnformatted("Continue and discard them?");
    ImGui::Spacing();

    if (ImGui::Button("Discard changes")) {
        const Action action = m_pendingAction;
        m_pendingAction = Action::None;

        if (action == Action::New)       doNew();
        else if (action == Action::Open) doOpen(m_pendingPath);

        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_pendingAction = Action::None;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace fe
