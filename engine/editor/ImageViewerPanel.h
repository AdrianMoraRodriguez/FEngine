#pragma once

/**
 * @file ImageViewerPanel.h
 * @brief Tabs that show an image from the Asset Browser at full size.
 */

#include "editor/Panel.h"
#include "gfx/Texture.h"

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fe {

class VulkanContext;

/**
 * @brief Shows each opened image in its own window, docked next to the Viewport.
 *
 * Controls inside a viewer:
 *   - Mouse wheel zooms around the cursor.
 *   - Dragging with the left or middle button pans.
 *   - "Fit" scales the image to the window (it stays fitted while the window
 *     is resized); "1:1" shows one image pixel per screen pixel.
 *
 * Transparent images are drawn over a checkerboard.
 *
 * ### GPU resources
 * Every open image owns a Texture and an ImGui descriptor set. A viewer that
 * is closed is released at the start of the next frame, after the GPU has
 * finished the frames that might still read it.
 *
 * Call shutdown() before ImGui's Vulkan backend is shut down.
 */
class ImageViewerPanel : public Panel {
public:
    /// @param ctx Vulkan context; must outlive the panel.
    explicit ImageViewerPanel(const VulkanContext& ctx) : m_ctx(ctx) {}
    ~ImageViewerPanel() override { shutdown(); }

    /**
     * @brief Opens the image at @p path in a viewer, or focuses it if it is already open.
     * @return `false` if the file could not be loaded (the reason is logged).
     */
    bool open(const std::filesystem::path& path);

    void onImGui() override;

    /// Closes every viewer and frees their GPU resources. Safe to call twice.
    void shutdown();

private:
    struct Viewer {
        std::filesystem::path path;
        std::string           label;      ///< Window title; the part after ### is its stable id.
        Texture               texture;
        VkDescriptorSet       set = VK_NULL_HANDLE;
        float                 zoom = 1.0f;
        ImVec2                pan  = {0.0f, 0.0f};  ///< Offset of the image centre from the window centre.
        bool                  fit  = true;
        bool                  isOpen = true;  ///< Cleared by the window's close button.
        bool                  placed = false; ///< Docked once, the first time it appears.
        bool                  focus  = true;  ///< Bring to front on the next frame.
    };

    /// Draws one viewer window.
    void drawViewer(Viewer& v);

    /// Releases the GPU resources of @p v. Waits for the GPU.
    void release(Viewer& v);

    const VulkanContext&                  m_ctx;
    std::vector<std::unique_ptr<Viewer>>  m_viewers;
};

} // namespace fe
