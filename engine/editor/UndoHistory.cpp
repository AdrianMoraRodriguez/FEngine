#include "editor/UndoHistory.h"

#include "scene/SceneSerializer.h"
#include "core/Log.h"

#include <imgui.h>

#include <utility>

namespace fe {

UndoHistory::UndoHistory(Scene& scene, entt::entity& selected)
    : m_scene(scene), m_selected(selected) {
    reset();
}

UndoHistory::State UndoHistory::capture() const {
    State state;
    state.text = SceneSerializer::serialize(m_scene);

    const entt::registry& reg = m_scene.registry();
    if (m_selected != entt::null && reg.valid(m_selected))
        if (const auto* id = reg.try_get<IDComponent>(m_selected))
            state.selected = id->uuid;
    return state;
}

bool UndoHistory::restore(const State& state) {
    // Load into a new scene first, so that a state that cannot be read leaves
    // the scene the user is working on untouched.
    Scene restored;
    if (!SceneSerializer::loadFromString(restored, state.text, "undo history")) return false;

    m_scene = std::move(restored);
    m_selected = (state.selected != 0) ? m_scene.findByUUID(state.selected) : entt::null;
    return true;
}

void UndoHistory::reset() {
    m_states.clear();
    m_states.push_back(capture());
    m_current = 0;
    m_bytes   = m_states.front().text.size();
    m_pendingCheck = m_undoRequested = m_redoRequested = false;
}

void UndoHistory::trim() {
    // The oldest state goes first; the current one is never dropped.
    while (m_current > 0 && (m_states.size() > kMaxSteps || m_bytes > kMaxBytes)) {
        m_bytes -= m_states.front().text.size();
        m_states.erase(m_states.begin());
        --m_current;
    }
}

bool UndoHistory::userIsBusy() {
    if (ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive()) return true;

    // Any key held, the modifiers included (Ctrl+click...).
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) return true;
    return false;
}

void UndoHistory::undo() {
    if (!canUndo()) return;
    if (restore(m_states[m_current - 1])) --m_current;
}

void UndoHistory::redo() {
    if (!canRedo()) return;
    if (restore(m_states[m_current + 1])) ++m_current;
}

void UndoHistory::drawMenuItems() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) m_undoRequested = true;
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo())) m_redoRequested = true;
}

void UndoHistory::update() {
    // ---- Undo and redo, from the shortcuts or the menu ----
    // Not while typing in a text field, which has an undo of its own, nor
    // while a popup is open.
    bool undoNow = m_undoRequested;
    bool redoNow = m_redoRequested;
    m_undoRequested = m_redoRequested = false;

    const bool shortcutsAllowed =
        !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (shortcutsAllowed) {
        // Ctrl+Shift+Z before Ctrl+Z: the chord test is exact about modifiers,
        // but this keeps the intent plain.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
            redoNow = true;
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
            undoNow = true;
    }

    if (undoNow || redoNow) {
        // Anything the user did right before the shortcut becomes a state of
        // its own first, so that it can be undone too.
        if (m_pendingCheck) {
            State state = capture();
            if (state.text != m_states[m_current].text) {
                m_bytes += state.text.size();
                m_states.resize(m_current + 1);   // a new edit ends the redo branch
                m_states.push_back(std::move(state));
                ++m_current;
            }
            m_pendingCheck = false;
        }
        if (undoNow) undo(); else redo();
        // Recompute: dropped redo states changed the total.
        m_bytes = 0;
        for (const State& s : m_states) m_bytes += s.text.size();
        return;
    }

    // ---- Recording ----
    if (userIsBusy()) {
        m_pendingCheck = true;
        return;
    }
    if (!m_pendingCheck) return;
    m_pendingCheck = false;

    State state = capture();
    if (state.text == m_states[m_current].text) return;   // nothing changed

    // A new edit after some undos ends the redo branch.
    for (std::size_t i = m_current + 1; i < m_states.size(); ++i)
        m_bytes -= m_states[i].text.size();
    m_states.resize(m_current + 1);

    m_bytes += state.text.size();
    m_states.push_back(std::move(state));
    ++m_current;
    trim();
}

} // namespace fe
