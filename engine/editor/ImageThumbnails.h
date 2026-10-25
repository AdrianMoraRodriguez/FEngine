#pragma once

/**
 * @file ImageThumbnails.h
 * @brief Small pictures of image files, for the Asset Browser's tiles.
 */

#include "gfx/Texture.h"
#include "import/ImageLoader.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>

namespace fe {

class VulkanContext;

/**
 * @brief Cache of thumbnails of image files (PNG, JPEG, BMP, TGA).
 *
 * ### How a thumbnail arrives
 *  1. request() is called every frame a tile is visible. The first call for
 *     a file starts a background task that decodes it and shrinks it to at
 *     most kSize pixels on its longest side, so a 8K photo costs the GPU
 *     no more than an icon.
 *  2. update(), once per frame on the main thread, uploads finished pictures
 *     (at most kMaxUploadsPerFrame, since an upload waits for the GPU) and
 *     registers them with ImGui.
 *  3. Until then request() returns false and the tile shows its icon.
 *
 * At most kMaxThumbnails pictures are kept: when there are more, the ones
 * that have not been asked for the longest are released.
 *
 * A thumbnail is not refreshed if the file changes on disk while the editor
 * is running.
 *
 * Call shutdown() before ImGui's Vulkan backend is shut down.
 */
class ImageThumbnails {
public:
    static constexpr uint32_t       kSize                = 128;  ///< Longest side, in pixels.
    static constexpr std::size_t    kMaxThumbnails       = 256;
    static constexpr std::size_t    kMaxLoading          = 4;    ///< Decodes running at once.
    static constexpr std::size_t    kMaxUploadsPerFrame  = 2;
    static constexpr std::uintmax_t kMaxFileBytes        = 64ull * 1024 * 1024;

    /// A picture ready to draw.
    struct Picture {
        VkDescriptorSet set    = VK_NULL_HANDLE;
        uint32_t        width  = 0;   ///< Of the thumbnail, not of the file.
        uint32_t        height = 0;
    };

    ImageThumbnails() = default;
    ~ImageThumbnails() { shutdown(); }

    ImageThumbnails(const ImageThumbnails&)            = delete;
    ImageThumbnails& operator=(const ImageThumbnails&) = delete;

    /// @param ctx Vulkan context; must outlive this object.
    void init(const VulkanContext& ctx) { m_ctx = &ctx; }

    /// Frees everything. Waits for the GPU and for decodes in progress. Safe to call twice.
    void shutdown();

    /**
     * @brief Asks for the thumbnail of @p file.
     * @param[out] picture The picture, when this returns true.
     * @return `false` while it is not ready, or if it never will be (too big,
     *         unreadable). Call again next frame.
     */
    bool request(const std::filesystem::path& file, Picture& picture);

    /// Finishes loads that are ready. Call once per frame, main thread only,
    /// outside of frame recording.
    void update();

private:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State                                 state = State::Loading;
        std::future<std::optional<ImageData>> decoding;   ///< Valid while Loading.
        Texture                               texture;    ///< Valid when Ready.
        Picture                               picture;
        uint64_t                              lastUsed = 0;
    };

    /// Releases a ready entry's GPU objects. The GPU must be idle.
    void release(Entry& entry);

    /// Drops the least recently used pictures above kMaxThumbnails.
    void evict();

    const VulkanContext* m_ctx = nullptr;
    uint64_t             m_frame = 0;   ///< Counts update() calls.
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace fe
