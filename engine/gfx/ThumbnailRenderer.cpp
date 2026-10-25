#include "gfx/ThumbnailRenderer.h"

#include "core/Log.h"
#include "core/PathUtil.h"
#include "gfx/SceneRenderer.h"

#include <algorithm>
#include <system_error>

namespace fe {

ThumbnailRenderer::~ThumbnailRenderer() { shutdown(); }

void ThumbnailRenderer::init(const VulkanContext& ctx, SceneRenderer& scenes) {
    m_ctx    = &ctx;
    m_scenes = &scenes;
}

void ThumbnailRenderer::shutdown() {
    for (auto& [key, entry] : m_entries)
        if (entry.picture) entry.picture->shutdown();

    m_entries.clear();
    m_pending.clear();
    m_pictures = 0;
    m_ctx      = nullptr;
    m_scenes   = nullptr;
}

VkDescriptorSet ThumbnailRenderer::request(const std::filesystem::path& modelFile) {
    if (!m_scenes) return VK_NULL_HANDLE;

    const std::string key = toUtf8(modelFile);

    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        Entry entry;
        entry.file = modelFile;
        it = m_entries.emplace(key, std::move(entry)).first;
        m_pending.push_back(key);
    }

    Entry& entry = it->second;
    entry.lastRequested = m_frame;

    return entry.state == State::Drawn ? entry.picture->imguiDescriptorSet()
                                       : VK_NULL_HANDLE;
}

void ThumbnailRenderer::update() {
    if (!m_scenes) return;
    ++m_frame;

    MeshLibrary& meshes = m_scenes->meshes();

    std::size_t loading = 0;
    std::vector<std::string> stillPending;
    stillPending.reserve(m_pending.size());

    for (const std::string& key : m_pending) {
        Entry& entry = m_entries.at(key);

        // A tile that scrolled out of view stops asking. Forget it, so it
        // can be asked for again (and queued again) when it comes back.
        if (entry.state == State::Queued && entry.lastRequested + 2 < m_frame) {
            m_entries.erase(key);
            continue;
        }

        if (entry.state == State::Queued) {
            if (loading >= kMaxLoading) {
                stillPending.push_back(key);
                continue;
            }

            std::error_code ec;
            const std::uintmax_t bytes = std::filesystem::file_size(entry.file, ec);
            if (ec || bytes > kMaxFileBytes) {
                entry.state = State::Unavailable;   // too big (or unreadable): keep the icon
                continue;
            }

            entry.handle = meshes.registerModel(entry.file);
            if (entry.handle == builtin::MeshNone) {
                entry.state = State::Unavailable;   // outside the asset root
                continue;
            }
            entry.state = State::Loading;
        }

        if (entry.state == State::Loading) {
            ++loading;

            // track = false: a thumbnail load must not show a progress bar
            // over the viewport, as a model the scene uses does.
            if (meshes.ensureLoaded(entry.handle, false)) {
                if (m_pictures >= kMaxThumbnails) {
                    entry.state = State::Unavailable;
                    continue;
                }
                entry.picture = std::make_unique<OffscreenBuffer>();
                if (!entry.picture->init(*m_ctx, kSize, kSize)) {
                    entry.picture.reset();
                    entry.state = State::Unavailable;
                    continue;
                }
                ++m_pictures;
                entry.state = State::Ready;
            } else if (meshes.isUnavailable(entry.handle)) {
                entry.state = State::Unavailable;   // failed, or cancelled by the user
                continue;
            }
        }

        if (entry.state == State::Loading || entry.state == State::Ready)
            stillPending.push_back(key);
    }

    m_pending = std::move(stillPending);
}

void ThumbnailRenderer::render(VkCommandBuffer cmd) {
    if (!m_scenes) return;

    std::size_t drawn = 0;
    for (const std::string& key : m_pending) {
        if (drawn >= kMaxDrawnPerFrame) break;

        Entry& entry = m_entries.at(key);
        if (entry.state != State::Ready) continue;

        if (m_scenes->renderPreview(cmd, entry.handle, *entry.picture)) {
            entry.state = State::Drawn;
        } else {
            entry.picture.reset();   // it was never drawn, so nothing uses it yet
            --m_pictures;
            entry.state = State::Unavailable;
        }
        ++drawn;
    }

    // Drawn and unavailable thumbnails need no more work.
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
        [this](const std::string& key) {
            const State state = m_entries.at(key).state;
            return state == State::Drawn || state == State::Unavailable;
        }), m_pending.end());
}

} // namespace fe
