#include "gfx/TextureLibrary.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"
#include "import/AssetDatabase.h"

#include <chrono>
#include <cstring>
#include <functional>

namespace fe {
namespace {

/// Most textures uploaded in one update(). Each upload waits for the GPU.
constexpr int kMaxUploadsPerFrame = 2;

/// Distance between the blocks in the uniform buffer. 256 is the largest
/// alignment Vulkan allows a device to ask for, so it is valid everywhere.
constexpr VkDeviceSize kBlockStride = 256;

constexpr uint32_t kBindingBlock = 4;   // after the four textures

template <typename T>
bool isReady(const std::future<T>& future) {
    return future.valid() &&
           future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

ImageData onePixel(uint8_t r, uint8_t g, uint8_t b) {
    ImageData image;
    image.width  = 1;
    image.height = 1;
    image.pixels = {r, g, b, 255};
    return image;
}

} // namespace

size_t TextureLibrary::MaterialKeyHash::operator()(const MaterialKey& key) const {
    size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= static_cast<size_t>(value) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    };
    for (uint64_t texture : key.textures) mix(texture);
    for (float number : key.numbers) {
        uint32_t bits;
        std::memcpy(&bits, &number, sizeof(bits));
        mix(bits);
    }
    return hash;
}

bool TextureLibrary::init(const VulkanContext& ctx) {
    m_ctx = &ctx;

    // ---- Layout: four textures and one uniform block, read by the fragment shader ----
    VkDescriptorSetLayoutBinding bindings[SlotCount + 1]{};
    for (uint32_t i = 0; i < SlotCount; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[SlotCount].binding         = kBindingBlock;
    bindings[SlotCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[SlotCount].descriptorCount = 1;
    bindings[SlotCount].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = SlotCount + 1;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.device(), &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot create the descriptor set layout");
        return false;
    }

    // ---- Pool: one set per material ----
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = SlotCount * kMaxMaterials;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = kMaxMaterials;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;   // idle materials give their set back
    poolInfo.maxSets       = kMaxMaterials;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot create the descriptor pool");
        shutdown();
        return false;
    }

    // ---- The uniform buffer: one block per material, written once ----
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = kBlockStride * kMaxMaterials;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(ctx.allocator(), &bufferInfo, &allocInfo,
                        &m_blockBuffer, &m_blockAllocation, &mapped) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot create the material uniform buffer");
        shutdown();
        return false;
    }
    m_blockMapped = static_cast<uint8_t*>(mapped.pMappedData);

    // ---- The neutral textures and the default material ----
    if (!m_white.create(ctx, onePixel(255, 255, 255), VK_FORMAT_R8G8B8A8_SRGB, false) ||
        !m_whiteLinear.create(ctx, onePixel(255, 255, 255), VK_FORMAT_R8G8B8A8_UNORM, false) ||
        !m_flatNormal.create(ctx, onePixel(128, 128, 255), VK_FORMAT_R8G8B8A8_UNORM, false)) {
        shutdown();
        return false;
    }

    const MaterialRequest plain;
    m_defaultSet = createMaterialSet(plain, {}).set;   // block 0
    if (m_defaultSet == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    return true;
}

void TextureLibrary::shutdown() {
    // Destroying a future that comes from std::async waits for its task, so
    // no decode outlives the library.
    m_colorTextures.clear();
    m_dataTextures.clear();
    m_materials.clear();

    m_white.destroy();
    m_whiteLinear.destroy();
    m_flatNormal.destroy();

    if (m_ctx) {
        if (m_blockBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_blockBuffer, m_blockAllocation);
        if (m_pool)        vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);   // frees the sets
        if (m_layout)      vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
    }
    m_blockBuffer     = VK_NULL_HANDLE;
    m_blockAllocation = nullptr;
    m_blockMapped     = nullptr;
    m_pool            = VK_NULL_HANDLE;
    m_layout          = VK_NULL_HANDLE;
    m_defaultSet      = VK_NULL_HANDLE;
    m_nextBlock       = 1;
    m_freeBlocks.clear();
    m_textureCount    = 0;
    m_frame           = 0;
    m_nextEvictFrame  = 0;
    m_pressure        = false;
    m_ctx             = nullptr;
}

uint64_t TextureLibrary::modelTextureKey(uint64_t model, uint32_t image) {
    // A 64-bit mix (splitmix64 finaliser) of both numbers, with the top bit
    // cleared so it cannot be an asset id, and never 0.
    uint64_t x = model + 0x9E3779B97F4A7C15ull * (static_cast<uint64_t>(image) + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (x & 0x7FFFFFFFFFFFFFFFull) | 1ull;
}

uint64_t TextureLibrary::cacheKey(const TextureRef& ref) {
    if (ref.maxSize == 0) return ref.key;
    uint64_t x = ref.key ^ (0xD6E8FEB86659FD93ull * (static_cast<uint64_t>(ref.maxSize) + 1));
    x = (x ^ (x >> 32)) * 0xD6E8FEB86659FD93ull;
    x ^= x >> 32;
    return x | 1ull;
}

TextureLibrary::Entry* TextureLibrary::entryFor(int slot, const TextureRef& ref) {
    EntryMap& map = isColorSlot(slot) ? m_colorTextures : m_dataTextures;
    const uint64_t cacheId = cacheKey(ref);

    const auto found = map.find(cacheId);
    if (found != map.end()) {
        found->second.lastUsed = m_frame;
        return &found->second;
    }

    if (m_textureCount >= kMaxTextures) {
        // Full. update() frees what is idle; the caller tries again next frame.
        m_pressure = true;
        return nullptr;
    }

    Entry& entry = map[cacheId];
    entry.lastUsed = m_frame;
    ++m_textureCount;

    if (ref.source) {
        // The task owns a copy of the source (cheap: the bytes are shared).
        entry.decoding = std::async(std::launch::async,
            [source = *ref.source, maxSize = ref.maxSize]() {
                std::optional<ImageData> image = loadImage(source);
                if (image && maxSize) image = shrunkToFit(std::move(*image), maxSize);
                return image;
            });
    } else {
        const std::filesystem::path file = AssetDatabase::instance().fullPathOf(ref.key);
        if (file.empty()) {
            // Deleted since the scene was saved. Remembered, so it is said once.
            FE_WARN("TextureLibrary: no image asset has id %llx (was it deleted?)",
                    static_cast<unsigned long long>(ref.key));
            entry.state = State::Failed;
        } else {
            entry.decoding = std::async(std::launch::async,
                [file, maxSize = ref.maxSize]() {
                    std::optional<ImageData> image = loadImage(file);
                    if (image && maxSize) image = shrunkToFit(std::move(*image), maxSize);
                    return image;
                });
        }
    }
    return &entry;
}

TextureLibrary::MaterialInfo TextureLibrary::createMaterialSet(
        const MaterialRequest& request, const std::array<const Texture*, SlotCount>& ready) {
    // A block of the uniform buffer: one a freed material left, or a new one.
    // (The default material, made first, takes block 0.)
    uint32_t block = 0;
    if (!m_freeBlocks.empty()) {
        block = m_freeBlocks.back();
    } else if (m_nextBlock < kMaxMaterials || m_defaultSet == VK_NULL_HANDLE) {
        block = (m_defaultSet == VK_NULL_HANDLE) ? 0 : m_nextBlock;
    } else {
        m_pressure = true;   // full: update() frees what is idle
        return {};
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &set) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot allocate a descriptor set");
        return {};
    }

    // Only now that nothing can fail is the block taken.
    if (!m_freeBlocks.empty() && m_defaultSet != VK_NULL_HANDLE) m_freeBlocks.pop_back();
    else if (m_defaultSet != VK_NULL_HANDLE)                      ++m_nextBlock;

    // This material's block. Written when the material is built and not again
    // until it is freed (and then only after it has been idle for a long time).
    const VkDeviceSize offset = kBlockStride * block;
    const MaterialBlock blockData{
        glm::vec4(request.metallic, request.roughness, request.normalScale, 0.0f),
        glm::vec4(request.emissive, 0.0f),
        glm::vec4(static_cast<float>(request.alphaMode), request.alphaCutoff, request.opacity, 0.0f)};
    std::memcpy(m_blockMapped + offset, &blockData, sizeof(blockData));
    vmaFlushAllocation(m_ctx->allocator(), m_blockAllocation, offset, sizeof(blockData));

    // A slot without a ready texture gets the neutral one.
    const Texture* neutral[SlotCount] = {&m_white, &m_whiteLinear, &m_flatNormal, &m_white};

    VkDescriptorImageInfo images[SlotCount];
    for (int i = 0; i < SlotCount; ++i)
        images[i] = (ready[i] ? ready[i] : neutral[i])->descriptorInfo();

    const VkDescriptorBufferInfo blockInfo{m_blockBuffer, offset, sizeof(MaterialBlock)};

    VkWriteDescriptorSet writes[SlotCount + 1]{};
    for (uint32_t i = 0; i < SlotCount; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &images[i];
    }
    writes[SlotCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[SlotCount].dstSet          = set;
    writes[SlotCount].dstBinding      = kBindingBlock;
    writes[SlotCount].descriptorCount = 1;
    writes[SlotCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[SlotCount].pBufferInfo     = &blockInfo;
    vkUpdateDescriptorSets(m_ctx->device(), SlotCount + 1, writes, 0, nullptr);
    return {set, block, m_frame};
}

VkDescriptorSet TextureLibrary::request(const MaterialRequest& request) {
    MaterialKey key;
    for (int i = 0; i < SlotCount; ++i) key.textures[i] = request.textures[i].key ? cacheKey(request.textures[i]) : 0;
    key.numbers = {request.metallic, request.roughness, request.normalScale,
                   request.emissive.r, request.emissive.g, request.emissive.b,
                   static_cast<float>(request.alphaMode), request.alphaCutoff, request.opacity};

    if (const auto found = m_materials.find(key); found != m_materials.end()) {
        found->second.lastUsed = m_frame;
        return found->second.set;
    }

    // Not built yet: are all its images settled?
    std::array<const Texture*, SlotCount> ready{};
    bool loading = false;

    for (int i = 0; i < SlotCount; ++i) {
        if (request.textures[i].key == 0) continue;

        const Entry* entry = entryFor(i, request.textures[i]);
        if (!entry)                              { loading = true; continue; }   // no room yet: try again next frame
        if (entry->state == State::Loading)      loading = true;
        else if (entry->state == State::Ready)   ready[i] = &entry->texture;
        // Failed: neutral texture
    }
    if (loading) {
        ++m_waitingRequests;
        return m_defaultSet;
    }

    const MaterialInfo info = createMaterialSet(request, ready);
    if (info.set == VK_NULL_HANDLE) {
        ++m_waitingRequests;
        return m_defaultSet;   // no room yet: not remembered, so it is tried again
    }

    m_materials.emplace(key, info);
    return info.set;
}

void TextureLibrary::evictIdle() {
    const uint64_t now = m_frame;
    const auto idle = [now](uint64_t lastUsed) { return now - lastUsed >= kIdleFrames; };

    // 1. Materials: give back the idle ones.
    uint32_t freedMaterials = 0;
    for (auto it = m_materials.begin(); it != m_materials.end();) {
        if (idle(it->second.lastUsed)) {
            vkFreeDescriptorSets(m_ctx->device(), m_pool, 1, &it->second.set);
            m_freeBlocks.push_back(it->second.block);
            it = m_materials.erase(it);
            ++freedMaterials;
        } else {
            ++it;
        }
    }

    // 2. Textures: which are still used by a material that stays?
    std::unordered_map<uint64_t, bool> usedColor, usedData;
    for (const auto& [key, info] : m_materials) {
        for (int slot = 0; slot < SlotCount; ++slot)
            if (key.textures[slot]) (isColorSlot(slot) ? usedColor : usedData)[key.textures[slot]] = true;
    }

    // 3. Free the idle ones nothing uses. One still loading is left alone
    //    (its decode runs on another thread and is waited for when it is
    //    destroyed); it is looked at again on a later pass.
    uint32_t freedTextures = 0;
    for (EntryMap* map : {&m_colorTextures, &m_dataTextures}) {
        const auto& used = (map == &m_colorTextures) ? usedColor : usedData;
        for (auto it = map->begin(); it != map->end();) {
            if (it->second.state != State::Loading && idle(it->second.lastUsed) && !used.count(it->first)) {
                it = map->erase(it);   // the Texture's destructor frees the GPU image
                --m_textureCount;
                ++freedTextures;
            } else {
                ++it;
            }
        }
    }

    if (freedMaterials || freedTextures)
        FE_INFO("TextureLibrary: freed %u idle material(s) and %u texture(s); %u textures and %u materials left",
                freedMaterials, freedTextures, m_textureCount, static_cast<uint32_t>(m_materials.size()));
}

void TextureLibrary::update() {
    ++m_frame;

    // Free what is idle when the library fills up (or has refused a request),
    // at most once a second or so: a pass looks at every material.
    const bool crowded = m_textureCount > kMaxTextures * 3 / 4 ||
                         m_materials.size() > kMaxMaterials * 3 / 4;
    if ((crowded || m_pressure) && m_frame >= m_nextEvictFrame) {
        evictIdle();
        m_nextEvictFrame = m_frame + 60;

        // Still no room after freeing everything idle: all of it is in use.
        if (m_pressure && (m_textureCount >= kMaxTextures || m_materials.size() + 1 >= kMaxMaterials) &&
            !m_warnedTextures) {
            FE_WARN("TextureLibrary: more than %u textures or %u materials in use at once; "
                    "the objects that do not fit are drawn untextured", kMaxTextures, kMaxMaterials);
            m_warnedTextures = true;
        }
        m_pressure = false;
    }

    int uploads = 0;

    for (EntryMap* map : {&m_colorTextures, &m_dataTextures}) {
        const VkFormat format = (map == &m_colorTextures) ? VK_FORMAT_R8G8B8A8_SRGB
                                                          : VK_FORMAT_R8G8B8A8_UNORM;

        for (auto& [key, entry] : *map) {
            if (entry.state != State::Loading) continue;
            if (!isReady(entry.decoding)) continue;
            if (uploads >= kMaxUploadsPerFrame) return;   // the rest wait for the next frame

            std::optional<ImageData> image;
            try {
                image = entry.decoding.get();
            } catch (const std::exception& e) {
                FE_ERROR("TextureLibrary: texture %llx: %s",
                         static_cast<unsigned long long>(key), e.what());
            }

            // loadImage / Texture already logged why a failure failed.
            entry.state = (image && entry.texture.create(*m_ctx, *image, format, true))
                              ? State::Ready : State::Failed;
            ++uploads;
        }
    }
}

} // namespace fe
