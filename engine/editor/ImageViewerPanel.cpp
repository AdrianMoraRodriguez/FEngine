#include "editor/ImageViewerPanel.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "editor/Icons.h"
#include "import/ImageLoader.h"

#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>

namespace fe {

namespace {

constexpr float kMinZoom = 0.02f;
constexpr float kMaxZoom = 64.0f;

/// Size of one square of the transparency checkerboard, in pixels.
constexpr float kCheckerSize = 10.0f;

/// Draws the checkerboard inside @p area (already clipped to the visible part).
void drawChecker(ImDrawList* dl, ImVec2 area0, ImVec2 area1, ImVec2 origin) {
    const ImU32 light = IM_COL32(70, 70, 76, 255);
    const ImU32 dark  = IM_COL32(48, 48, 54, 255);
    dl->AddRectFilled(area0, area1, dark);

    // Squares are anchored to the image's corner, so they move with it.
    const int firstX = static_cast<int>(std::floor((area0.x - origin.x) / kCheckerSize));
    const int firstY = static_cast<int>(std::floor((area0.y - origin.y) / kCheckerSize));
    for (int y = firstY; origin.y + y * kCheckerSize < area1.y; ++y) {
        for (int x = firstX; origin.x + x * kCheckerSize < area1.x; ++x) {
            if (((x + y) & 1) == 0) continue;
            const ImVec2 a(std::max(origin.x + x * kCheckerSize, area0.x),
                           std::max(origin.y + y * kCheckerSize, area0.y));
            const ImVec2 b(std::min(origin.x + (x + 1) * kCheckerSize, area1.x),
                           std::min(origin.y + (y + 1) * kCheckerSize, area1.y));
            dl->AddRectFilled(a, b, light);
        }
    }
}

} // namespace

bool ImageViewerPanel::open(const std::filesystem::path& path) {
    for (auto& v : m_viewers) {
        if (v->path == path) { v->focus = true; return true; }
    }

    std::optional<ImageData> image = loadImage(path);
    if (!image) {
        FE_ERROR("Image viewer: cannot load '%s'", toUtf8(path).c_str());
        return false;
    }

    auto v = std::make_unique<Viewer>();
    v->path = path;

    // UNORM, not SRGB: ImGui writes the sampled values straight to the
    // screen, so the pixels must reach it unchanged.
    if (!v->texture.create(m_ctx, *image, VK_FORMAT_R8G8B8A8_UNORM, true)) {
        FE_ERROR("Image viewer: cannot upload '%s'", toUtf8(path).c_str());
        return false;
    }

    v->set = ImGui_ImplVulkan_AddTexture(v->texture.sampler(), v->texture.view(),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!v->set) {
        FE_ERROR("Image viewer: ImGui_ImplVulkan_AddTexture failed");
        return false;
    }

    // "###" makes everything after it the window's identity, so the title can
    // hold the icon and name while the id stays the full path.
    v->label = std::string(FE_ICON_IMAGE "  ") + toUtf8(path.filename()) +
               "###imgview:" + toUtf8(path);

    m_viewers.push_back(std::move(v));
    return true;
}

void ImageViewerPanel::release(Viewer& v) {
    vkDeviceWaitIdle(m_ctx.device());
    if (v.set) {
        ImGui_ImplVulkan_RemoveTexture(v.set);
        v.set = VK_NULL_HANDLE;
    }
    v.texture.destroy();
}

void ImageViewerPanel::shutdown() {
    // The destructor calls this again after the Vulkan device is gone; with
    // no viewers left there is nothing to do and the device is not touched.
    if (m_viewers.empty()) return;
    for (auto& v : m_viewers) release(*v);
    m_viewers.clear();
}

void ImageViewerPanel::onImGui() {
    // Viewers closed last frame go first: the GPU is done with them now.
    for (auto it = m_viewers.begin(); it != m_viewers.end();) {
        if (!(*it)->isOpen) {
            release(**it);
            it = m_viewers.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& v : m_viewers) drawViewer(*v);
}

void ImageViewerPanel::drawViewer(Viewer& v) {
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

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(v.label.c_str(), &v.isOpen, flags)) {
        ImGui::End();
        return;
    }

    const float w = static_cast<float>(v.texture.width());
    const float h = static_cast<float>(v.texture.height());

    // ---- Toolbar ----
    if (ImGui::Button("Fit")) v.fit = true;
    ImGui::SameLine();
    if (ImGui::Button("1:1")) { v.fit = false; v.zoom = 1.0f; v.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    ImGui::TextDisabled("%u x %u    %.0f%%", v.texture.width(), v.texture.height(),
                        v.zoom * 100.0f);
    ImGui::Separator();

    // ---- Canvas ----
    const ImVec2 canvasPos  = ImGui::GetCursorScreenPos();
    ImVec2       canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 1.0f);
    canvasSize.y = std::max(canvasSize.y, 1.0f);
    const ImVec2 canvasEnd(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
    const ImVec2 canvasCentre(canvasPos.x + canvasSize.x * 0.5f,
                              canvasPos.y + canvasSize.y * 0.5f);

    ImGui::InvisibleButton("##canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    const ImGuiIO& io  = ImGui::GetIO();

    if (v.fit) {
        v.zoom = std::clamp(std::min(canvasSize.x / w, canvasSize.y / h), kMinZoom, 8.0f);
        v.pan  = ImVec2(0.0f, 0.0f);
    }

    // Wheel: zoom keeping the pixel under the cursor where it is.
    if (hovered && io.MouseWheel != 0.0f) {
        const float oldZoom = v.zoom;
        const float newZoom = std::clamp(oldZoom * std::pow(1.15f, io.MouseWheel),
                                         kMinZoom, kMaxZoom);
        const ImVec2 centre(canvasCentre.x + v.pan.x, canvasCentre.y + v.pan.y);
        const float  k = newZoom / oldZoom;
        const ImVec2 newCentre(io.MousePos.x - (io.MousePos.x - centre.x) * k,
                               io.MousePos.y - (io.MousePos.y - centre.y) * k);
        v.pan  = ImVec2(newCentre.x - canvasCentre.x, newCentre.y - canvasCentre.y);
        v.zoom = newZoom;
        v.fit  = false;
    }

    // Drag: pan.
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        v.pan.x += io.MouseDelta.x;
        v.pan.y += io.MouseDelta.y;
        v.fit = false;
    }

    // ---- Draw ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvasPos, canvasEnd, true);
    dl->AddRectFilled(canvasPos, canvasEnd, IM_COL32(24, 24, 28, 255));

    const ImVec2 size(w * v.zoom, h * v.zoom);
    const ImVec2 p0(canvasCentre.x + v.pan.x - size.x * 0.5f,
                    canvasCentre.y + v.pan.y - size.y * 0.5f);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Only the visible part of the checkerboard needs drawing.
    const ImVec2 vis0(std::max(p0.x, canvasPos.x), std::max(p0.y, canvasPos.y));
    const ImVec2 vis1(std::min(p1.x, canvasEnd.x), std::min(p1.y, canvasEnd.y));
    if (vis0.x < vis1.x && vis0.y < vis1.y)
        drawChecker(dl, vis0, vis1, p0);

    dl->AddImage(reinterpret_cast<ImTextureID>(v.set), p0, p1);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40));
    dl->PopClipRect();

    ImGui::End();
}

} // namespace fe
