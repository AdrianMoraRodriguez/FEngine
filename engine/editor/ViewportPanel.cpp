#include "editor/ViewportPanel.h"

#include "core/Log.h"
#include "core/PathUtil.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/constants.hpp>
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

// ---------------------------------------------------------------------------
// Light gizmos
// ---------------------------------------------------------------------------

// The combo in the light inspector maps its index straight to the enum, and
// the gizmo code below relies on the same order.
static_assert(static_cast<int>(LightComponent::Type::Directional) == 0,
              "Light gizmos assume the order Directional, Point, Spot");
constexpr int kLightDirectional = 0;
constexpr int kLightPoint       = 1;
constexpr int kLightSpot        = 2;

constexpr float kLightIconRadius   = 0.25f;  // size of the little symbol, world units
constexpr float kLightPickHalfSize = 0.3f;   // half size of the box that selects a light
constexpr float kMaxDrawnRange     = 200.0f; // keeps an absurd range from drawing a huge sphere
constexpr float kMaxConeRadius     = 6.0f;   // widest circle drawn at the end of a spot cone

/// Copy of @p m with the scale removed from its three axes. Gizmos keep the
/// same size however much the entity is scaled.
glm::mat4 withoutScale(const glm::mat4& m) {
    glm::mat4 result = m;
    for (int i = 0; i < 3; ++i) {
        const float length = glm::length(glm::vec3(m[i]));
        if (length > 1e-6f)
            result[i] = glm::vec4(glm::vec3(m[i]) / length, 0.0f);
    }
    return result;
}

/// Colour of a light's gizmo: the light's own colour (never too dark to see),
/// or orange when the light is selected, like the camera gizmos.
ImU32 lightGizmoColor(const glm::vec3& lightColor, bool selected, int alpha = 255) {
    if (selected) return IM_COL32(255, 170, 40, alpha);

    const glm::vec3 c = glm::clamp(glm::max(lightColor, glm::vec3(0.35f)),
                                   glm::vec3(0.0f), glm::vec3(1.0f));
    return IM_COL32(static_cast<int>(c.x * 255.0f),
                    static_cast<int>(c.y * 255.0f),
                    static_cast<int>(c.z * 255.0f), alpha);
}

/// Draws lines in the local space of a model matrix, inside the viewport rectangle.
struct GizmoPen {
    ImDrawList*      draw;
    const glm::mat4& viewProj;
    glm::mat4        model;
    float x, y, width, height;   ///< The viewport image rectangle on screen.
    ImU32            color;
    float            thickness;

    void line(const glm::vec3& a, const glm::vec3& b) const {
        drawWorldLine(draw, viewProj, model, a, b, x, y, width, height, color, thickness);
    }

    /// A circle in the plane spanned by the unit vectors @p u and @p v.
    void circle(const glm::vec3& center, const glm::vec3& u, const glm::vec3& v,
                float radius) const {
        constexpr int kSegments = 24;
        glm::vec3 previous = center + u * radius;
        for (int i = 1; i <= kSegments; ++i) {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / kSegments;
            const glm::vec3 point = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
            line(previous, point);
            previous = point;
        }
    }
};

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
    drawLightGizmos(imageMin.x, imageMin.y, imageSize.x, imageSize.y);
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

void ViewportPanel::drawLightGizmos(float x, float y, float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return;

    const glm::mat4 viewProj =
        m_camera.gizmoProjection(width / height) * m_camera.view();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    const glm::vec3 axisX{1.0f, 0.0f, 0.0f};
    const glm::vec3 axisY{0.0f, 1.0f, 0.0f};
    const glm::vec3 axisZ{0.0f, 0.0f, 1.0f};
    const glm::vec3 origin{0.0f};
    const float     r = kLightIconRadius;

    auto lights = m_scene.registry().view<const LightComponent, const WorldTransform>();
    for (auto [entity, light, world] : lights.each()) {
        const bool      selected = (entity == m_selected);
        const glm::vec3 tint     = glm::vec3(light.color);
        const float     thick    = selected ? 2.0f : 1.5f;
        const int       type     = static_cast<int>(light.type);

        // A light shines along its local -Z axis (like a camera looks), so the
        // directional and spot shapes follow the entity's rotation. A point
        // light shines everywhere, so it only needs the position.
        glm::mat4 model = withoutScale(world.matrix);
        if (type == kLightPoint) {
            model    = glm::mat4(1.0f);
            model[3] = world.matrix[3];
        }

        const GizmoPen pen{draw, viewProj, model, x, y, width, height,
                           lightGizmoColor(tint, selected), thick};
        const char* label = "Light";

        if (type == kLightDirectional) {
            label = "Directional light";

            // A disc (the sun), parallel rays leaving it, and one arrow
            // along the direction the light travels.
            pen.circle(origin, axisX, axisY, r);

            const float rayLength = 1.2f;
            for (int i = 0; i < 4; ++i) {
                const float angle = glm::half_pi<float>() * static_cast<float>(i);
                const glm::vec3 start(std::cos(angle) * r, std::sin(angle) * r, 0.0f);
                pen.line(start, start - axisZ * rayLength);
            }

            const float length = 1.8f, head = 0.12f, back = 0.3f;
            pen.line(origin, -axisZ * length);
            pen.line(-axisZ * length, glm::vec3( head, 0.0f, -length + back));
            pen.line(-axisZ * length, glm::vec3(-head, 0.0f, -length + back));
            pen.line(-axisZ * length, glm::vec3(0.0f,  head, -length + back));
            pen.line(-axisZ * length, glm::vec3(0.0f, -head, -length + back));

        } else if (type == kLightPoint) {
            label = "Point light";

            // Three small crossed circles: a bulb.
            pen.circle(origin, axisX, axisY, r);
            pen.circle(origin, axisX, axisZ, r);
            pen.circle(origin, axisY, axisZ, r);

            // The area it lights is only drawn for the selected light, so a
            // scene full of lights does not turn into a tangle of spheres.
            if (selected) {
                const GizmoPen rangePen{draw, viewProj, model, x, y, width, height,
                                        lightGizmoColor(tint, true, 120), 1.0f};
                const float range = std::clamp(light.range, 0.01f, kMaxDrawnRange);
                rangePen.circle(origin, axisX, axisY, range);
                rangePen.circle(origin, axisX, axisZ, range);
                rangePen.circle(origin, axisY, axisZ, range);
            }

        } else if (type == kLightSpot) {
            label = "Spot light";

            // A cone from the light with the light's own outer angle, as long
            // as its range. A very wide cone is drawn shorter rather than
            // narrower, so the shape stays true and the gizmo stays a
            // reasonable size.
            const float outer  = std::clamp(light.outerConeDegrees, 1.0f, 89.0f);
            const float inner  = std::clamp(light.innerConeDegrees, 0.0f, outer);
            const float tanOut = std::tan(glm::radians(outer));
            const float length = std::max(std::min(std::clamp(light.range, 0.5f, 10.0f),
                                                   kMaxConeRadius / tanOut), 0.3f);
            const float radius = length * tanOut;
            const glm::vec3 base = -axisZ * length;

            pen.circle(origin, axisX, axisY, r * 0.5f);        // the lamp
            pen.circle(base, axisX, axisY, radius);            // where the cone ends
            pen.line(origin, base + axisX * radius);
            pen.line(origin, base - axisX * radius);
            pen.line(origin, base + axisY * radius);
            pen.line(origin, base - axisY * radius);

            // The inner cone (full strength) as a dimmer ring, selected light only.
            if (selected && inner > 0.0f) {
                const GizmoPen innerPen{draw, viewProj, model, x, y, width, height,
                                        lightGizmoColor(tint, true, 120), 1.0f};
                innerPen.circle(base, axisX, axisY, length * std::tan(glm::radians(inner)));
            }
        }

        // Name the type of the selected light next to its symbol.
        if (selected) {
            const glm::vec4 clip = viewProj * world.matrix[3];
            if (clip.w > 0.01f) {
                const ImVec2 screen(x + (clip.x / clip.w * 0.5f + 0.5f) * width + 12.0f,
                                    y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height - 22.0f);
                draw->AddText(screen, IM_COL32(255, 170, 40, 255), label);
            }
        }
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

    // Lights have no mesh either: a small box around their symbol is the
    // thing to click, so a light can be selected without the hierarchy.
    auto lights = m_scene.registry().view<const LightComponent, const WorldTransform>();
    for (auto [entity, light, world] : lights.each()) {
        const float h = kLightPickHalfSize;
        const Aabb  box{ {-h, -h, -h}, {h, h, h} };

        float t = 0.0f;
        if (intersectRayBox(ray, box, withoutScale(world.matrix), t) && t < bestT) {
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
