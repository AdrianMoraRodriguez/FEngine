#pragma once

/**
 * @file ModelImporter.h
 * @brief Single entry point for importing any supported 3D model file.
 *
 * Picks the importer that matches the file's extension:
 *   - .glb, .gltf                        : glTF importer (cgltf)
 *   - .fbx, .obj, .dae, .3ds, .stl, .ply : Assimp
 *
 * Supporting a new format means adding an importer and one line to the
 * dispatch in ModelImporter.cpp. Nothing else needs to know.
 */

#include "import/ImportControl.h"
#include "import/MeshData.h"

#include <filesystem>
#include <optional>

namespace fe {

/**
 * @brief Tells whether a file has an extension the engine can import.
 *
 * Only looks at the extension, case-insensitively; it does not open the file.
 * This is the single list of supported formats: the asset scan and the
 * Asset Browser both use it.
 */
bool isSupportedModel(const std::filesystem::path& file);

/**
 * @brief Imports a model file as a single merged mesh.
 *
 * @param file    Path to a file for which isSupportedModel() is `true`.
 * @param control Optional progress report and cancellation. Only the Assimp
 *                formats use it for now; the glTF importer is fast enough
 *                not to need it and ignores it.
 * @return The mesh, or an empty optional if the format is unsupported, the
 *         import failed, or it was cancelled. Failures are logged.
 */
std::optional<MeshData> importModel(const std::filesystem::path& file,
                                    ImportControl* control = nullptr);

} // namespace fe
