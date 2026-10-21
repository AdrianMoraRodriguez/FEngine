#pragma once

#include "editor/Panel.h"
#include "editor/EditorCamera.h"
#include "core/Geometry.h"
#include "gfx/OffscreenBuffer.h"
#include "gfx/SceneRenderer.h"
#include "scene/Scene.h"

#include <entt/entt.hpp>

#include <filesystem>

namespace fe {

/**
 * @brief Panel that shows the rendered scene and everything you can do in it.
 *
 *   - Navigation with the editor camera (see EditorCamera).
 *   - Click an object to select it; click empty space to deselect.
 *   - Transform gizmos on the selected entity.
 *   - Scene cameras are drawn as frustum gizmos, and can be selected too.
 *   - Drop a model from the Asset Browser to add it to the scene.
 *
 * Tool keys work while the pointer is over the viewport and the right mouse
 * button is not held (that button is for flying):
 *   - Q select, W translate, E rotate, R scale, F focus the selection.
 *
 * Hold Ctrl while dragging a gizmo to snap. The Local / World button chooses
 * the space the gizmo operates in.
 */
class ViewportPanel : public Panel {
public:
    /**
     * @param offscreen      Buffer the scene is rendered into. Resized to fit the panel.
     * @param camera         Editor camera driven by this panel.
     * @param scene          Scene shown and edited.
     * @param selectedEntity The editor's current selection.
     * @param sceneRenderer  Used to find which mesh lies under the cursor and
     *                       to register models dropped into the viewport.
     */
    ViewportPanel(OffscreenBuffer& offscreen,
                  EditorCamera&    camera,
                  Scene&           scene,
                  entt::entity&    selectedEntity,
                  SceneRenderer&   sceneRenderer)
        : m_offscreen(offscreen)
        , m_camera(camera)
        , m_scene(scene)
        , m_selected(selectedEntity)
        , m_sceneRenderer(sceneRenderer) {}

    void onImGui() override;

private:
    enum class Tool { Select, Translate, Rotate, Scale };

    /// Switches tool on Q / W / E / R and focuses the selection on F.
    void handleShortcuts();

    /// Draws the tool buttons with their top-left corner at the given screen position.
    void drawToolbar(float x, float y);

    /**
     * @brief Draws the list of models that are importing, cancelled or
     *        failed, with their buttons.
     *
     * Nothing is drawn when every model the scene uses is ready.
     */
    void drawImportOverlay(float x, float y);

    /// Draws the frustum gizmo of every scene camera inside the given rectangle.
    void drawCameraGizmos(float x, float y, float width, float height);

    /**
     * @brief Draws and processes the transform gizmo of the selected entity.
     * @return `true` if a gizmo was active this frame (so ImGuizmo's hover
     *         state can be trusted).
     */
    bool drawGizmo(float x, float y, float width, float height);

    /// Selects the entity under the cursor when the viewport is clicked.
    void handlePicking(float x, float y, float width, float height,
                       bool hovered, bool gizmoActive);

    /// @return The ray from the camera through the cursor, for the image
    ///         shown in the given rectangle.
    Ray cursorRay(float x, float y, float width, float height) const;

    /// @return The nearest selectable entity along the ray, or entt::null.
    entt::entity pickEntity(const Ray& ray) const;

    /// Adds an entity that shows the given model, placed under the cursor.
    void instantiateModel(const std::filesystem::path& file,
                          float x, float y, float width, float height);

    /// Points the editor camera at the selected entity.
    void focusSelected();

    OffscreenBuffer& m_offscreen;
    EditorCamera&    m_camera;
    Scene&           m_scene;
    entt::entity&    m_selected;
    SceneRenderer&   m_sceneRenderer;

    Tool m_tool       = Tool::Translate;
    bool m_localSpace = false;

    /// The pointer is over the import overlay this frame, so a click there
    /// must not select whatever is behind it.
    bool m_overlayHovered = false;
};

} // namespace fe
