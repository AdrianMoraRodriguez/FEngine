#include "editor/ImageThumbnails.h"

#include "core/Log.h"
#include "core/PathUtil.h"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace fe {

namespace {

/// Shrinks @p src so its longest side is at most @p maxSide, averaging the
/// source pixels each new pixel covers (a box filter). Smaller images are
/// returned unchanged.
ImageData shrink(ImageData src, uint32_t maxSide) {
    const uint32_t longest = std::max(src.width, src.height);
    if (longest <= maxSide) return src;

    ImageData dst;
    dst.width  = std::max<uint32_t>(1, static_cast<uint32_t>(
        static_cast<uint64_t>(src.width)  * maxSide / longest));
    dst.height = std::max<uint32_t>(1, static_cast<uint32_t>(
        static_cast<uint64_t>(src.height) * maxSide / longest));
    dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * 4);

    for (uint32_t y = 0; y < dst.height; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y)     * src.height / dst.height);
        const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * src.height / dst.height));
        for (uint32_t x = 0; x < dst.width; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x)     * src.width / dst.width);
            const uint32_t x1 = std::max(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * src.width / dst.width));

            uint64_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* row = &src.pixels[(static_cast<size_t>(sy) * src.width + x0) * 4];
                for (uint32_t sx = x0; sx < x1; ++sx, row += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += row[c];
            }
            const uint64_t count = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
            uint8_t* out = &dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4];
            for (int c = 0; c < 4; ++c) out[c] = static_cast<uint8_t>(sum[c] / count);
        }
    }
    return dst;
}

} // namespace

void ImageThumbnails::release(Entry& entry) {
    if (entry.picture.set) {
        ImGui_ImplVulkan_RemoveTexture(entry.picture.set);
        entry.picture.set = VK_NULL_HANDLE;
    }
    entry.texture.destroy();
}

void ImageThumbnails::shutdown() {
    // Nothing to free: either it never held a picture, or shutdown() already ran.
    // The destructor calls this again after the Vulkan device is gone, so it
    // must not touch the device then.
    if (!m_ctx || m_entries.empty()) return;

    // A decode in progress owns nothing the GPU uses; just let it finish.
    for (auto& [key, entry] : m_entries)
        if (entry.state == State::Loading && entry.decoding.valid()) entry.decoding.wait();

    vkDeviceWaitIdle(m_ctx->device());
    for (auto& [key, entry] : m_entries) release(entry);
    m_entries.clear();
}

bool ImageThumbnails::request(const std::filesystem::path& file, Picture& picture) {
    if (!m_ctx) return false;

    const std::string key = toUtf8(file);
    auto it = m_entries.find(key);

    if (it == m_entries.end()) {
        // Do not start more decodes than kMaxLoading: a folder with
        // hundreds of photos would otherwise start hundreds of threads.
        std::size_t loading = 0;
        for (const auto& [k, e] : m_entries)
            if (e.state == State::Loading) ++loading;
        if (loading >= kMaxLoading) return false;

        Entry entry;
        entry.lastUsed = m_frame;

        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
        if (ec || bytes > kMaxFileBytes) {
            entry.state = State::Failed;
        } else {
            entry.decoding = std::async(std::launch::async, [file]() -> std::optional<ImageData> {
                std::optional<ImageData> image = loadImage(file);
                if (!image) return std::nullopt;
                return shrink(std::move(*image), kSize);
            });
        }
        it = m_entries.emplace(key, std::move(entry)).first;
    }

    Entry& entry = it->second;
    entry.lastUsed = m_frame;

    if (entry.state != State::Ready) return false;
    picture = entry.picture;
    return true;
}

void ImageThumbnails::update() {
    if (!m_ctx) return;
    ++m_frame;

    std::size_t uploads = 0;
    for (auto& [key, entry] : m_entries) {
        if (entry.state != State::Loading) continue;
        if (uploads >= kMaxUploadsPerFrame) break;
        if (entry.decoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;

        std::optional<ImageData> image = entry.decoding.get();
        if (!image || image->pixels.empty()) {
            entry.state = State::Failed;
            continue;
        }

        // Already shrunk, so no mip chain; UNORM because ImGui shows the
        // values as they are.
        if (!entry.texture.create(*m_ctx, *image, VK_FORMAT_R8G8B8A8_UNORM, false)) {
            entry.state = State::Failed;
            continue;
        }
        entry.picture.set = ImGui_ImplVulkan_AddTexture(
            entry.texture.sampler(), entry.texture.view(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!entry.picture.set) {
            entry.texture.destroy();
            entry.state = State::Failed;
            continue;
        }
        entry.picture.width  = image->width;
        entry.picture.height = image->height;
        entry.state = State::Ready;
        ++uploads;
    }

    evict();
}

void ImageThumbnails::evict() {
    if (m_entries.size() <= kMaxThumbnails) return;

    // Oldest first, among the entries nothing is waiting on.
    std::vector<std::unordered_map<std::string, Entry>::iterator> candidates;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (it->second.state != State::Loading && it->second.lastUsed + 1 < m_frame)
            candidates.push_back(it);
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a->second.lastUsed < b->second.lastUsed; });

    const std::size_t excess = m_entries.size() - kMaxThumbnails;
    const std::size_t count  = std::min(excess, candidates.size());
    if (count == 0) return;

    // The previous frames may still be drawing these pictures.
    vkDeviceWaitIdle(m_ctx->device());
    for (std::size_t i = 0; i < count; ++i) {
        release(candidates[i]->second);
        m_entries.erase(candidates[i]);
    }
}

} // namespace fe
