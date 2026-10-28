#include "gfx/MeshLibrary.h"

#include "gfx/Vertex.h"
#include "import/ModelImporter.h"
#include "core/Log.h"
#include "core/PathUtil.h"

#include <algorithm>
#include <thread>
#include <exception>
#include <string>
#include <vector>

namespace fe {
namespace {

/// Top bit of an imported model's handle. Built-in handles are small numbers,
/// so this bit tells the two kinds apart at a glance.
constexpr AssetHandle kImportedBit = AssetDatabase::kIdBit;

/// Where a model is now, for log messages.
std::string nameOf(AssetHandle handle) {
    std::string path = AssetDatabase::instance().pathOf(handle);
    return path.empty() ? std::string("(unknown model)") : path;
}

/// Bounding box of a vertex set, padded so flat meshes keep some thickness.
Aabb computeBounds(const std::vector<Vertex>& vertices) {
    Aabb box{vertices.front().position, vertices.front().position};
    for (const Vertex& v : vertices) {
        box.min = glm::min(box.min, v.position);
        box.max = glm::max(box.max, v.position);
    }

    const glm::vec3 padding(0.01f);
    box.min -= padding;
    box.max += padding;
    return box;
}

/// Appends one quad as two triangles.
///
/// The corners must be given counter-clockwise as seen from outside the
/// surface. All four vertices share @p normal: a cube needs its own
/// vertices per face, otherwise the normals would be averaged across the
/// edges and the faces would look rounded instead of flat.
void addFace(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
             const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d,
             const glm::vec3& normal, const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());

    // The image is mapped upright onto the face: a is the bottom-left corner
    // as seen from outside, c the top-right one. V grows downwards.
    vertices.push_back({a, normal, color, {0.0f, 1.0f}});
    vertices.push_back({b, normal, color, {1.0f, 1.0f}});
    vertices.push_back({c, normal, color, {1.0f, 0.0f}});
    vertices.push_back({d, normal, color, {0.0f, 0.0f}});

    const uint32_t quad[6] = {0, 1, 2, 2, 3, 0};
    for (uint32_t i : quad)
        indices.push_back(base + i);
}

/// @return `true` if the future holds a result (or an exception) already.
template <typename T>
bool isReady(const std::future<T>& future) {
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

} // namespace

// ---------------------------------------------------------------------------
// Built-in primitives
// ---------------------------------------------------------------------------

bool MeshLibrary::init(const VulkanContext& ctx, uint32_t fieldCells) {
    m_ctx        = &ctx;
    m_fieldCells = std::clamp(fieldCells, 8u, 128u);
    if (!m_atlas.init(ctx, m_fieldCells)) return false;

    // Unit cube centred on the origin: 6 faces x 4 vertices.
    {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;

        const float     h = 0.5f;
        const glm::vec3 color{0.85f};

        addFace(vertices, indices, {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}, { 0, 0, 1}, color); // +Z
        addFace(vertices, indices, { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}, { 0, 0,-1}, color); // -Z
        addFace(vertices, indices, { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}, { 1, 0, 0}, color); // +X
        addFace(vertices, indices, {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}, {-1, 0, 0}, color); // -X
        addFace(vertices, indices, {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}, { 0, 1, 0}, color); // +Y
        addFace(vertices, indices, {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}, { 0,-1, 0}, color); // -Y

        if (!m_cube.upload(ctx, vertices, indices)) return false;
        m_cubeBounds = computeBounds(vertices);
        buildDistanceFieldNow(builtin::MeshCube, vertices, indices);
    }

    // Unit plane in XZ facing +Y (visible from above only: back faces are culled).
    {
        const glm::vec3 up{0.0f, 1.0f, 0.0f};
        const glm::vec3 grey{0.35f};

        const std::vector<Vertex> vertices = {
            // Seen from above with +X to the right, -Z is the top of the image.
            {{-0.5f, 0.0f, -0.5f}, up, grey, {0.0f, 0.0f}},
            {{ 0.5f, 0.0f, -0.5f}, up, grey, {1.0f, 0.0f}},
            {{ 0.5f, 0.0f,  0.5f}, up, grey, {1.0f, 1.0f}},
            {{-0.5f, 0.0f,  0.5f}, up, grey, {0.0f, 1.0f}},
        };
        const std::vector<uint32_t> indices = {0, 3, 2, 0, 2, 1};

        if (!m_plane.upload(ctx, vertices, indices)) return false;
        m_planeBounds = computeBounds(vertices);
        buildDistanceFieldNow(builtin::MeshPlane, vertices, indices);
    }

    return true;
}

void MeshLibrary::shutdown() {
    // Ask every running import to stop first, so that waiting for them below
    // does not mean waiting for a 400 MB file to finish. Destroying a future
    // that comes from std::async waits for its task, so no background import
    // outlives the library.
    for (auto& entry : m_pending)
        if (entry.second.control) entry.second.control->cancelRequested = true;
    for (auto& entry : m_abandoned)
        if (entry.control) entry.control->cancelRequested = true;

    // Give them a moment to notice. Not every importer checks the request
    // often (or at all), and a model read for a thumbnail can be big, so the
    // wait has a limit: the editor must not hang on exit for one file.
    const auto allStopped = [this]() {
        for (const auto& entry : m_pending)
            if (!isReady(entry.second.future)) return false;
        for (const auto& entry : m_abandoned)
            if (!isReady(entry.future)) return false;
        return true;
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (!allStopped() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!allStopped()) {
        FE_WARN("MeshLibrary: an import did not stop in time; leaving it running");

        // Destroying a future that comes from std::async waits for its task,
        // which is exactly what must not happen now. The unfinished ones are
        // moved to a list that is never destroyed. The tasks only use their
        // own data, so they can safely outlive the library.
        static auto* parked = new std::vector<std::future<std::optional<MeshData>>>();
        for (auto& entry : m_pending)
            if (!isReady(entry.second.future)) parked->push_back(std::move(entry.second.future));
        for (auto& entry : m_abandoned)
            if (!isReady(entry.future)) parked->push_back(std::move(entry.future));
        m_importsStillRunning = true;
    }

    m_pending.clear();
    m_abandoned.clear();

    // The distance fields stop at their next triangle, so this is quick.
    for (auto& entry : m_sdfPending) entry.second.cancel->store(true);
    m_sdfPending.clear();
    m_fields.clear();
    m_atlas.shutdown();

    m_cube.destroy();
    m_plane.destroy();

    for (auto& entry : m_imported)
        entry.second.buffer.destroy();
    m_imported.clear();

    m_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Imported models
// ---------------------------------------------------------------------------

AssetHandle MeshLibrary::registerModel(const std::filesystem::path& file) {
    const AssetHandle handle = AssetDatabase::instance().idFor(file);
    if (handle == 0) {
        FE_WARN("MeshLibrary: '%s' is not a model inside the asset folder", toUtf8(file).c_str());
        return builtin::MeshNone;
    }
    return handle;
}

void MeshLibrary::beginFrame() {
    m_used.clear();

    // Release the results of cancelled imports whose thread has now stopped.
    // Discarding the value is what frees its memory.
    std::erase_if(m_abandoned, [](AbandonedImport& job) {
        if (!isReady(job.future)) return false;
        job.future.get();
        return true;
    });

    collectDistanceFields();
}

bool MeshLibrary::ensureLoaded(AssetHandle handle, bool track) {
    // Built-in handles (and "none") are small numbers and always ready.
    if ((handle & kImportedBit) == 0) return true;

    if (track) m_used.insert(handle);

    if (m_imported.count(handle)) return true;
    if (m_failed.count(handle))   return false;

    // ---- An import is already running: has it finished? ----
    const auto pending = m_pending.find(handle);
    if (pending != m_pending.end()) {
        if (!isReady(pending->second.future))
            return false;   // still working; the entity is simply not drawn yet

        const auto started = pending->second.started;
        std::optional<MeshData> data = pending->second.future.get();
        m_pending.erase(pending);

        if (!data) {
            m_failed.insert(handle);   // the importer already logged the reason
            return false;
        }

        // Only the upload runs here: try_emplace builds the entry in place,
        // because MeshBuffer cannot be copied.
        auto [entry, inserted] = m_imported.try_emplace(handle);
        if (!entry->second.buffer.upload(*m_ctx, data->vertices, data->indices)) {
            m_imported.erase(entry);
            m_failed.insert(handle);
            return false;
        }
        entry->second.bounds = computeBounds(data->vertices);
        buildDistanceFieldLater(handle, data->vertices, data->indices);

        // A model without material information is one range, no texture.
        entry->second.parts.parts = std::move(data->parts);
        if (entry->second.parts.parts.empty())
            entry->second.parts.parts.push_back(
                {0, static_cast<uint32_t>(data->indices.size()), -1});
        entry->second.parts.materials = std::move(data->materials);
        entry->second.parts.images    = std::move(data->images);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        FE_INFO("Imported '%s': %zu vertices, %zu triangles, in %lld ms",
                nameOf(handle).c_str(),
                data->vertices.size(), data->indices.size() / 3,
                static_cast<long long>(elapsed.count()));
        return true;
    }

    // ---- Cancelled by the user: waits for confirmImport() ----
    if (m_declined.count(handle)) return false;

    // ---- Not started yet: find the file and start reading it ----
    // The database knows where the model is NOW, even if it was moved or
    // renamed after the scene was saved.
    const std::filesystem::path file = AssetDatabase::instance().fullPathOf(handle);
    if (file.empty()) {
        FE_WARN("MeshLibrary: no model has id %llx (was the file deleted?)",
                static_cast<unsigned long long>(handle));
        m_failed.insert(handle);
        return false;
    }

    std::error_code sizeEc;
    const std::uintmax_t bytes = std::filesystem::file_size(file, sizeEc);

    FE_INFO("Importing '%s' (%.1f MB)...", nameOf(handle).c_str(),
            sizeEc ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0));

    PendingImport job;
    job.started = std::chrono::steady_clock::now();
    job.control = std::make_shared<ImportControl>();
    job.bytes   = sizeEc ? 0 : bytes;

    // The task only touches its own local data, the shared ImportControl and
    // the (thread-safe) log, so it needs no access to the library. It must
    // also never let an exception escape: on a background thread that would
    // take the program down.
    job.future = std::async(std::launch::async,
        [file, control = job.control]() -> std::optional<MeshData> {
            try {
                return importModel(file, control.get());
            } catch (const std::exception& e) {
                FE_ERROR("Importing '%s' failed: %s", toUtf8(file).c_str(), e.what());
            } catch (...) {
                FE_ERROR("Importing '%s' failed with an unknown error", toUtf8(file).c_str());
            }
            return std::nullopt;
        });

    m_pending.emplace(handle, std::move(job));
    return false;
}

std::vector<ImportStatus> MeshLibrary::importStatuses() const {
    std::vector<ImportStatus> list;

    for (AssetHandle handle : m_used) {
        if (m_imported.count(handle)) continue;

        ImportStatus status;
        status.handle = handle;

        const std::filesystem::path file = AssetDatabase::instance().fullPathOf(handle);
        status.name = file.empty() ? std::string("(unknown model)") : toUtf8(file.filename());

        if (const auto pending = m_pending.find(handle); pending != m_pending.end()) {
            status.state    = ImportState::Loading;
            status.progress = pending->second.control->progress.load(std::memory_order_relaxed);
            status.bytes    = pending->second.bytes;
        } else if (m_declined.count(handle)) {
            status.state = ImportState::Declined;
        } else if (m_failed.count(handle)) {
            status.state = ImportState::Failed;
        } else {
            continue;   // about to start; nothing to show yet
        }

        list.push_back(std::move(status));
    }

    std::sort(list.begin(), list.end(),
              [](const ImportStatus& a, const ImportStatus& b) {
                  return a.name != b.name ? a.name < b.name : a.handle < b.handle;
              });
    return list;
}

void MeshLibrary::confirmImport(AssetHandle handle) {
    m_declined.erase(handle);   // ensureLoaded() starts it on the next frame
}

void MeshLibrary::cancelImport(AssetHandle handle) {
    if (const auto pending = m_pending.find(handle); pending != m_pending.end()) {
        pending->second.control->cancelRequested = true;

        // The editor does not wait for the thread: Assimp may need a while to
        // reach a point where it can stop. The future is parked here and its
        // result is thrown away by beginFrame() once it is ready.
        m_abandoned.push_back({std::move(pending->second.future), pending->second.control});
        m_pending.erase(pending);
        FE_INFO("Import of '%s' cancelled", nameOf(handle).c_str());
    }

    m_declined.insert(handle);
}

// ---------------------------------------------------------------------------
// Distance fields
// ---------------------------------------------------------------------------

/// @return The average of the vertex colours (white for a mesh without vertices).
static glm::vec3 averageVertexColour(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) return glm::vec3(1.0f);
    glm::dvec3 sum(0.0);
    for (const Vertex& v : vertices) sum += glm::dvec3(v.color);
    return glm::vec3(sum / static_cast<double>(vertices.size()));
}

void MeshLibrary::buildDistanceFieldNow(AssetHandle handle, const std::vector<Vertex>& vertices,
                                        const std::vector<uint32_t>& indices) {
    std::vector<glm::vec3> positions;
    positions.reserve(vertices.size());
    for (const Vertex& v : vertices) positions.push_back(v.position);

    auto [entry, inserted] = m_fields.try_emplace(handle);
    entry->second.volume = buildMeshSdf(positions, indices, SdfSettings{m_fieldCells});
    entry->second.colour = averageVertexColour(vertices);
    if (entry->second.volume.valid()) entry->second.atlasZ = m_atlas.add(entry->second.volume);
    if (entry->second.atlasZ == SdfAtlas::kNoSlot) m_fields.erase(entry);
}

void MeshLibrary::buildDistanceFieldLater(AssetHandle handle, const std::vector<Vertex>& vertices,
                                          const std::vector<uint32_t>& indices) {
    if (m_fields.count(handle) || m_sdfPending.count(handle)) return;

    // The task works on its own copy: the mesh data goes away once it is uploaded.
    std::vector<glm::vec3> positions;
    positions.reserve(vertices.size());
    for (const Vertex& v : vertices) positions.push_back(v.position);

    PendingField job;
    job.colour = averageVertexColour(vertices);
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    job.future = std::async(std::launch::async,
        [positions = std::move(positions), indices, cancel = job.cancel,
         cells = m_fieldCells]() -> SdfVolume {
            try {
                return buildMeshSdf(positions, indices, SdfSettings{cells}, cancel.get());
            } catch (const std::exception& e) {
                FE_ERROR("Building a distance field failed: %s", e.what());
            } catch (...) {
                FE_ERROR("Building a distance field failed with an unknown error");
            }
            return SdfVolume{};
        });
    m_sdfPending.emplace(handle, std::move(job));
}

void MeshLibrary::collectDistanceFields() {
    for (auto it = m_sdfPending.begin(); it != m_sdfPending.end();) {
        if (!isReady(it->second.future)) { ++it; continue; }

        const AssetHandle handle = it->first;
        SdfVolume volume = it->second.future.get();
        const glm::vec3 colour = it->second.colour;
        it = m_sdfPending.erase(it);

        if (!volume.valid()) {
            FE_WARN("MeshLibrary: no distance field for '%s'", nameOf(handle).c_str());
            continue;
        }

        auto [entry, inserted] = m_fields.try_emplace(handle);
        entry->second.volume = std::move(volume);
        entry->second.colour = colour;
        entry->second.atlasZ = m_atlas.add(entry->second.volume);
        if (entry->second.atlasZ == SdfAtlas::kNoSlot) {
            m_fields.erase(entry);
            continue;
        }

        FE_INFO("Distance field of '%s': %ux%ux%u cells",
                nameOf(handle).c_str(), entry->second.volume.dims.x,
                entry->second.volume.dims.y, entry->second.volume.dims.z);
    }
}

std::optional<MeshLibrary::SdfPlacement> MeshLibrary::distancePlacement(AssetHandle handle) const {
    const auto it = m_fields.find(handle);
    if (it == m_fields.end() || it->second.atlasZ == SdfAtlas::kNoSlot) return std::nullopt;

    const SdfVolume& volume = it->second.volume;
    return SdfPlacement{volume.boxMin, volume.boxMax, volume.dims, volume.voxelSize, it->second.atlasZ, it->second.colour};
}

const SdfVolume* MeshLibrary::distanceVolume(AssetHandle handle) const {
    const auto it = m_fields.find(handle);
    return it != m_fields.end() ? &it->second.volume : nullptr;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const MeshBuffer* MeshLibrary::get(AssetHandle handle) const {
    switch (handle) {
        case builtin::MeshCube:  return m_cube.isValid()  ? &m_cube  : nullptr;
        case builtin::MeshPlane: return m_plane.isValid() ? &m_plane : nullptr;
        case builtin::MeshNone:  return nullptr;
        default: {
            const auto it = m_imported.find(handle);
            return (it != m_imported.end() && it->second.buffer.isValid())
                       ? &it->second.buffer : nullptr;
        }
    }
}

const ModelParts* MeshLibrary::parts(AssetHandle handle) const {
    const auto it = m_imported.find(handle);
    return it != m_imported.end() ? &it->second.parts : nullptr;
}

std::optional<Aabb> MeshLibrary::bounds(AssetHandle handle) const {
    switch (handle) {
        case builtin::MeshCube:  return m_cube.isValid()  ? std::optional<Aabb>(m_cubeBounds)  : std::nullopt;
        case builtin::MeshPlane: return m_plane.isValid() ? std::optional<Aabb>(m_planeBounds) : std::nullopt;
        case builtin::MeshNone:  return std::nullopt;
        default: {
            const auto it = m_imported.find(handle);
            return it != m_imported.end() ? std::optional<Aabb>(it->second.bounds)
                                          : std::nullopt;
        }
    }
}

} // namespace fe
