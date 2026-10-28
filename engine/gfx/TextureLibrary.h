#pragma once

/**
 * @file TextureLibrary.h
 * @brief Loads the textures that materials ask for and hands out the
 *        descriptor sets (one per material) the mesh shader reads them through.
 */

#include "core/AlphaMode.h"
#include "gfx/Texture.h"
#include "import/ImageLoader.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe {

class VulkanContext;

/**
 * @brief Cache of GPU textures and of the descriptor sets that bundle them
 *        into materials.
 *
 * ### What a material is for the shader
 * Set 1 of the mesh pipeline holds four textures and a small block of numbers:
 *
 * | binding | content                                   | format |
 * |---------|-------------------------------------------|--------|
 * | 0       | albedo (colour)                           | sRGB   |
 * | 1       | metalness (blue) and roughness (green)    | linear |
 * | 2       | tangent-space normal map                  | linear |
 * | 3       | emissive colour                           | sRGB   |
 * | 4       | uniform block: metallic, roughness, normal scale, emissive factor, alpha mode, cutoff, opacity | |
 *
 * A slot without a texture holds a 1x1 neutral one (white, or a flat normal),
 * so the shader never has to ask whether a texture exists: the neutral one
 * leaves the number it is multiplied with unchanged.
 *
 * ### How a texture arrives
 *  1. The renderer calls request() every frame for every material it draws.
 *     The first call that mentions an image starts decoding it on a
 *     background thread; reading and decoding a large PNG takes long enough
 *     to be noticed if it blocked a frame.
 *  2. update(), once per frame on the main thread, collects finished decodes
 *     and uploads them to the GPU (at most a couple per frame, since an
 *     upload waits for the GPU).
 *  3. Until every image of a material is ready, request() returns the plain
 *     default material, so the object is drawn untextured and the textures
 *     "pop in" together. An image that cannot be loaded is replaced by the
 *     neutral one; the reason is logged once.
 *
 * ### How they leave
 * A texture or material nobody has asked for during the last kIdleFrames
 * frames is idle. When the library is getting full (more than three quarters
 * of kMaxTextures or kMaxMaterials) or has just had to refuse a request for
 * lack of room, update() frees every idle material and, once no material uses
 * it, every idle texture. A freed one that is needed again is simply loaded
 * again (and pops in like the first time).
 *
 * That is safe without waiting for the GPU because the renderer asks for the
 * material of everything it draws, on every frame it draws it: something idle
 * for kIdleFrames frames cannot be in any frame still in flight, and no set is
 * kept from one frame to the next.
 */
class TextureLibrary {
public:
    /// Most textures that can be alive at once.
    static constexpr uint32_t kMaxTextures = 512;

    /// Most distinct materials (descriptor sets) that can be alive at once.
    static constexpr uint32_t kMaxMaterials = 1024;

    /// Frames without a request after which a texture or material may be freed.
    /// Many times the number of frames the GPU keeps in flight.
    static constexpr uint64_t kIdleFrames = 120;

    /// The texture slots of a material (the bindings of set 1).
    enum Slot : int { Albedo = 0, MetalRough = 1, Normal = 2, Emissive = 3, SlotCount = 4 };

    /// Metalness and roughness of a material that says nothing about them.
    static constexpr float kDefaultMetallic  = 0.0f;
    static constexpr float kDefaultRoughness = 0.6f;

    /// Names one image for a slot.
    struct TextureRef {
        /// 0 means "no texture". For an asset image this is its asset id
        /// (AssetDatabase); for a model's own image, a key from modelTextureKey().
        uint64_t key = 0;

        /// Where a model's own image comes from, only used the first time the
        /// key is seen. `nullptr` means the key is an asset id.
        const ImageSource* source = nullptr;

        /// 0 loads the image at full size. Otherwise a copy shrunk to fit this
        /// many pixels is loaded (and cached on its own, apart from the full-size
        /// one): the asset thumbnails use it so that a folder of models with
        /// 4K textures does not fill the GPU.
        uint32_t maxSize = 0;
    };

    /// Everything the shader needs to know about one material.
    struct MaterialRequest {
        std::array<TextureRef, SlotCount> textures;
        float     metallic    = kDefaultMetallic;
        float     roughness   = kDefaultRoughness;
        float     normalScale = 1.0f;
        glm::vec3 emissive{0.0f};   ///< Linear colour, multiplies the emissive texture.

        AlphaMode alphaMode   = AlphaMode::Opaque;   ///< How the albedo's alpha is used.
        float     alphaCutoff = 0.5f;                ///< Mask: pixels with less alpha are dropped.
        float     opacity     = 1.0f;                ///< Multiplies the albedo's alpha.

        /// Draw back faces too. Not part of the descriptor set (it is pipeline
        /// state), so it does not tell two materials apart; the renderer reads it.
        bool      doubleSided = false;
    };

    TextureLibrary() = default;
    ~TextureLibrary() = default;

    TextureLibrary(const TextureLibrary&)            = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    /**
     * @brief Creates the descriptor layout, pool, uniform buffer and the
     *        neutral textures and default material.
     * @param ctx Vulkan context; must outlive the library.
     */
    bool init(const VulkanContext& ctx);

    /// Frees everything. The GPU must be idle. Waits for decodes in progress.
    void shutdown();

    /// Layout of the descriptor set returned by request(): the table above.
    /// This is set 1 of the mesh pipeline.
    VkDescriptorSetLayout layout() const { return m_layout; }

    /// The plain default material: no textures, default metalness and roughness.
    VkDescriptorSet fallback() const { return m_defaultSet; }

    /**
     * @brief Descriptor set to draw with for the material @p request.
     *
     * Starts loading the images the first time they are asked for. Never
     * returns null: until the material is ready it is fallback().
     */
    VkDescriptorSet request(const MaterialRequest& request);

    /// Finishes loads that are ready. Call once per frame, main thread only.
    void update();

    /// @return How many times request() had to hand out the default material
    ///         because an image was still loading. A caller that wants to know
    ///         whether a picture it just drew is final compares this number
    ///         before and after.
    uint64_t waitingRequests() const { return m_waitingRequests; }

    /// @return The number of textures loaded or loading (the limit is kMaxTextures).
    uint32_t textureCount() const { return m_textureCount; }

    /// @return The number of materials alive, not counting the default one (the limit is kMaxMaterials).
    uint32_t materialCount() const { return static_cast<uint32_t>(m_materials.size()); }

    /// @return A key for image number @p image of the model @p model. It never
    ///         equals an asset id (those have the top bit set; this does not).
    static uint64_t modelTextureKey(uint64_t model, uint32_t image);

private:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State                                 state = State::Loading;
        std::future<std::optional<ImageData>> decoding;   ///< Valid while Loading.
        Texture                               texture;    ///< Valid when Ready.
        uint64_t                              lastUsed = 0;   ///< Frame of the last request that looked it up.
    };

    using EntryMap = std::unordered_map<uint64_t, Entry>;

    /// The identity of a material: its textures and its numbers.
    struct MaterialKey {
        std::array<uint64_t, SlotCount> textures{};
        std::array<float, 9>            numbers{};   ///< metallic, roughness, normal scale, emissive rgb, alpha mode, cutoff, opacity.
        bool operator==(const MaterialKey& other) const {
            return textures == other.textures && numbers == other.numbers;
        }
    };
    struct MaterialKeyHash {
        size_t operator()(const MaterialKey& key) const;
    };

    /// The block at binding 4, as the shader reads it (std140).
    struct MaterialBlock {
        glm::vec4 params;     ///< x: metallic. y: roughness. z: normal scale.
        glm::vec4 emissive;   ///< rgb: emissive factor.
        glm::vec4 alpha;      ///< x: alpha mode (0 opaque, 1 mask, 2 blend). y: mask cutoff. z: opacity.
    };

    /// The key a texture is cached under: its own key, mixed with the size limit if it has one.
    static uint64_t cacheKey(const TextureRef& ref);

    /// Whether @p slot holds colour (sRGB) or data (linear).
    static bool isColorSlot(int slot) { return slot == Albedo || slot == Emissive; }

    /// @return The entry for @p ref in the right map, starting its decode the
    ///         first time. `nullptr` if there is no room for another texture.
    Entry* entryFor(int slot, const TextureRef& ref);

    /// A built material: its set, which block of the uniform buffer it owns,
    /// and when it was last asked for.
    struct MaterialInfo {
        VkDescriptorSet set       = VK_NULL_HANDLE;
        uint32_t        block     = 0;
        uint64_t        lastUsed  = 0;
    };

    /// Allocates a set for @p request, once all its images are settled.
    /// @return The set and its block, or a null set if there is no room left.
    MaterialInfo createMaterialSet(const MaterialRequest& request,
                                   const std::array<const Texture*, SlotCount>& ready);

    /// Frees the idle materials and then the idle textures nothing uses.
    void evictIdle();

    const VulkanContext* m_ctx = nullptr;

    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool   = VK_NULL_HANDLE;

    Texture m_white;          ///< sRGB white: neutral albedo and emissive.
    Texture m_whiteLinear;    ///< Linear white: neutral metalness/roughness.
    Texture m_flatNormal;     ///< Linear (0.5, 0.5, 1): a normal map that changes nothing.

    VkBuffer      m_blockBuffer     = VK_NULL_HANDLE;   ///< One MaterialBlock per set.
    VmaAllocation m_blockAllocation = nullptr;
    uint8_t*      m_blockMapped     = nullptr;

    VkDescriptorSet m_defaultSet = VK_NULL_HANDLE;
    uint32_t        m_nextBlock     = 1;   ///< First block never handed out (0 is the default material's).
    std::vector<uint32_t> m_freeBlocks;    ///< Blocks of freed materials, ready to be reused.
    uint32_t        m_textureCount  = 0;
    uint64_t        m_frame         = 0;   ///< Counts update() calls.
    uint64_t        m_nextEvictFrame = 0;  ///< No eviction pass before this frame.
    bool            m_pressure      = false;   ///< A request was refused for lack of room.
    uint64_t        m_waitingRequests = 0;
    bool            m_warnedTextures  = false;

    EntryMap m_colorTextures;    ///< Uploaded as sRGB.
    EntryMap m_dataTextures;     ///< Uploaded as linear.

    std::unordered_map<MaterialKey, MaterialInfo, MaterialKeyHash> m_materials;
};

} // namespace fe
