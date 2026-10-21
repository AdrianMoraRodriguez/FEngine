#include "editor/ViewportPanel.h"

#include "core/Log.h"
#include "core/PathUtil.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace fe {
namespace {

// ---------------------------------------------------------------------------
// Scene camera gizmo
// ---------------------------------------------------------------------------

constexpr float kCameraGizmoDepth  = 0.8f;           // world units, apex to far rectangle
constexpr float kCameraGizmoAspect = 16.0f / 9.0f;   // shape of the drawn rectangle

/// Size of the pyramid drawn for a scene camera. It follows the camera's
/// field of view, so changing FOV in the inspector reshapes the gizmo.
struct CameraShape {
    float halfWidth;
    float halfHeight;
    float depth;
};

CameraShape cameraShape(const CameraComponent& camera) {
    // Clamp so an extreme field of view cannot produce an enormous gizmo.
    const float halfHeight = std::min(
        kCameraGizmoDepth * std::tan(glm::radians(camera.fovDegrees) * 0.5f), 2.0f);
    return { halfHeight * kCameraGizmoAspect, halfHeight, kCameraGizmoDepth };
}

/// Draws a line given in the local space of @p model, projected onto the
/// viewport rectangle. Does the clipping against the camera plane that a
/// real rasteriser would do, so lines can cross behind the viewer safely.
void drawWorldLine(ImDrawList* draw,
                   const glm::mat4& viewProj, const glm::mat4& model,
                   const glm::vec3& a, const glm::vec3& b,
                   float x, float y, float width, float height,
                   ImU32 color, float thickness) {
    const glm::mat4 mvp = viewProj * model;
    glm::vec4 ca = mvp * glm::vec4(a, 1.0f);
    glm::vec4 cb = mvp * glm::vec4(b, 1.0f);

    // Points with w <= 0 are behind the camera and cannot be projected.
    constexpr float kNear = 0.01f;
    if (ca.w < kNear && cb.w < kNear) return;
    if (ca.w < kNear)      ca = ca + (cb - ca) * ((kNear - ca.w) / (cb.w - ca.w));
    else if (cb.w < kNear) cb = cb + (ca - cb) * ((kNear - cb.w) / (ca.w - cb.w));

    auto toScreen = [&](const glm::vec4& c) {
        const float ndcX = c.x / c.w;
        const float ndcY = c.y / c.w;   // +Y up (OpenGL convention)
        return ImVec2(x + (ndcX * 0.5f + 0.5f) * width,
                      y + (1.0f - (ndcY * 0.5f + 0.5f)) * height);
    };

    draw->AddLine(toScreen(ca), toScreen(cb), color, thickness);
}

} // namespace

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

void ViewportPanel::onImGui() {
    // ImGuizmo needs to start its frame before any other call to it.
    ImGuizmo::BeginFrame();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    // The mouse wheel moves the camera, so it must not scroll the panel.
    const bool visible = ImGui::Begin("Viewport", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (!visible) {   // collapsed, or docked behind another tab
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const ImVec2   avail = ImGui::GetContentRegionAvail();
    const uint32_t w     = static_cast<uint32_t>(avail.x);
    const uint32_t h     = static_cast<uint32_t>(avail.y);

    if (w > 0 && h > 0)
        m_offscreen.resize(w, h);

    ImGui::Image(
        reinterpret_cast<ImTextureID>(m_offscreen.imguiDescriptorSet()),
        avail);

    // Read these right after the image: they refer to the last item.
    const bool   imageHovered = ImGui::IsItemHovered();
    const ImVec2 imageMin     = ImGui::GetItemRectMin();
    const ImVec2 imageSize    = ImGui::GetItemRectSize();

    // A model dragged from the Asset Browser can be dropped on the image.
    // This must come right after the image too: the drop target belongs to
    // the last item.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FE_MODEL")) {
            // The Asset Browser sends the path as UTF-8 text.
            const std::filesystem::path file(std::u8string(
                reinterpret_cast<const char8_t*>(payload->Data)));
            instantiateModel(file, imageMin.x, imageMin.y, imageSize.x, imageSize.y);
        }
        ImGui::EndDragDropTarget();
    }

    handleShortcuts();

    drawCameraGizmos(imageMin.x, imageMin.y, imageSize.x, imageSize.y);
    const bool gizmoActive =
        drawGizmo(imageMin.x, imageMin.y, imageSize.x, imageSize.y);
    drawToolbar(imageMin.x + 8.0f, imageMin.y + 8.0f);
    drawImportOverlay(imageMin.x + 8.0f, imageMin.y + 44.0f);

    // While a gizmo is being dragged the camera must stay still.
    m_camera.handleInput(imageHovered && !(gizmoActive && ImGuizmo::IsUsing()));

    handlePicking(imageMin.x, imageMin.y, imageSize.x, imageSize.y,
                  imageHovered, gizmoActive);

    ImGui::End();
    ImGui::PopStyleVar();
}

void ViewportPanel::handleShortcuts() {
    const bool active = ImGui::IsWindowHovered() || ImGui::IsWindowFocused();
    if (!active) return;

    // Do not steal keys while typing. The right mouse button is held for
    // flying, where W, A, S, D, Q and E mean movement instead.
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_tool = Tool::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_tool = Tool::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_tool = Tool::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_tool = Tool::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelected();
}

void ViewportPanel::focusSelected() {
    if (m_selected == entt::null || !m_scene.registry().valid(m_selected)) return;

    const glm::mat4& m = m_scene.registry().get<WorldTransform>(m_selected).matrix;

    // Frame the object from a distance proportional to its size.
    const float size = std::max({ glm::length(glm::vec3(m[0])),
                                  glm::length(glm::vec3(m[1])),
                                  glm::length(glm::vec3(m[2])) });
    m_camera.focusOn(glm::vec3(m[3]), std::max(size * 3.0f, 2.0f));
}

void ViewportPanel::drawToolbar(float x, float y) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));

    auto toolButton = [this](const char* label, Tool tool) {
        const bool active = (m_tool == tool);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label))
            m_tool = tool;
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
    };

    toolButton("Q Select", Tool::Select);
    toolButton("W Move",   Tool::Translate);
    toolButton("E Rotate", Tool::Rotate);
    toolButton("R Scale",  Tool::Scale);

    if (ImGui::Button(m_localSpace ? "Local" : "World"))
        m_localSpace = !m_localSpace;
}

void ViewportPanel::drawImportOverlay(float x, float y) {
    m_overlayHovered = false;

    MeshLibrary& meshes = m_sceneRenderer.meshes();
    const std::vector<ImportStatus> statuses = meshes.importStatuses();
    if (statuses.empty()) return;

    // Buttons pressed below change the library, so they are only recorded
    // here and applied after the list has been drawn.
    AssetHandle toConfirm = 0;
    AssetHandle toCancel  = 0;

    ImGui::SetCursorScreenPos(ImVec2(x, y));

    // The viewport window has no padding (the image fills it), so the
    // overlay brings its own.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 0.85f));

    if (ImGui::BeginChild("##imports", ImVec2(340.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        m_overlayHovered = ImGui::IsWindowHovered();

        for (const ImportStatus& s : statuses) {
            ImGui::PushID(static_cast<int>(s.handle ^ (s.handle >> 32)));

            switch (s.state) {
                case ImportState::Loading:
                    ImGui::Text("Importing %s", s.name.c_str());
                    ImGui::ProgressBar(s.progress, ImVec2(230.0f, 0.0f));
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) toCancel = s.handle;
                    break;

                case ImportState::Declined:
                    ImGui::TextWrapped("%s: import cancelled.", s.name.c_str());
                    if (ImGui::Button("Import")) toConfirm = s.handle;
                    break;

                case ImportState::Failed:
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                       "%s could not be imported.", s.name.c_str());
                    ImGui::TextDisabled("See the log for the reason.");
                    break;
            }

            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (toConfirm != 0) meshes.confirmImport(toConfirm);
    if (toCancel  != 0) meshes.cancelImport(toCancel);
}

void ViewportPanel::drawCameraGizmos(float x, float y, float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return;

    const glm::mat4 viewProj =
        m_camera.gizmoProjection(width / height) * m_camera.view();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    auto cameras = m_scene.registry().view<const CameraComponent, const WorldTransform>();
    for (auto [entity, camera, world] : cameras.each()) {
        const CameraShape s        = cameraShape(camera);
        const bool        selected = (entity == m_selected);
        const ImU32       color    = selected ? IM_COL32(255, 170, 40, 255)
                                              : IM_COL32(210, 210, 210, 255);
        const float       thick    = selected ? 2.0f : 1.5f;
        const glm::mat4   model    = world.matrix;

        // A camera looks along its local -Z axis. The pyramid has its apex
        // at the camera and the framed rectangle at the far end.
        const glm::vec3 apex{0.0f};
        const glm::vec3 c0{-s.halfWidth, -s.halfHeight, -s.depth};
        const glm::vec3 c1{ s.halfWidth, -s.halfHeight, -s.depth};
        const glm::vec3 c2{ s.halfWidth,  s.halfHeight, -s.depth};
        const glm::vec3 c3{-s.halfWidth,  s.halfHeight, -s.depth};

        // Small triangle above the rectangle: shows which way is up.
        const glm::vec3 t0{-s.halfWidth * 0.5f, s.halfHeight,         -s.depth};
        const glm::vec3 t1{ s.halfWidth * 0.5f, s.halfHeight,         -s.depth};
        const glm::vec3 t2{ 0.0f,               s.halfHeight * 1.35f, -s.depth};

        auto line = [&](const glm::vec3& a, const glm::vec3& b) {
            drawWorldLine(draw, viewProj, model, a, b,
                          x, y, width, height, color, thick);
        };

        line(apex, c0); line(apex, c1); line(apex, c2); line(apex, c3);
        line(c0, c1);   line(c1, c2);   line(c2, c3);   line(c3, c0);
        line(t0, t1);   line(t1, t2);   line(t2, t0);
    }

    draw->PopClipRect();
}

bool ViewportPanel::drawGizmo(float x, float y, float width, float height) {
    if (m_tool == Tool::Select) return false;
    if (width <= 0.0f || height <= 0.0f) return false;
    if (m_selected == entt::null || !m_scene.registry().valid(m_selected)) return false;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();                 // draw into this window
    ImGuizmo::SetRect(x, y, width, height);  // where the scene image is

    // ImGuizmo works in OpenGL conventions, so it gets the projection
    // without the Vulkan Y flip.
    const glm::mat4 view = m_camera.view();
    const glm::mat4 proj = m_camera.gizmoProjection(width / height);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snapStep = 0.5f;                   // metres
    if (m_tool == Tool::Rotate) { operation = ImGuizmo::ROTATE; snapStep = 15.0f; } // degrees
    if (m_tool == Tool::Scale)  { operation = ImGuizmo::SCALE;  snapStep = 0.1f;  }

    const ImGuizmo::MODE mode = m_localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    const float  snapValues[3] = { snapStep, snapStep, snapStep };
    const float* snap          = ImGui::GetIO().KeyCtrl ? snapValues : nullptr;

    // The gizmo edits the entity's world matrix.
    glm::mat4 world = m_scene.registry().get<WorldTransform>(m_selected).matrix;

    const bool modified = ImGuizmo::Manipulate(
        glm::value_ptr(view), glm::value_ptr(proj),
        operation, mode,
        glm::value_ptr(world),
        nullptr, snap);

    // Convert back to the entity's local space; this also marks its world
    // matrix as stale, so the scene recomputes it (and its children's).
    if (modified)
        m_scene.setWorldMatrix(m_selected, world);

    return true;
}

Ray ViewportPanel::cursorRay(float x, float y, float width, float height) const {
    const ImGuiIO& io = ImGui::GetIO();

    // Cursor position inside the image, as normalised device coordinates
    // with +Y up (the same convention as gizmoProjection).
    const float ndcX =   ((io.MousePos.x - x) / width)  * 2.0f - 1.0f;
    const float ndcY = -(((io.MousePos.y - y) / height) * 2.0f - 1.0f);

    // Unproject the cursor at the near (depth 0) and far (depth 1) planes;
    // the line between the two points is the ray under the cursor.
    const glm::mat4 invViewProj =
        glm::inverse(m_camera.gizmoProjection(width / height) * m_camera.view());

    auto unproject = [&](float depth) {
        const glm::vec4 p = invViewProj * glm::vec4(ndcX, ndcY, depth, 1.0f);
        return glm::vec3(p) / p.w;
    };

    const glm::vec3 nearPoint = unproject(0.0f);
    const glm::vec3 farPoint  = unproject(1.0f);

    Ray ray;
    ray.origin    = nearPoint;
    ray.direction = glm::normalize(farPoint - nearPoint);
    return ray;
}

void ViewportPanel::handlePicking(float x, float y, float width, float height,
                                  bool hovered, bool gizmoActive) {
    if (!hovered || width <= 0.0f || height <= 0.0f) return;
    if (m_overlayHovered) return;
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) return;

    const ImGuiIO& io = ImGui::GetIO();

    // A click that belongs to something else must not change the selection:
    // grabbing a gizmo handle, pressing a toolbar button, or Alt-orbiting.
    if (gizmoActive && (ImGuizmo::IsOver() || ImGuizmo::IsUsing())) return;
    if (ImGui::IsAnyItemHovered()) return;
    if (io.KeyAlt || m_camera.isNavigating()) return;

    // Clicking empty space gives entt::null, which deselects.
    m_selected = pickEntity(cursorRay(x, y, width, height));
}

entt::entity ViewportPanel::pickEntity(const Ray& ray) const {
    // Meshes first...
    float        bestT = 0.0f;
    entt::entity best  = m_sceneRenderer.pick(m_scene, ray, bestT);
    if (best == entt::null)
        bestT = std::numeric_limits<float>::max();

    // ...then scene cameras, which have no mesh but a pickable volume that
    // matches the pyramid drawn for them. The nearest of everything wins.
    auto cameras = m_scene.registry().view<const CameraComponent, const WorldTransform>();
    for (auto [entity, camera, world] : cameras.each()) {
        const CameraShape s = cameraShape(camera);
        const Aabb box{ {-s.halfWidth, -s.halfHeight, -s.depth},
                        { s.halfWidth,  s.halfHeight,  0.0f} };

        float t = 0.0f;
        if (intersectRayBox(ray, box, world.matrix, t) && t < bestT) {
            bestT = t;
            best  = entity;
        }
    }

    return best;
}

void ViewportPanel::instantiateModel(const std::filesystem::path& file,
                                     float x, float y, float width, float height) {
    const AssetHandle handle = m_sceneRenderer.meshes().registerModel(file);
    if (handle == builtin::MeshNone) return;   // registerModel already said why

    // Place the model where the cursor ray meets the ground plane (y = 0).
    // If the ray never reaches the ground, or it is absurdly far, put the
    // model a few metres in front of the camera instead.
    const Ray ray = cursorRay(x, y, width, height);

    glm::vec3 position = ray.origin + ray.direction * 5.0f;
    if (std::abs(ray.direction.y) > 1e-4f) {
        const float t = -ray.origin.y / ray.direction.y;
        if (t > 0.0f && t < 200.0f)
            position = ray.origin + ray.direction * t;
    }

    entt::entity entity = m_scene.create(toUtf8(file.stem()));

    MeshRenderer renderer;
    renderer.mesh = handle;
    m_scene.registry().emplace<MeshRenderer>(entity, renderer);

    m_scene.transform(entity).position = position;
    m_selected = entity;
}

} // namespace fe
