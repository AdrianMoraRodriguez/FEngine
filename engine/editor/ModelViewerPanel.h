#pragma once

/**
 * @file ModelViewerPanel.h
 * @brief Tabs that show a 3D model from the Asset Browser, with an orbiting camera.
 */

#include "editor/Panel.h"
#include "gfx/OffscreenBuffer.h"
#include "gfx/MeshLibrary.h"
#include "core/Geometry.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vulkan/vulkan.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fe {

class SceneRenderer;
class VulkanContext;

/**
 * @brief Shows each opened model in its own window, docked next to the Viewport.
 *
 * Controls inside a viewer:
 *   - Left drag turns the model, to see it from any side (also from below).
 *   - Right or middle drag pans.
 *   - Mouse wheel zooms.
 *   - "Frame" (or F with the pointer over the viewer) returns to the starting view.
 *
 * The camera and the light are fixed, like in the Asset Browser thumbnails,
 * and it is the model that turns. Turning the model is the same picture as
 * orbiting the camera, with one difference that matters here: the light
 * always comes from the camera's side, so the face you are looking at is
 * always lit, from every angle.
 *
 * ### Where it fits in the frame
 *  - update(): once per frame before recording; finishes loading models.
 *  - onImGui(): draws the windows and handles the mouse.
 *  - render(): while recording, outside any render pass, after the shadow
 *    passes; draws every viewer into its own OffscreenBuffer.
 *
 * Call shutdown() before ImGui's Vulkan backend goes away.
 */
class ModelViewerPanel : public Panel {
public:
    /**
     * @param ctx    Vulkan context; must outlive the panel.
     * @param scenes Scene renderer whose mesh library and pipeline draw the model.
     */
    ModelViewerPanel(const VulkanContext& ctx, SceneRenderer& scenes)
        : m_ctx(ctx), m_scenes(scenes) {}
    ~ModelViewerPanel() override { shutdown(); }

    /**
     * @brief Opens the model at @p path in a viewer, or focuses it if it is already open.
     * @return `false` if the file is outside the asset folder.
     */
    bool open(const std::filesystem::path& path);

    /// Finishes loads in progress and releases viewers that were closed. Call before recording the frame.
    void update();

    void onImGui() override;

    /// Draws every viewer that is ready. Call while recording, outside any render pass.
    void render(VkCommandBuffer cmd);

    /// Closes every viewer and frees their GPU resources. Safe to call twice.
    void shutdown();

private:
    enum class State { Loading, Ready, Failed };

    struct Viewer {
        std::filesystem::path path;
        std::string           label;        ///< Window title; the part after ### is its stable id.
        AssetHandle           handle = 0;
        State                 state  = State::Loading;

        std::unique_ptr<OffscreenBuffer> target;
        bool                  drawn  = false;   ///< The target has been drawn at least once.

        // The model turns around the middle of its bounding box; the camera stays put.
        glm::vec3             centre {0.0f};    ///< Middle of the box.
        glm::vec3             size   {0.0f};    ///< Box dimensions, for the info line.
        float                 radius = 1.0f;    ///< Radius of the bounding sphere.
        glm::quat             orientation {1.0f, 0.0f, 0.0f, 0.0f};  ///< How the model is turned.
        glm::vec3             panOffset {0.0f}; ///< Where the camera looks, relative to the centre.
        float                 distance = 1.0f;  ///< Camera distance to what it looks at.
        float                 aspect = 1.0f;

        bool                  isOpen = true;    ///< Cleared by the window's close button.
        bool                  placed = false;   ///< Docked once, the first time it appears.
        bool                  focus  = true;    ///< Bring to front on the next frame.
    };

    /// Puts the camera back where a new viewer starts.
    static void frame(Viewer& v);

    /// Projection * view of the viewer's camera, with the Vulkan Y flip applied.
    static glm::mat4 viewProjection(const Viewer& v);

    /// The model's placement: its turn around its own centre.
    static glm::mat4 modelMatrix(const Viewer& v);

    /// Direction from what the camera looks at towards the camera.
    static glm::vec3 cameraDirection();

    void drawViewer(Viewer& v);

    const VulkanContext&                 m_ctx;
    SceneRenderer&                       m_scenes;
    std::vector<std::unique_ptr<Viewer>> m_viewers;
};

} // namespace fe
