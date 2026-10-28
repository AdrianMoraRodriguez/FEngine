#pragma once

/**
 * @file MeshDistanceFieldPanel.h
 * @brief A window that shows the distance field of the selected entity's mesh.
 */

#include "editor/Panel.h"

#include <entt/entt.hpp>

namespace fe {

class Scene;
class SceneRenderer;

/**
 * @brief Draws one slice of a mesh's signed distance field, to check it.
 *
 * Meant for developing the software ray tracing, and for finding out why a
 * model casts a strange shadow or reflection: the field is what rays are
 * traced against, so if the slice looks wrong the shadows will too.
 *
 * The slice is a picture of the field on a plane cutting the mesh: blue is
 * outside (brighter the farther from the surface), orange is inside, and the
 * white line is the surface itself. Thin dark lines mark equal distances.
 * Hovering a cell shows its distance.
 *
 * The window is hidden at first; View > Mesh Distance Field shows it.
 */
class MeshDistanceFieldPanel : public Panel {
public:
    /**
     * @param scene    The scene being edited.
     * @param selected The selected entity (the panel follows it).
     * @param renderer Owner of the mesh library.
     */
    MeshDistanceFieldPanel(Scene& scene, entt::entity& selected, SceneRenderer& renderer)
        : m_scene(scene), m_selected(selected), m_renderer(renderer) {}

    void onImGui() override;

    /// Whether the window is shown. The View menu toggles it.
    bool visible = false;

private:
    Scene&         m_scene;
    entt::entity&  m_selected;
    SceneRenderer& m_renderer;

    int   m_axis  = 1;      ///< The axis the slice is perpendicular to: 0 x, 1 y, 2 z.
    float m_slice = 0.5f;   ///< Where along that axis, from 0 to 1.
};

} // namespace fe
