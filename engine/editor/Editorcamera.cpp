#include "editor/EditorCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace fe {
namespace {

constexpr float kLookSensitivity = 0.003f;   // radians per pixel of mouse movement
constexpr float kMaxPitch        = 1.5533f;  // 89 degrees: avoids flipping at the poles
constexpr float kMinPivotDist    = 0.2f;

const glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

} // namespace

glm::vec3 EditorCamera::forward() const {
    // yaw = 0, pitch = 0 looks along -Z. A positive yaw turns left.
    return glm::vec3(-std::sin(m_yaw) * std::cos(m_pitch),
                      std::sin(m_pitch),
                     -std::cos(m_yaw) * std::cos(m_pitch));
}

glm::mat4 EditorCamera::view() const {
    return glm::lookAt(m_position, m_position + forward(), kWorldUp);
}

glm::mat4 EditorCamera::gizmoProjection(float aspect) const {
    return glm::perspective(glm::radians(m_fovDegrees), aspect,
                            m_nearPlane, m_farPlane);
}

glm::mat4 EditorCamera::projection(float aspect) const {
    glm::mat4 proj = gizmoProjection(aspect);
    proj[1][1] *= -1.0f;   // Vulkan clip space: +Y points down.
    return proj;
}

void EditorCamera::focusOn(const glm::vec3& target, float distance) {
    m_pivotDistance = std::max(distance, kMinPivotDist);
    m_position      = target - forward() * m_pivotDistance;
}

void EditorCamera::look(float dx, float dy) {
    // Moving the mouse right turns the view right (yaw decreases);
    // moving it down looks down (pitch decreases).
    m_yaw   -= dx * kLookSensitivity;
    m_pitch -= dy * kLookSensitivity;
    m_pitch  = std::clamp(m_pitch, -kMaxPitch, kMaxPitch);
}

void EditorCamera::move(float deltaTime, bool fast) {
    const glm::vec3 fwd   = forward();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, kWorldUp));

    glm::vec3 direction(0.0f);
    if (ImGui::IsKeyDown(ImGuiKey_W)) direction += fwd;
    if (ImGui::IsKeyDown(ImGuiKey_S)) direction -= fwd;
    if (ImGui::IsKeyDown(ImGuiKey_D)) direction += right;
    if (ImGui::IsKeyDown(ImGuiKey_A)) direction -= right;
    if (ImGui::IsKeyDown(ImGuiKey_E)) direction += kWorldUp;   // vertical moves follow the world,
    if (ImGui::IsKeyDown(ImGuiKey_Q)) direction -= kWorldUp;   // not the camera's tilt

    if (glm::dot(direction, direction) > 0.0f) {
        const float speed = m_flySpeed * (fast ? 3.0f : 1.0f);
        m_position += glm::normalize(direction) * speed * deltaTime;
    }
}

void EditorCamera::pan(float dx, float dy) {
    const glm::vec3 fwd   = forward();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, kWorldUp));
    const glm::vec3 up    = glm::cross(right, fwd);

    // Scale with the distance so panning feels the same at any zoom level.
    const float speed = m_pivotDistance * 0.002f;
    m_position += (-right * dx + up * dy) * speed;
}

void EditorCamera::orbit(float dx, float dy) {
    // Rotate the view, then place the camera so the point it was looking at
    // stays exactly where it was.
    const glm::vec3 pivot = m_position + forward() * m_pivotDistance;
    look(dx, dy);
    m_position = pivot - forward() * m_pivotDistance;
}

void EditorCamera::dolly(float wheel) {
    // Multiplicative: each notch covers a fixed fraction of the distance to
    // the pivot, so zooming is fine up close and fast from far away.
    const float amount = m_pivotDistance * wheel * 0.1f;
    m_position     += forward() * amount;
    m_pivotDistance = std::max(m_pivotDistance - amount, kMinPivotDist);
}

void EditorCamera::handleInput(bool viewportHovered) {
    const ImGuiIO& io = ImGui::GetIO();

    // A navigation drag can only begin over the viewport...
    if (m_mode == Mode::None && viewportHovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            m_mode = Mode::Fly;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            m_mode = Mode::Pan;
        else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_mode = Mode::Orbit;
    }

    // ...and ends when its button is released, wherever the cursor is.
    if (m_mode != Mode::None) {
        const ImGuiMouseButton button =
              (m_mode == Mode::Fly) ? ImGuiMouseButton_Right
            : (m_mode == Mode::Pan) ? ImGuiMouseButton_Middle
                                    : ImGuiMouseButton_Left;
        if (!ImGui::IsMouseDown(button))
            m_mode = Mode::None;
    }

    switch (m_mode) {
        case Mode::Fly:
            look(io.MouseDelta.x, io.MouseDelta.y);
            move(io.DeltaTime, io.KeyShift);
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);   // hide the cursor while looking
            break;
        case Mode::Pan:
            pan(io.MouseDelta.x, io.MouseDelta.y);
            break;
        case Mode::Orbit:
            orbit(io.MouseDelta.x, io.MouseDelta.y);
            break;
        case Mode::None:
            break;
    }

    if (io.MouseWheel != 0.0f) {
        if (m_mode == Mode::Fly) {
            // While flying, the wheel adjusts the speed instead of zooming.
            m_flySpeed = std::clamp(m_flySpeed * (1.0f + io.MouseWheel * 0.15f),
                                    0.2f, 100.0f);
        } else if (m_mode == Mode::None && viewportHovered) {
            dolly(io.MouseWheel);
        }
    }
}

} // namespace fe
