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
 *   - Imported models use their asset id (see AssetDatabase), which has
 *     the top bit set so it can never collide with a built-in handle. The
 *     id is stored next to the file, so it survives moving and renaming the
 *     model, which is what lets saved scenes find their models again.
 *
 * Imported models are read on a background thread, so a large file never
 * freezes the editor. Only the final upload to the GPU happens on the
 * calling thread. While a model is being read its progress can be queried
 * and the import can be cancelled (see importStatuses()).
 */

#include "core/Geometry.h"
#include "gfx/MeshBuffer.h"
#include "gfx/MeshDistanceField.h"
#include "gfx/SdfAtlas.h"
#include "import/AssetDatabase.h"
#include "import/ImportControl.h"
#include "import/MeshData.h"
#include "scene/Components.h"

#include <atomic>
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

/// How an imported model is drawn: one range of indices per material.
struct ModelParts {
    std::vector<MeshPart>      parts;      ///< Never empty for an imported model.
    std::vector<ModelMaterial> materials;  ///< What MeshPart::material refers to.
    std::vector<ImageSource>   images;     ///< What the materials' texture indices refer to.
};

class MeshLibrary {
public:
    /**
     * @brief Uploads all built-in primitives to the GPU.
     *
     * @param ctx        Initialised Vulkan context.
     * @param fieldCells Cells along the longest side of every mesh's distance
     *                   field (see ProjectSettings::meshFieldResolution). Fixed
     *                   from here on: the atlas that holds the fields is sized
     *                   for it.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, uint32_t fieldCells = 64);

    /// Destroys all GPU buffers. Safe to call more than once.
    /// Asks imports still running in the background to stop, and waits for
    /// them for up to one second. An import that has not stopped by then is
    /// left running (see importsStillRunning()) so that closing the editor is
    /// never held up by a big file.
    void shutdown();

    /// @return `true` if shutdown() gave up waiting for an import. Its thread
    ///         is still running, so the program should end with
    ///         std::quick_exit() instead of returning from main().
    bool importsStillRunning() const { return m_importsStillRunning; }

    /**
     * @brief Returns the handle (asset id) of a model file.
     *
     * The AssetDatabase knows where the model is, so nothing is read until it
     * is first used. Safe to call repeatedly with the same file.
     *
     * @param file Path to a supported model file inside the asset root.
     * @return The handle, or fe::builtin::MeshNone if the file is outside
     *         the asset root or not a model.
     */
    AssetHandle registerModel(const std::filesystem::path& file);

    /**
     * @brief Starts a new frame of bookkeeping.
     *
     * Forgets which models the scene used last frame (so importStatuses()
     * only reports models still in use) and releases the memory of imports
     * that were cancelled and have now actually stopped. It also moves the
     * distance fields that finished building to the GPU (see distanceField()).
     * SceneRenderer::prepare() calls it once per frame, before the
     * ensureLoaded() calls.
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
     * @param track `false` for a caller that is not the scene (the thumbnails):
     *              the model is then left out of importStatuses(), so that
     *              loading a model only to draw its preview does not show a
     *              progress bar over the viewport.
     *
     * @return `true` if the mesh can be drawn right now.
     */
    bool ensureLoaded(AssetHandle handle, bool track = true);

    /**
     * @return `true` if @p handle will not load by itself: the import failed,
     *         was cancelled by the user, or the model is unknown.
     */
    bool isUnavailable(AssetHandle handle) const {
        return m_failed.count(handle) != 0 || m_declined.count(handle) != 0;
    }

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
     * @brief Returns the material ranges of an imported model.
     * @return The parts, or `nullptr` for built-in meshes and for models that
     *         are not on the GPU (draw those whole).
     */
    const ModelParts* parts(AssetHandle handle) const;

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

    /// Where the distance field of a mesh is, inside the SdfAtlas.
    struct SdfPlacement {
        glm::vec3  boxMin{0.0f};   ///< Corner of the field, in the mesh's space.
        glm::vec3  boxMax{0.0f};   ///< Opposite corner.
        glm::uvec3 dims{0};        ///< Cells of the field along each axis.
        float      voxelSize = 0.0f;
        uint32_t   atlasZ    = 0;  ///< Where its brick starts along z of the atlas.
        glm::vec3  colour{1.0f};   ///< Average of the mesh's vertex colours: what the reflections show of it, before the material's colour.
    };

    /**
     * @brief Where the distance field of a mesh is in the atlas.
     *
     * The field is built on a background thread once the mesh is on the GPU
     * (the built-in primitives have theirs from the start), so a model has
     * none for a moment after it appears.
     *
     * @return The placement, or an empty optional if the mesh has no field on the GPU yet.
     */
    std::optional<SdfPlacement> distancePlacement(AssetHandle handle) const;

    /// @return The texture that holds the distance fields of all meshes.
    const SdfAtlas& distanceAtlas() const { return m_atlas; }

    /// @return The CPU copy of the distance field (what distanceField() was filled from), or `nullptr`.
    const SdfVolume* distanceVolume(AssetHandle handle) const;

    /// @return How many distance fields are still being built.
    size_t distanceFieldsPending() const { return m_sdfPending.size(); }

private:
    /// A model imported from disk, with its GPU buffers.
    struct ImportedMesh {
        MeshBuffer buffer;
        Aabb       bounds;
        ModelParts parts;
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

    /// A mesh's distance field: the CPU copy, and where it went in the atlas.
    struct DistanceFieldEntry {
        SdfVolume volume;
        uint32_t  atlasZ = SdfAtlas::kNoSlot;
        glm::vec3 colour{1.0f};   ///< Average vertex colour.
    };

    /// A distance field being built on a background thread.
    struct PendingField {
        std::future<SdfVolume>        future;
        glm::vec3                     colour{1.0f};   ///< Average vertex colour.
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    /// Builds the field of a mesh now (small built-in meshes only).
    void buildDistanceFieldNow(AssetHandle handle, const std::vector<Vertex>& vertices,
                               const std::vector<uint32_t>& indices);

    /// Starts building the field of a mesh in the background.
    void buildDistanceFieldLater(AssetHandle handle, const std::vector<Vertex>& vertices,
                                 const std::vector<uint32_t>& indices);

    /// Uploads the fields that have finished building.
    void collectDistanceFields();

    const VulkanContext* m_ctx = nullptr;

    MeshBuffer m_cube;
    MeshBuffer m_plane;
    Aabb       m_cubeBounds;
    Aabb       m_planeBounds;

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

    /// All the distance fields on the GPU.
    SdfAtlas m_atlas;
    uint32_t m_fieldCells = 64;   ///< Cells along the longest side of a mesh's field.

    /// Distance fields ready to be sampled, by mesh handle (built-in meshes included).
    std::unordered_map<AssetHandle, DistanceFieldEntry> m_fields;

    /// Distance fields being built.
    std::unordered_map<AssetHandle, PendingField> m_sdfPending;

    /// Set by shutdown() when it left an import running.
    bool m_importsStillRunning = false;
};

} // namespace fe
