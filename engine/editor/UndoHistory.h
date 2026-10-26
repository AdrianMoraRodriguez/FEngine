#pragma once

/**
 * @file UndoHistory.h
 * @brief Undo and redo for everything in the scene.
 */

#include "scene/Scene.h"

#include <entt/entt.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace fe {

/**
 * @brief Remembers the states the scene has been in, and goes back and forth
 *        between them (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z and the Edit menu).
 *
 * ### How it works
 * A state is the scene serialised to text (SceneSerializer), plus which
 * entity was selected. Undoing loads the previous text into a fresh Scene
 * and swaps it in, exactly as File > Open does. Nothing in the editor has to
 * announce its edits or be written as a command: whatever the serialiser
 * saves (entities, components, hierarchy, scene settings) can be undone.
 * What is not part of the scene file (the camera, which panels are open, the
 * selection on its own) is not undone.
 *
 * ### When a state is recorded
 * Once the user has finished a gesture: update() notes that the mouse, the
 * keyboard or a widget was busy, and the first frame afterwards in which
 * nothing is busy it serialises the scene once and compares it with the
 * current state. So a whole drag of a slider or of the gizmo is one step, not
 * one per frame, and a frame in which nothing happened costs nothing.
 *
 * Typing into a text field is left to the field's own undo, and the shortcuts
 * are ignored while a popup is open.
 *
 * ### Limits
 * The history keeps at most kMaxSteps states and about kMaxBytes of text; the
 * oldest are dropped first. A scene replaced by New or Open starts a new
 * history (see reset()).
 *
 * Usage, once per frame after the panels and before ImGui::Render():
 * @code
 * undo.update();
 * @endcode
 * and, inside the menu bar, `undo.drawMenuItems()` inside an Edit menu.
 */
class UndoHistory {
public:
    /// Most states kept, the current one included.
    static constexpr std::size_t kMaxSteps = 200;

    /// Most text kept, in bytes, over all the states. The current state is
    /// always kept, however large.
    static constexpr std::size_t kMaxBytes = 128u * 1024u * 1024u;

    /**
     * @param scene    The active scene. Its contents are replaced by undo and redo.
     * @param selected The editor selection, restored along with the scene.
     */
    UndoHistory(Scene& scene, entt::entity& selected);

    /// Forgets the history and takes the scene as it is now as the first
    /// state. Call it when the scene was replaced (New, Open) and once after
    /// building the startup scene.
    void reset();

    /// Handles the shortcuts and records the state of the scene when the user
    /// has finished a gesture. Call once per frame, after the panels.
    void update();

    /// Draws the Undo and Redo entries. Call inside the Edit menu.
    void drawMenuItems();

    bool canUndo() const { return m_current > 0; }
    bool canRedo() const { return m_current + 1 < m_states.size(); }

    /// Goes back one state. Does nothing at the oldest one.
    void undo();

    /// Goes forward one state. Does nothing at the newest one.
    void redo();

private:
    /// A moment of the scene.
    struct State {
        std::string text;       ///< The scene, as SceneSerializer::serialize() wrote it.
        UUID        selected = 0;   ///< UUID of the selected entity, or 0.
    };

    /// @return The scene as a state, with the current selection.
    State capture() const;

    /// Replaces the scene and the selection with @p state.
    /// @return `false` (and leaves the scene alone) if the text cannot be loaded.
    bool restore(const State& state);

    /// Drops the oldest states while there are too many or too much text.
    void trim();

    /// Whether the mouse, a key or a widget is in use this frame.
    static bool userIsBusy();

    Scene&         m_scene;
    entt::entity&  m_selected;

    std::vector<State> m_states;      ///< Oldest first.
    std::size_t        m_current = 0; ///< Index in m_states of the state the scene is in.
    std::size_t        m_bytes   = 0; ///< Text held by all the states.

    bool m_pendingCheck = false;   ///< The user did something since the last comparison.
    bool m_undoRequested = false;  ///< Asked from the menu; done in update(), between panels' frames.
    bool m_redoRequested = false;
};

} // namespace fe
