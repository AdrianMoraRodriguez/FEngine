#pragma once

/**
 * @file GltfImporter.h
 * @brief Reads glTF 2.0 files (.gltf and .glb) into CPU mesh data.
 *
 * The importer is deliberately free of any GPU code: it returns plain
 * vertex and index arrays, and the caller decides how to upload them.
 * Most code should call importModel() instead, which picks the importer
 * that matches the file's extension.
 */

#include "import/MeshData.h"

#include <filesystem>
#include <optional>

namespace fe {

/**
 * @brief Imports every mesh of a glTF file as a single merged mesh.
 *
 * What is imported:
 *   - Triangle primitives of every mesh in the default scene. The node
 *     hierarchy is flattened: each node's transform is baked into the
 *     vertices, so the result is one mesh in the model's own space.
 *   - Positions and normals. Missing normals are generated.
 *   - Colour: the material's base colour factor, multiplied by the vertex
 *     colours (COLOR_0) if the primitive has them.
 *
 * What is not (yet) imported: textures, animations, skins and morph targets,
 * and compressed geometry (Draco, meshopt), which is reported as an error.
 *
 * @param file Path to a .gltf or .glb file.
 * @return The mesh, or an empty optional if the file cannot be read, is not
 *         valid, or contains no triangles. The reason is logged.
 */
std::optional<MeshData> importGltf(const std::filesystem::path& file);

} // namespace fe
