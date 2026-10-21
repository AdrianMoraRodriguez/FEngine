#pragma once

/**
 * @file AssimpImporter.h
 * @brief Reads FBX, OBJ, DAE, 3DS, STL and PLY files through Assimp.
 *
 * glTF does not go through here: it has its own importer, which follows the
 * format more closely. Most code should call importModel(), which picks the
 * right importer for a file.
 */

#include "import/ImportControl.h"
#include "import/MeshData.h"

#include <filesystem>
#include <optional>

namespace fe {

/**
 * @brief Imports every mesh of a file as a single merged mesh.
 *
 * Assimp does the format-specific work; this function only applies the same
 * rules as the glTF importer so both give the same kind of result:
 *   - The node hierarchy is flattened, with each node's transform baked
 *     into the vertices.
 *   - Faces are triangulated and missing normals are generated (smooth).
 *   - Points and lines are dropped.
 *   - Colour: the material's diffuse colour, multiplied by the vertex
 *     colours if the mesh has them.
 *
 * What is not (yet) imported: textures, animations and skins.
 *
 * Assimp's own data validation is not requested: on files with millions of
 * vertices it costs more than the rest of the import together, and it only
 * protects against damaged files.
 *
 * @note Units are taken as they come. FBX files exported in centimetres
 *       will appear 100 times larger than a glTF of the same object; use the
 *       entity's Scale (0.01) until models get a per-import scale.
 *
 * @param file    Path to a file Assimp can read.
 * @param control Optional. When given, receives the progress of the import
 *                and can cancel it. Cancelling is checked at Assimp's own
 *                checkpoints and, in our code, every few thousand vertices;
 *                a stage that Assimp runs without checkpoints finishes
 *                first. A cancelled import returns an empty optional and
 *                does not log an error.
 * @return The mesh, or an empty optional if the file cannot be read, has
 *         no triangles, or the import was cancelled. Failures are logged.
 */
std::optional<MeshData> importWithAssimp(const std::filesystem::path& file,
                                         ImportControl* control = nullptr);

} // namespace fe
