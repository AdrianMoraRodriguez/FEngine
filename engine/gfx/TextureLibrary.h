#pragma once

/**
 * @file TextureLibrary.h
 * @brief Loads the textures that materials ask for and hands out the
 *        descriptor sets the shader reads them through.
 */

#include "gfx/Texture.h"
#include "import/ImageLoader.h"

#include <vulkan/vulkan.h>

#include <future>
#include <optional>
#include <string>
#include <unordered_map>

namespace fe {

class VulkanContext;

/**
 * @brief Cache of GPU textures, keyed by their asset-relative path.
 *
 * ### How a texture arrives
 *  1. The renderer calls request() every frame for every textured object.
 *     The first call for a path starts decoding the file on a background
 *     thread; reading and decoding a large PNG takes long enough to be
 *     noticed if it blocked a frame.
 *  2. update(), once per frame on the main thread, collects finished
 *     decodes and uploads them to the GPU (at most a couple per frame, since
 *     an upload waits for the GPU).
 *  3. Until then, request() returns the descriptor of a 1x1 white texture,
 *     so the object is drawn with its plain colour and the texture "pops
 *     in" when it is ready. A file that cannot be loaded stays white; the
 *     reason is logged once.
 *
 * Every texture is uploaded as sRGB colour data (albedo). Other uses
 * (normal maps, roughness...) will need their own format in the key.
 *
 * Textures are never evicted yet: they live until shutdown().
 */
class TextureLibrary {
public:
    /// Most textures that can be alive at once (size of the descriptor pool).
    static constexpr uint32_t kMaxTextures = 512;

    TextureLibrary() = default;
    ~TextureLibrary() = default;

    TextureLibrary(const TextureLibrary&)            = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    /**
     * @brief Creates the descriptor layout and pool and the white fallback.
     * @param ctx Vulkan context; must outlive the library.
     */
    bool init(const VulkanContext& ctx);

    /// Frees everything. The GPU must be idle. Waits for decodes in progress.
    void shutdown();

    /// Layout of the descriptor set returned by request(): one combined image
    /// sampler at binding 0, visible to the fragment stage. This is set 1 of
    /// the mesh pipeline.
    VkDescriptorSetLayout layout() const { return m_layout; }

    /// Descriptor set of the 1x1 white texture, for objects without one.
    VkDescriptorSet fallback() const { return m_fallbackSet; }

    /**
     * @brief Descriptor set to draw with for the texture at @p relativePath.
     *
     * Starts loading the file the first time it is asked for. Never returns
     * null: until the texture is ready, or if it failed, it is the fallback.
     *
     * @param relativePath Path relative to the asset folder (see assets::resolve).
     */
    VkDescriptorSet request(const std::string& relativePath);

    /// Finishes loads that are ready. Call once per frame, main thread only.
    void update();

private:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State                                 state = State::Loading;
        std::future<std::optional<ImageData>> decoding;   ///< Valid while Loading.
        Texture                               texture;    ///< Valid when Ready.
        VkDescriptorSet                       set = VK_NULL_HANDLE;
    };

    /// Allocates a set from the pool and points it at @p texture.
    VkDescriptorSet makeSet(const Texture& texture);

    const VulkanContext* m_ctx           = nullptr;
    VkDescriptorSetLayout m_layout       = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool         = VK_NULL_HANDLE;
    Texture               m_white;
    VkDescriptorSet       m_fallbackSet  = VK_NULL_HANDLE;
    uint32_t              m_setsUsed     = 0;
    bool                  m_warnedFull   = false;

    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace fe
