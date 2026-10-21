#pragma once

/**
 * @file EditorCamera.h
 * @brief Free-fly camera used to navigate the scene viewport.
 *
 * Controls (the first press must happen over the viewport; the drag then
 * keeps working wherever the cursor goes):
 *   - Right mouse held        : look around, and fly with W A S D
 *                               (Q down, E up, Shift faster, wheel = speed)
 *   - Middle mouse drag       : pan
 *   - Alt + left mouse drag   : orbit around a point in front of the camera
 *   - Mouse wheel             : move forward / backward
 *
 * It is an editor tool, not a scene entity: the CameraComponent of a scene
 * is what the game uses, this is what the author uses to look around.
 */

#include <glm/glm.hpp>

namespace fe {

class EditorCamera {
public:
    /**
     * @brief Applies mouse and keyboard input to the camera.
     *
     * Call once per frame. Navigation drags are latched: they can only
     * start while @p viewportHovered is true, but continue until the button
     * is released even if the cursor leaves the viewport.
     *
     * @param viewportHovered Whether the cursor is over the viewport image
     *                        and no gizmo is being dragged.
     */
    void handleInput(bool viewportHovered);

    /// @return `true` while a navigation drag (fly, pan or orbit) is in progress.
    bool isNavigating() const { return m_mode != Mode::None; }

    /**
     * @brief Moves the camera so it looks at a point from a given distance.
     *
     * The viewing direction is kept; only the position changes.
     *
     * @param target   Point to look at, in world space.
     * @param distance Distance from the target to the camera.
     */
    void focusOn(const glm::vec3& target, float distance);

    /// @return World-to-view matrix.
    glm::mat4 view() const;

    /**
     * @brief Returns the projection matrix used for rendering with Vulkan.
     *
     * The Y axis is flipped because Vulkan's clip space has +Y pointing
     * down, whereas glm::perspective assumes OpenGL (+Y up). Without the
     * flip the image is upside down and face culling picks the wrong faces.
     *
     * @param aspect Width divided by height of the render target.
     * @see gizmoProjection()
     */
    glm::mat4 projection(float aspect) const;

    /**
     * @brief Returns the same projection without the Vulkan Y flip.
     *
     * Everything that works in OpenGL conventions must receive this matrix:
     * ImGuizmo, the editor overlays drawn with ImGui, and picking rays.
     * They only convert between world and screen coordinates and never
     * touch the depth buffer.
     *
     * @param aspect Width divided by height of the render target.
     */
    glm::mat4 gizmoProjection(float aspect) const;

    /// @return Camera position in world space.
    glm::vec3 position() const { return m_position; }

private:
    enum class Mode { None, Fly, Pan, Orbit };

    /// @return Unit vector the camera is looking along.
    glm::vec3 forward() const;

    void look(float dx, float dy);
    void move(float deltaTime, bool fast);
    void pan(float dx, float dy);
    void orbit(float dx, float dy);
    void dolly(float wheel);

    Mode m_mode = Mode::None;

    glm::vec3 m_position {3.0f, 2.5f, 3.0f};
    float     m_yaw      = 0.785f;    ///< Rotation around Y (radians). 0 looks along -Z.
    float     m_pitch    = -0.561f;   ///< Elevation (radians). Positive looks up.

    /// Distance from the camera to the point it orbits around and zooms
    /// towards. It is not a position: it follows the camera as it flies.
    float m_pivotDistance = 4.7f;

    float m_flySpeed = 4.0f;          ///< Fly speed in world units per second.

    float m_fovDegrees = 60.0f;
    float m_nearPlane  = 0.1f;
    float m_farPlane   = 1000.0f;
};

} // namespace fe
