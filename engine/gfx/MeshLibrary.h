#pragma once

/**
 * @file MeshLibrary.h
 * @brief GPU meshes: the built-in primitives and the models imported from disk.
 *
 * Temporary stand-in for a real asset manager. It resolves the mesh handle
 * of a MeshRenderer to the buffer that must be drawn.
 *
 * Two kinds of handle exist:
 *   - Built-in primitives use the small fixed values of fe::builtin.
 *   - Imported models use the hash of their path relative to the asset
 *     root, with the top bit set so it can never collide with a built-in
 *     handle. The same file always gets the same handle, which is what lets
 *     saved scenes find their models again.
 *
 * Imported models are read on a background thread, so a large file never
 * freezes the editor. Only the final upload to the GPU happens on the
 * calling thread. While a model is being read its progress can be queried
 * and the import can be cancelled (see importStatuses()).
 */

#include "core/Geometry.h"
#include "gfx/MeshBuffer.h"
#include "import/ImportControl.h"
#include "import/MeshData.h"
#include "scene/Components.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fe {

class VulkanContext;

/// What is happening to a model the scene uses but that is not on the GPU.
enum class ImportState {
    Loading,   ///< Being read on a background thread.
    Declined,  ///< The user cancelled it. Not retried by itself.
    Failed     ///< Could not be imported. The reason is in the log.
};

/// Describes one model that needs the user's attention. See MeshLibrary::importStatuses().
struct ImportStatus {
    AssetHandle   handle   = 0;
    ImportState   state    = ImportState::Loading;
    std::string   name;              ///< File name, UTF-8.
    float         progress = 0.0f;   ///< 0 to 1; only meaningful while Loading.
    std::uintmax_t bytes   = 0;      ///< Size of the file on disk, 0 if unknown.
};

class MeshLibrary {
public:
    /**
     * @brief Uploads all built-in primitives to the GPU.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx);

    /// Destroys all GPU buffers. Safe to call more than once.
    /// Asks imports still running in the background to stop, and waits for them.
    void shutdown();

    /// Sets the directory that model paths are relative to (normally `assets/`).
    void setAssetRoot(std::filesystem::path root);

    /**
     * @brief Registers every supported model file (see isSupportedModel())
     *        under the asset root.
     *
     * Needed at startup so that a scene that references a model by handle
     * can find the file again. Registering only records where the file is:
     * nothing is read until the model is first used.
     */
    void scanAssets();

    /**
     * @brief Registers one model file and returns its handle.
     *
     * Safe to call repeatedly with the same file.
     *
     * @param file Path to a supported model file inside the asset root.
     * @return The handle, or fe::builtin::MeshNone if the file is outside
     *         the asset root.
     */
    AssetHandle registerModel(const std::filesystem::path& file);

    /**
     * @brief Starts a new frame of bookkeeping.
     *
     * Forgets which models the scene used last frame (so importStatuses()
     * only reports models still in use) and releases the memory of imports
     * that were cancelled and have now actually stopped. SceneRenderer::
     * prepare() calls it once per frame, before the ensureLoaded() calls.
     */
    void beginFrame();

    /**
     * @brief Makes progress towards having the mesh behind @p handle on the GPU.
     *
     * Built-in handles are always ready. For an imported model, the first
     * call starts reading the file in the background and returns `false`;
     * later calls check on it, and the call that finds it finished uploads
     * it to the GPU. A model that fails to load is remembered, so the
     * failure is reported once and not every frame.
     *
     * A model whose import the user cancelled does not start again by
     * itself: it waits until confirmImport() is called for it.
     *
     * Call it once per frame for every mesh the scene uses (SceneRenderer::
     * prepare() does), and not while recording a frame: the upload submits
     * its own work to the GPU.
     *
     * @return `true` if the mesh can be drawn right now.
     */
    bool ensureLoaded(AssetHandle handle);

    /**
     * @brief Lists the models used last frame that are not ready.
     *
     * Meant for the UI: it shows progress bars, cancelled imports and
     * failures. Sorted by file name so the list does not jump around.
     * Models that are loaded, built in, or not used by the scene are not
     * reported.
     */
    std::vector<ImportStatus> importStatuses() const;

    /// Retries a model whose import was cancelled.
    void confirmImport(AssetHandle handle);

    /**
     * @brief Cancels a running import.
     *
     * The model stays registered and can be imported later with
     * confirmImport(). A running import is asked to stop; the editor does
     * not wait for it. Its memory is released when the thread actually ends.
     */
    void cancelImport(AssetHandle handle);

    /**
     * @brief Resolves a mesh handle to its GPU buffer.
     *
     * @param handle A MeshRenderer::mesh value.
     * @return The buffer, or `nullptr` for fe::builtin::MeshNone and for
     *         meshes that are unknown, still importing, or failed (the
     *         entity is then not drawn).
     */
    const MeshBuffer* get(AssetHandle handle) const;

    /**
     * @brief Returns the local-space bounding box of a mesh.
     *
     * Computed from the vertex data when the mesh is uploaded, and padded
     * slightly so that flat meshes still have some thickness and can be
     * hit by a picking ray.
     *
     * @return The box, or an empty optional if the mesh is not available.
     */
    std::optional<Aabb> bounds(AssetHandle handle) const;

private:
    /// A model imported from disk, with its GPU buffers.
    struct ImportedMesh {
        MeshBuffer buffer;
        Aabb       bounds;
    };

    /// A model being read on a background thread.
    struct PendingImport {
        std::future<std::optional<MeshData>>  future;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<ImportControl>        control;
        std::uintmax_t                        bytes = 0;
    };

    /// An import that was cancelled but whose thread has not stopped yet.
    /// Kept only so its memory is released at the right time.
    struct AbandonedImport {
        std::future<std::optional<MeshData>> future;
        std::shared_ptr<ImportControl>       control;
    };

    const VulkanContext* m_ctx = nullptr;

    MeshBuffer m_cube;
    MeshBuffer m_plane;
    Aabb       m_cubeBounds;
    Aabb       m_planeBounds;

    std::filesystem::path m_assetRoot;

    /// Where each registered model lives, relative to the asset root.
    std::unordered_map<AssetHandle, std::filesystem::path> m_sources;

    /// Models already on the GPU.
    std::unordered_map<AssetHandle, ImportedMesh> m_imported;

    /// Models currently being read.
    std::unordered_map<AssetHandle, PendingImport> m_pending;

    /// Cancelled imports still winding down.
    std::vector<AbandonedImport> m_abandoned;

    /// Models whose import the user cancelled.
    std::unordered_set<AssetHandle> m_declined;

    /// Models that failed to load. They are not retried.
    std::unordered_set<AssetHandle> m_failed;

    /// Imported models that ensureLoaded() was asked about since beginFrame().
    std::unordered_set<AssetHandle> m_used;
};

} // namespace fe
