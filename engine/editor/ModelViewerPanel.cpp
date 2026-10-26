#include "editor/ModelViewerPanel.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "editor/Icons.h"
#include "gfx/SceneRenderer.h"
#include "scene/Components.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace fe {

namespace {

constexpr float kFovY       = 35.0f;    // degrees
constexpr float kTurnSpeed  = 0.008f;   // radians per pixel

} // namespace

bool ModelViewerPanel::open(const std::filesystem::path& path) {
    for (auto& v : m_viewers) {
        if (v->path == path) { v->focus = true; return true; }
    }

    const AssetHandle handle = m_scenes.meshes().registerModel(path);
    if (handle == builtin::MeshNone) {
        FE_ERROR("Model viewer: '%s' is not inside the asset folder", toUtf8(path).c_str());
        return false;
    }

    auto v = std::make_unique<Viewer>();
    v->path   = path;
    v->handle = handle;
    v->label  = std::string(FE_ICON_CUBOID "  ") + toUtf8(path.filename()) +
                "###modelview:" + toUtf8(path);
    m_viewers.push_back(std::move(v));
    return true;
}

glm::vec3 ModelViewerPanel::cameraDirection() {
    // The same direction the thumbnails are drawn from; the preview light is
    // placed to suit it (see SceneRenderer::setupPreviewLighting()).
    return glm::normalize(glm::vec3(0.75f, 0.55f, 1.0f));
}

void ModelViewerPanel::frame(Viewer& v) {
    v.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    v.panOffset   = glm::vec3(0.0f);
    // Far enough that the whole bounding sphere fits the vertical field of view.
    v.distance    = v.radius / std::sin(0.5f * glm::radians(kFovY)) * 1.1f;
}

glm::mat4 ModelViewerPanel::modelMatrix(const Viewer& v) {
    return glm::translate(glm::mat4(1.0f), v.centre) *
           glm::mat4_cast(v.orientation) *
           glm::translate(glm::mat4(1.0f), -v.centre);
}

glm::mat4 ModelViewerPanel::viewProjection(const Viewer& v) {
    const glm::vec3 target = v.centre + v.panOffset;
    const glm::vec3 eye    = target + cameraDirection() * v.distance;
    const glm::mat4 view   = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));

    // The planes follow the zoom: close enough to see detail, far enough to
    // keep the whole model in view when it is far away.
    const float nearPlane = std::max(v.distance * 0.02f, v.radius * 0.001f);
    const float farPlane  = v.distance + v.radius * 4.0f;
    glm::mat4 proj = glm::perspective(glm::radians(kFovY), v.aspect, nearPlane, farPlane);
    proj[1][1] *= -1.0f;   // Vulkan clip space: +Y points down (as in EditorCamera).
    return proj * view;
}

void ModelViewerPanel::update() {
    MeshLibrary& meshes = m_scenes.meshes();
    for (auto& v : m_viewers) {
        if (v->state != State::Loading) continue;

        // track = false: opening a viewer must not show a progress bar over the Viewport.
        if (meshes.ensureLoaded(v->handle, false)) {
            const std::optional<Aabb> box = meshes.bounds(v->handle);
            if (!box) { v->state = State::Failed; continue; }

            v->centre = 0.5f * (box->min + box->max);
            v->size   = box->max - box->min;
            v->radius = std::max(0.5f * glm::length(v->size), 1e-4f);
            frame(*v);
            v->state = State::Ready;
        } else if (meshes.isUnavailable(v->handle)) {
            v->state = State::Failed;   // failed to load, or cancelled
        }
    }
}

void ModelViewerPanel::onImGui() {
    // Viewers closed last frame go first. This is the start of a new ImGui
    // frame, so nothing refers to their pictures any more, and the GPU is
    // done with the frames that did (OffscreenBuffer::shutdown() waits for it).
    for (auto it = m_viewers.begin(); it != m_viewers.end();) {
        if (!(*it)->isOpen) {
            if ((*it)->target) (*it)->target->shutdown();
            it = m_viewers.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& v : m_viewers) drawViewer(*v);
}

void ModelViewerPanel::drawViewer(Viewer& v) {
    // First time: join the tab bar of the Viewport, if there is one.
    if (!v.placed) {
        if (const ImGuiWindow* vp = ImGui::FindWindowByName("Viewport")) {
            if (vp->DockId != 0)
                ImGui::SetNextWindowDockID(vp->DockId, ImGuiCond_Always);
        }
        ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Appearing);
        v.placed = true;
    }
    if (v.focus) {
        ImGui::SetNextWindowFocus();
        v.focus = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(v.label.c_str(), &v.isOpen,
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::End(); return; }

    if (v.state == State::Loading) {
        ImGui::SetCursorPos(ImVec2(12.0f, 12.0f));
        ImGui::TextDisabled("Loading %s ...", toUtf8(v.path.filename()).c_str());
        ImGui::End();
        return;
    }
    if (v.state == State::Failed) {
        ImGui::SetCursorPos(ImVec2(12.0f, 12.0f));
        ImGui::TextDisabled("Could not load %s", toUtf8(v.path.filename()).c_str());
        ImGui::End();
        return;
    }

    // ---- Toolbar ----
    ImGui::SetCursorPos(ImVec2(8.0f, 6.0f));
    if (ImGui::Button("Frame")) frame(v);
    ImGui::SameLine();
    ImGui::TextDisabled("%.3g x %.3g x %.3g", v.size.x, v.size.y, v.size.z);
    ImGui::SetCursorPosX(0.0f);
    ImGui::Separator();

    // ---- Picture ----
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t w = static_cast<uint32_t>(std::max(avail.x, 1.0f));
    const uint32_t h = static_cast<uint32_t>(std::max(avail.y, 1.0f));

    if (!v.target) {
        v.target = std::make_unique<OffscreenBuffer>();
        if (!v.target->init(m_ctx, w, h)) {
            v.target.reset();
            v.state = State::Failed;
            ImGui::End();
            return;
        }
    } else {
        v.target->resize(w, h);
    }
    v.aspect = static_cast<float>(w) / static_cast<float>(h);

    const ImVec2 pictureSize(static_cast<float>(w), static_cast<float>(h));
    const ImVec2 picturePos = ImGui::GetCursorScreenPos();

    // Until render() has drawn it once, the image has no contents to show.
    if (v.drawn)
        ImGui::Image(reinterpret_cast<ImTextureID>(v.target->imguiDescriptorSet()), pictureSize);

    // An image does not react to the mouse: the buttons on top of it do, and
    // they keep reporting the drag while the pointer leaves the picture.
    ImGui::SetCursorScreenPos(picturePos);
    ImGui::InvisibleButton("##model", pictureSize,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);

    // ---- Mouse ----
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (hovered) {
        // Wheel: zoom, in proportion to the distance so it feels the same at any size.
        if (io.MouseWheel != 0.0f) {
            v.distance *= std::pow(0.88f, io.MouseWheel);
            v.distance = std::clamp(v.distance, v.radius * 0.05f, v.radius * 80.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(v);
    }

    // The camera's own axes, as directions in the world: the model turns around them.
    const glm::vec3 forward = -cameraDirection();
    const glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up      = glm::cross(right, forward);

    const bool dragging = ImGui::IsItemActive();
    if (dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        // Dragging right brings the front of the model to the right, dragging
        // down brings it down. Turning around the camera's axes (and not the
        // model's) keeps this true whatever side is facing you.
        const glm::quat turnY = glm::angleAxis(io.MouseDelta.x * kTurnSpeed, up);
        const glm::quat turnX = glm::angleAxis(io.MouseDelta.y * kTurnSpeed, right);
        v.orientation = glm::normalize(turnY * turnX * v.orientation);
    }
    if ((hovered || dragging) &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        // Pan in the screen plane; one pixel moves the model by one pixel at the centre.
        const float worldPerPixel =
            2.0f * v.distance * std::tan(0.5f * glm::radians(kFovY)) / static_cast<float>(h);
        v.panOffset += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * worldPerPixel;
    }

    ImGui::End();
}

void ModelViewerPanel::render(VkCommandBuffer cmd) {
    for (auto& v : m_viewers) {
        if (v->state != State::Ready || !v->target) continue;
        if (!v->isOpen) continue;   // being closed: the image may be gone next frame
        if (m_scenes.renderMeshView(cmd, v->handle, *v->target, viewProjection(*v),
                                    modelMatrix(*v)))
            v->drawn = true;
    }
}

void ModelViewerPanel::shutdown() {
    // The destructor calls this again after the Vulkan device is gone; with
    // no viewers left there is nothing to do and the device is not touched.
    if (m_viewers.empty()) return;
    for (auto& v : m_viewers)
        if (v->target) v->target->shutdown();
    m_viewers.clear();
}

} // namespace fe
