#pragma once

/**
 * @file ProjectSettings.h
 * @brief Settings that belong to the project (and the machine it runs on), not to a scene.
 */

#include <cstdint>
#include <filesystem>

namespace fe {

/**
 * @brief Quality and performance settings shared by every scene of a project.
 *
 * What a scene looks like (its sky, exposure, which effects are on) lives in
 * SceneSettings and is saved with the scene. What it *costs* to look that way
 * (how finely the distance fields are sampled, how many steps a ray may take)
 * does not change from scene to scene: it is a decision about the project and
 * the hardware it targets, so it lives here, in one file next to the assets.
 *
 * The file is read at start-up and written whenever the Project Settings
 * window changes something. A missing file, or a key missing from it, leaves
 * the default.
 */
struct ProjectSettings {
    /// Cells per side of each level of the scene's distance field
    /// (0: 128, 1: 192, 2: 256). More cells make every cell smaller, for more
    /// video memory (about 38, 127 and 302 MB) and a slower build.
    int distanceFieldResolution = 0;

    /// How finely the scene's distance field follows what is near the camera
    /// (0 low, 1 medium, 2 high). Higher means smaller cells, but the finest
    /// levels then cover less space and are rebuilt more often.
    int distanceFieldDetail = 1;

    /// Cells along the longest side of each mesh's own distance field
    /// (0: 32, 1: 64, 2: 96, 3: 128). Read when the editor starts: the fields
    /// are built when a mesh loads and live in an atlas sized for this value.
    int meshFieldResolution = 1;

    /// Steps a reflection ray may take (0: 32, 1: 64, 2: 128).
    int distanceFieldReflectionQuality = 1;

    /// How far the reflections look: 0 only through the finest level of the
    /// field (sharp, near), 1 also through the next one (blurrier, farther).
    int distanceFieldReflectionReach = 0;

    /// How far above a surface a shadow ray starts, in cells of the field (0.25 .. 4).
    float distanceFieldShadowBias = 1.0f;

    /// Whether what a distance field reflection shows is shadowed too.
    bool distanceFieldReflectedShadows = true;

    /// Number of choices of meshFieldResolution.
    static constexpr int kMeshFieldChoices = 4;

    /// @return Cells along the longest side of a mesh's field, from meshFieldResolution.
    uint32_t meshFieldCells() const;

    /// @return `true` if the two hold the same values.
    bool operator==(const ProjectSettings&) const = default;

    /**
     * @brief Reads the settings from a JSON file.
     * @return `true` if the file was read. If it does not exist or is broken,
     *         the settings keep their current values and `false` is returned.
     */
    bool load(const std::filesystem::path& file);

    /// Writes the settings to a JSON file. @return `true` on success.
    bool save(const std::filesystem::path& file) const;
};

} // namespace fe
