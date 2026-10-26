#include "import/AssimpImporter.h"

#include "core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

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

/// State shared by all the meshes of one file while importing.
struct ImportContext {
    std::filesystem::path             directory;       ///< Folder of the model file.
    std::unordered_map<std::string, int> imageIndex;   ///< Texture name -> index in MeshData::images (-1: unusable).
    std::unordered_map<unsigned, int>    materialIndex;///< Assimp material -> index in MeshData::materials.
};

/// Registers the first texture of the given types that the material has.
/// @return The index in MeshData::images, or -1 if there is none.
int imageForTypes(const aiScene& scene, const aiMaterial& material,
                  std::initializer_list<aiTextureType> types,
                  ImportContext& context, MeshData& out) {
    aiString name;
    bool found = false;
    for (aiTextureType type : types) {
        if (material.GetTexture(type, 0, &name) == AI_SUCCESS && name.length > 0) { found = true; break; }
    }
    if (!found) return -1;

    const std::string key = name.C_Str();
    if (const auto known = context.imageIndex.find(key); known != context.imageIndex.end())
        return known->second;

    ImageSource source;
    source.label = key;

    if (const aiTexture* embedded = scene.GetEmbeddedTexture(key.c_str())) {
        // Inside the model file. Only compressed images (PNG, JPEG...) are
        // supported; they are stored with the byte count in mWidth.
        if (embedded->mHeight == 0 && embedded->pcData && embedded->mWidth > 0) {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(embedded->pcData);
            source.encoded = std::make_shared<const std::vector<uint8_t>>(
                bytes, bytes + embedded->mWidth);
        } else {
            FE_WARN("AssimpImporter: embedded raw texture '%s' is not supported", key.c_str());
        }
    } else {
        // A file next to the model. Windows tools write backslashes.
        std::string text = key;
        std::replace(text.begin(), text.end(), '\\', '/');
        std::filesystem::path file = context.directory / std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(text.data()), text.size()));

        // Models often point at a folder that no longer exists; the same
        // file name next to the model is the usual fix.
        std::error_code ec;
        if (!std::filesystem::exists(file, ec))
            file = context.directory / file.filename();
        source.file = file;
    }

    int index = -1;
    if (source.encoded || !source.file.empty()) {
        index = static_cast<int>(out.images.size());
        out.images.push_back(std::move(source));
    }
    context.imageIndex[key] = index;
    return index;
}

/// Registers a material once and returns its index in MeshData::materials.
int materialFor(const aiScene& scene, unsigned materialIndex,
                ImportContext& context, MeshData& out) {
    if (materialIndex >= scene.mNumMaterials) return -1;
    if (const auto known = context.materialIndex.find(materialIndex); known != context.materialIndex.end())
        return known->second;

    const aiMaterial& source = *scene.mMaterials[materialIndex];
    ModelMaterial material;

    // glTF-style "base colour" first, then the classic "diffuse" slot.
    material.albedo   = imageForTypes(scene, source, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, context, out);
    material.normal   = imageForTypes(scene, source, {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA}, context, out);
    material.emissive = imageForTypes(scene, source, {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR}, context, out);

    // Formats that know metalness and roughness say so; the old ones only have
    // a "shininess" exponent, which converts to a roughness.
    float value = 0.0f;
    if (source.Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS)
        material.metallic = value;
    if (source.Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == AI_SUCCESS) {
        material.roughness = value;
    } else if (source.Get(AI_MATKEY_SHININESS, value) == AI_SUCCESS) {
        material.roughness = std::clamp(std::sqrt(2.0f / (std::max(value, 0.0f) + 2.0f)), 0.1f, 1.0f);
    }

    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (source.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
        material.emissiveFactor = glm::vec3(emissive.r, emissive.g, emissive.b);

    // Transparency: glTF files say it outright; the older formats only have an
    // opacity number (below 1, the surface is see-through).
    aiString gltfMode;
    if (source.Get(AI_MATKEY_GLTF_ALPHAMODE, gltfMode) == AI_SUCCESS) {
        const std::string mode = gltfMode.C_Str();
        if (mode == "MASK")       material.alphaMode = AlphaMode::Mask;
        else if (mode == "BLEND") material.alphaMode = AlphaMode::Blend;
        float cutoff = 0.5f;
        if (source.Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS)
            material.alphaCutoff = cutoff;
    }
    float opacity = 1.0f;
    if (source.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
        material.opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (material.alphaMode == AlphaMode::Opaque) material.alphaMode = AlphaMode::Blend;
    }
    int twoSided = 0;
    if (source.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
        material.doubleSided = twoSided != 0;

    const int index = static_cast<int>(out.materials.size());
    out.materials.push_back(material);
    context.materialIndex[materialIndex] = index;
    return index;
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

    ImportContext context;
    context.directory = file.parent_path();

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

        const int material = materialFor(*scene, mesh->mMaterialIndex, context, result);
        const bool textured = material >= 0 && result.materials[material].albedo >= 0;

        // A black diffuse colour usually means "the colour comes from a
        // texture". Black would make the model invisible, so fall back to
        // white under a texture (it is multiplied with it) and to the default
        // grey otherwise.
        if (glm::dot(baseColor, baseColor) < 1e-6f)
            baseColor = textured ? glm::vec3(1.0f) : kDefaultColor;

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

        const uint32_t firstIndex = static_cast<uint32_t>(result.indices.size());

        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;   // already triangulated; defensive

            for (unsigned i = 0; i < 3; ++i)
                result.indices.push_back(baseVertex + face.mIndices[i]);
        }

        result.addPart(firstIndex, static_cast<uint32_t>(result.indices.size()) - firstIndex, material);
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
