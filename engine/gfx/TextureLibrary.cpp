#include "gfx/TextureLibrary.h"

#include "gfx/VulkanContext.h"
#include "core/AssetPaths.h"
#include "core/Log.h"

#include <chrono>

namespace fe {
namespace {

/// Most textures uploaded in one update(). Each upload waits for the GPU.
constexpr int kMaxUploadsPerFrame = 2;

template <typename T>
bool isReady(const std::future<T>& future) {
    return future.valid() &&
           future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

} // namespace

bool TextureLibrary::init(const VulkanContext& ctx) {
    m_ctx = &ctx;

    // ---- Layout: one texture, read by the fragment shader ----
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot create the descriptor set layout");
        return false;
    }

    // ---- Pool: one set per texture, plus the fallback ----
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kMaxTextures + 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = kMaxTextures + 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot create the descriptor pool");
        shutdown();
        return false;
    }

    // ---- The white fallback ----
    ImageData white;
    white.width  = 1;
    white.height = 1;
    white.pixels = {255, 255, 255, 255};
    if (!m_white.create(ctx, white, VK_FORMAT_R8G8B8A8_SRGB, false)) {
        shutdown();
        return false;
    }
    m_fallbackSet = makeSet(m_white);
    if (m_fallbackSet == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    return true;
}

void TextureLibrary::shutdown() {
    // Destroying a future that comes from std::async waits for its task, so
    // no decode outlives the library.
    m_entries.clear();
    m_white.destroy();

    if (m_ctx) {
        if (m_pool)   vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);      // frees the sets
        if (m_layout) vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
    }
    m_pool        = VK_NULL_HANDLE;
    m_layout      = VK_NULL_HANDLE;
    m_fallbackSet = VK_NULL_HANDLE;
    m_setsUsed    = 0;
    m_ctx         = nullptr;
}

VkDescriptorSet TextureLibrary::makeSet(const Texture& texture) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &set) != VK_SUCCESS) {
        FE_ERROR("TextureLibrary: cannot allocate a descriptor set");
        return VK_NULL_HANDLE;
    }
    ++m_setsUsed;

    const VkDescriptorImageInfo imageInfo = texture.descriptorInfo();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imageInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    return set;
}

VkDescriptorSet TextureLibrary::request(const std::string& relativePath) {
    if (relativePath.empty()) return m_fallbackSet;

    auto found = m_entries.find(relativePath);
    if (found == m_entries.end()) {
        if (m_setsUsed >= kMaxTextures + 1) {
            if (!m_warnedFull) {
                FE_WARN("TextureLibrary: more than %u textures; the rest are drawn white", kMaxTextures);
                m_warnedFull = true;
            }
            return m_fallbackSet;
        }

        Entry entry;
        entry.decoding = std::async(std::launch::async,
            [relativePath]() { return loadImage(assets::resolve(relativePath)); });
        found = m_entries.emplace(relativePath, std::move(entry)).first;
    }

    const Entry& entry = found->second;
    return entry.state == State::Ready ? entry.set : m_fallbackSet;
}

void TextureLibrary::update() {
    int uploads = 0;

    for (auto& [path, entry] : m_entries) {
        if (entry.state != State::Loading) continue;
        if (!isReady(entry.decoding)) continue;
        if (uploads >= kMaxUploadsPerFrame) break;   // the rest wait for the next frame

        std::optional<ImageData> image;
        try {
            image = entry.decoding.get();
        } catch (const std::exception& e) {
            FE_ERROR("TextureLibrary: '%s': %s", path.c_str(), e.what());
        }

        if (!image || !entry.texture.create(*m_ctx, *image, VK_FORMAT_R8G8B8A8_SRGB, true)) {
            entry.state = State::Failed;   // loadImage / Texture already logged why
            continue;
        }

        entry.set = makeSet(entry.texture);
        entry.state = (entry.set != VK_NULL_HANDLE) ? State::Ready : State::Failed;
        ++uploads;
    }
}

} // namespace fe
