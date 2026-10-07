#pragma once

/**
 * @file SceneSerializer.h
 * @brief Saves and loads scenes to and from JSON files.
 *
 * Scene files use the extension `.fescene` and are plain JSON, making them
 * human-readable and diff-friendly for version control.
 *
 * Format overview:
 * @code
 * {
 *   "version": 1,
 *   "name": "My Scene",
 *   "entities": [
 *     {
 *       "uuid":   12345678,
 *       "parent": 0,
 *       "components": {
 *         "Name":      { "name": "Root" },
 *         "Transform": { "position": [0,0,0], ... },
 *         "Camera":    { "fovDegrees": 60, ... }
 *       }
 *     },
 *     ...
 *   ]
 * }
 * @endcode
 *
 * Entities are written in depth-first order so that parents always precede
 * their children. During load, each entity's parent UUID is resolved against
 * the already-created entities, which works because of this ordering.
 */

#include "scene/Scene.h"

#include <filesystem>

namespace fe {

/**
 * @brief Serialises and deserialises Scene objects to disk.
 *
 * Uses ComponentRegistry to discover which components each entity has,
 * so adding a new component type requires no changes here.
 */
class SceneSerializer {
public:
    /**
     * @brief Writes the scene to a JSON file.
     *
     * @param scene The scene to serialise.
     * @param path  Destination file path (created or overwritten).
     * @return `true` on success. Logs the reason on failure.
     */
    static bool saveToFile(const Scene& scene,
                           const std::filesystem::path& path);

    /**
     * @brief Populates a scene from a JSON file.
     *
     * The scene is not cleared before loading — call this on a freshly
     * constructed Scene to get a clean result.
     *
     * @param scene The scene to populate.
     * @param path  Source file path.
     * @return `true` on success. Logs the reason on failure.
     */
    static bool loadFromFile(Scene& scene,
                             const std::filesystem::path& path);
};

} // namespace fe
