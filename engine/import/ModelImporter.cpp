#include "import/ModelImporter.h"

#include "import/AssimpImporter.h"
#include "import/GltfImporter.h"
#include "core/Log.h"
#include "core/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fe {
namespace {

enum class Format { Unsupported, Gltf, Assimp };

Format formatOf(const std::filesystem::path& file) {
    std::string ext = toUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".glb" || ext == ".gltf")
        return Format::Gltf;

    if (ext == ".fbx" || ext == ".obj" || ext == ".dae" ||
        ext == ".3ds" || ext == ".stl" || ext == ".ply")
        return Format::Assimp;

    return Format::Unsupported;
}

} // namespace

bool isSupportedModel(const std::filesystem::path& file) {
    return formatOf(file) != Format::Unsupported;
}

std::optional<MeshData> importModel(const std::filesystem::path& file,
                                    ImportControl* control) {
    switch (formatOf(file)) {
        case Format::Gltf:   return importGltf(file);
        case Format::Assimp: return importWithAssimp(file, control);
        case Format::Unsupported: break;
    }

    FE_ERROR("ModelImporter: '%s' is not a supported model format",
             toUtf8(file).c_str());
    return std::nullopt;
}

} // namespace fe
