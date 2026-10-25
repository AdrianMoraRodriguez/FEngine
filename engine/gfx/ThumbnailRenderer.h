#pragma once

/**
 * @file ThumbnailRenderer.h
 * @brief Small previews of 3D model files, drawn on demand by the GPU.
 *
 * The Asset Browser asks for the thumbnail of every model tile it shows. A
 * thumbnail is a 128 x 128 picture of the model, seen from a fixed angle under
 * a fixed light (see SceneRenderer::renderPreview()), kept as a texture that
 * ImGui can draw.
 *
 * A thumbnail is not available the first time it is asked for. Making it
 * takes three steps, spread over several frames so that the editor never
 * stalls:
 *   1. @ref Queued   the tile asked for it; it waits its turn.
 *   2. @ref Loading  the model is read on a background thread (MeshLibrary)
 *                    and uploaded to the GPU.
 *   3. @ref Ready    its picture is drawn into its own small image.
 * Until then request() returns nothing and the browser shows the plain icon.
 *
 * Only tiles that are actually on screen are queued: a request that stops
 * being repeated is dropped, so scrolling past hundreds of models does not
 * load all of them.
 *
 * Limits, to keep a big folder from eating the machine:
 *   - kMaxThumbnails pictures at most (about 128 KB each).
 *   - Model files bigger than kMaxFileBytes keep their icon.
 *   - At most kMaxLoading models are being read at once, and kMaxDrawnPerFrame
 *     pictures are drawn per frame.
 *
 * Models loaded for a thumbnail stay in the MeshLibrary, like the ones the
 * scene uses, so adding the model to the scene afterwards is instant.
 *
 * Per frame, in this order:
 * @code
 * thumbnails.update();                 // before Renderer::beginFrame()
 * // ... record the frame ...
 * thumbnails.render(cmd);              // outside any render pass
 * // ... while building the UI:
 * VkDescriptorSet picture = thumbnails.request(modelFile);
 * @endcode
 */

#include "gfx/OffscreenBuffer.h"
#include "scene/Components.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe {

class SceneRenderer;
class VulkanContext;

class ThumbnailRenderer {
public:
    static constexpr uint32_t       kSize           = 128;   ///< Side of a thumbnail, in pixels.
    static constexpr std::size_t    kMaxThumbnails  = 256;   ///< Most pictures kept at once.
    static constexpr std::uintmax_t kMaxFileBytes   = 64ull * 1024 * 1024;
    static constexpr std::size_t    kMaxLoading     = 2;     ///< Models read at the same time.
    static constexpr std::size_t    kMaxDrawnPerFrame = 2;   ///< Pictures drawn per frame.

    ThumbnailRenderer() = default;
    ~ThumbnailRenderer();

    ThumbnailRenderer(const ThumbnailRenderer&)            = delete;
    ThumbnailRenderer& operator=(const ThumbnailRenderer&) = delete;

    /**
     * @brief Prepares the renderer.
     * @param ctx    Initialised Vulkan context.
     * @param scenes The scene renderer whose meshes and pipeline are used.
     *               It must outlive this object.
     */
    void init(const VulkanContext& ctx, SceneRenderer& scenes);

    /**
     * @brief Destroys every picture. Safe to call more than once.
     *
     * The pictures are registered with ImGui, so call it while ImGui is still
     * alive, and after the GPU has finished with them (vkDeviceWaitIdle()).
     */
    void shutdown();

    /**
     * @brief Asks for the thumbnail of a model file.
     *
     * Call it every frame the tile is visible. The first call queues the
     * thumbnail.
     *
     * @param modelFile Absolute path of a supported model file inside the asset root.
     * @return The picture as an ImGui texture (cast it with
     *         `reinterpret_cast<ImTextureID>`), or `VK_NULL_HANDLE` if it is
     *         not ready yet, or never will be (too big, failed to load).
     */
    VkDescriptorSet request(const std::filesystem::path& modelFile);

    /**
     * @brief Moves the queued thumbnails forward: starts loads, uploads
     *        finished models to the GPU.
     *
     * Call once per frame before Renderer::beginFrame(): uploading a model
     * submits GPU work of its own, which must not happen while a frame is
     * being recorded.
     */
    void update();

    /**
     * @brief Draws the pictures that are ready to be drawn.
     *
     * Call while recording the frame, outside any render pass. The pictures
     * show up in the UI from the next frame on.
     */
    void render(VkCommandBuffer cmd);

private:
    /// Where a thumbnail is on its way to being drawn.
    enum class State {
        Queued,       ///< Asked for; not started.
        Loading,      ///< The model is being read or uploaded.
        Ready,        ///< The model is on the GPU; the picture is not drawn yet.
        Drawn,        ///< The picture exists and can be shown.
        Unavailable   ///< Will never exist (too big, failed, over the limit).
    };

    struct Entry {
        State                            state = State::Queued;
        std::filesystem::path            file;
        AssetHandle                      handle = 0;
        std::unique_ptr<OffscreenBuffer> picture;
        std::uint64_t                    lastRequested = 0;   ///< Frame of the last request().
    };

    const VulkanContext* m_ctx    = nullptr;
    SceneRenderer*       m_scenes = nullptr;

    std::unordered_map<std::string, Entry> m_entries;   ///< By UTF-8 path.
    std::vector<std::string>               m_pending;   ///< Keys still on their way, oldest first.
    std::size_t   m_pictures = 0;                       ///< Pictures allocated so far.
    std::uint64_t m_frame    = 0;                       ///< Counts update() calls.
};

} // namespace fe
