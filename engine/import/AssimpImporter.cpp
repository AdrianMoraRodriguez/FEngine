#include "import/AssimpImporter.h"

#include "core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace fe {
namespace {

/// Colour used for meshes without a usable material.
const glm::vec3 kDefaultColor{0.8f};

/// Share of the progress bar given to Assimp's own work (reading and
/// post-processing). The conversion to engine data fills the remainder.
constexpr float kAssimpShare = 0.85f;

/// While converting, the cancel flag is looked at every this many vertices.
constexpr unsigned kCancelCheckInterval = 1u << 16;

/// Forwards Assimp's progress to an ImportControl and lets it cancel.
class ProgressBridge final : public Assimp::ProgressHandler {
public:
    explicit ProgressBridge(ImportControl& control) : m_control(control) {}

    /// Called by Assimp. Returning `false` asks it to abort.
    bool Update(float percentage) override {
        // Assimp passes a negative value when it has no percentage to report.
        if (percentage >= 0.0f)
            m_control.progress.store(std::min(percentage, 1.0f) * kAssimpShare,
                                     std::memory_order_relaxed);
        return !m_control.cancelRequested.load(std::memory_order_relaxed);
    }

private:
    ImportControl& m_control;
};

using Clock = std::chrono::steady_clock;

long long millisecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

} // namespace

std::optional<MeshData> importWithAssimp(const std::filesystem::path& file,
                                         ImportControl* control) {
    // Assimp expects UTF-8 paths, which it converts itself on Windows.
    const std::u8string u8 = file.u8string();
    const std::string   path(reinterpret_cast<const char*>(u8.c_str()), u8.size());

    const auto isCancelled = [control]() {
        return control && control->cancelRequested.load(std::memory_order_relaxed);
    };

    const Clock::time_point start = Clock::now();

    Assimp::Importer importer;

    // The mesh pipeline only draws triangles, so points and lines are
    // removed while importing instead of being skipped by hand afterwards.
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    // The importer takes ownership of the handler and deletes it itself.
    if (control)
        importer.SetProgressHandler(new ProgressBridge(*control));

    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate            // quads and polygons become triangles
      | aiProcess_JoinIdenticalVertices  // identical vertices are shared, so indices are useful
      | aiProcess_GenSmoothNormals       // only generates normals the file does not have
      | aiProcess_SortByPType            // needed by the point/line removal above
      | aiProcess_PreTransformVertices); // bakes the node transforms: one flat mesh

    const long long readMs = millisecondsSince(start);

    if (isCancelled()) {
        FE_INFO("AssimpImporter: import of '%s' cancelled", path.c_str());
        return std::nullopt;
    }

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0) {
        FE_ERROR("AssimpImporter: cannot import '%s': %s", path.c_str(),
                 importer.GetErrorString());
        return std::nullopt;
    }

    // Count first, so the arrays are allocated once. Growing a vector that
    // ends up with millions of entries copies it again and again.
    size_t totalVertices = 0;
    size_t totalIndices  = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        totalVertices += scene->mMeshes[m]->mNumVertices;
        totalIndices  += static_cast<size_t>(scene->mMeshes[m]->mNumFaces) * 3;
    }

    // Indices are 32-bit.
    if (totalVertices > std::numeric_limits<uint32_t>::max()) {
        FE_ERROR("AssimpImporter: '%s' has %zu vertices, more than the 32-bit "
                 "indices of a mesh can address", path.c_str(), totalVertices);
        return std::nullopt;
    }

    MeshData result;
    result.vertices.reserve(totalVertices);
    result.indices.reserve(totalIndices);

    size_t convertedVertices = 0;

    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasPositions() || mesh->mNumFaces == 0) continue;

        // Base colour from the material's diffuse colour.
        glm::vec3 baseColor = kDefaultColor;
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            aiColor3D diffuse(1.0f, 1.0f, 1.0f);
            if (scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse)
                    == AI_SUCCESS)
                baseColor = glm::vec3(diffuse.r, diffuse.g, diffuse.b);
        }

        // A black diffuse colour usually means "the colour comes from a
        // texture", which we do not load yet. Black would make the model
        // invisible against the dark background, so fall back to the default.
        if (glm::dot(baseColor, baseColor) < 1e-6f)
            baseColor = kDefaultColor;

        const uint32_t baseVertex = static_cast<uint32_t>(result.vertices.size());

        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            if ((v % kCancelCheckInterval) == 0) {
                if (isCancelled()) {
                    FE_INFO("AssimpImporter: import of '%s' cancelled", path.c_str());
                    return std::nullopt;
                }
                if (control && totalVertices > 0) {
                    const float done = static_cast<float>(convertedVertices + v) /
                                       static_cast<float>(totalVertices);
                    control->progress.store(kAssimpShare + (1.0f - kAssimpShare) * done,
                                            std::memory_order_relaxed);
                }
            }

            Vertex vertex;

            const aiVector3D& p = mesh->mVertices[v];
            vertex.position = glm::vec3(p.x, p.y, p.z);

            if (mesh->HasNormals()) {
                const aiVector3D& n = mesh->mNormals[v];
                vertex.normal = glm::vec3(n.x, n.y, n.z);
            } else {
                vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            vertex.color = baseColor;
            if (mesh->HasVertexColors(0)) {
                const aiColor4D& c = mesh->mColors[0][v];
                vertex.color *= glm::vec3(c.r, c.g, c.b);
            }

            // Assimp follows the OpenGL convention (V up from the bottom of
            // the image, for every format it reads); we use the image-file
            // convention (V down from the top), hence the flip.
            if (mesh->HasTextureCoords(0)) {
                const aiVector3D& t = mesh->mTextureCoords[0][v];
                vertex.uv = glm::vec2(t.x, 1.0f - t.y);
            }

            result.vertices.push_back(vertex);
        }
        convertedVertices += mesh->mNumVertices;

        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;   // already triangulated; defensive

            for (unsigned i = 0; i < 3; ++i)
                result.indices.push_back(baseVertex + face.mIndices[i]);
        }
    }

    if (result.vertices.empty() || result.indices.empty()) {
        FE_ERROR("AssimpImporter: '%s' contains no triangle meshes", path.c_str());
        return std::nullopt;
    }

    if (control)
        control->progress.store(1.0f, std::memory_order_relaxed);

    // Timings per stage tell, when an import is slow, whether Assimp or our
    // own conversion is to blame.
    FE_INFO("AssimpImporter: '%s': Assimp %lld ms, conversion %lld ms",
            path.c_str(), readMs, millisecondsSince(start) - readMs);

    return result;
}

} // namespace fe
