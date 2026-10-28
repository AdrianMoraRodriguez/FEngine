#pragma once

/**
 * @file MeshDistanceField.h
 * @brief Signed distance fields of meshes: the base of the software ray tracing.
 *
 * A signed distance field (SDF) stores, for every point of a grid around a
 * mesh, how far the nearest surface is: positive outside, negative inside,
 * zero on the surface. With it a ray can be followed in big safe steps (it
 * can always advance by the distance to the nearest surface without hitting
 * anything), which is how shadows and reflections can be traced on any GPU,
 * without ray tracing hardware. Unreal's software Lumen works the same way.
 *
 * buildMeshSdf() turns a triangle mesh into an SdfVolume on the CPU. It
 * touches no GPU, so it can run on a background thread. SdfAtlas puts the
 * volumes of all meshes into one 3D texture for the GPU.
 *
 * All distances are in the mesh's own (object) space, so scaling an entity
 * scales its field with it.
 */

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace fe {

/// How a mesh is turned into a distance field.
struct SdfSettings {
    /// Cells along the longest side of the mesh's box (the other sides get
    /// proportionally fewer). More cells follow thin features better and cost
    /// memory: the volume grows with the cube of this value.
    uint32_t maxResolution = 64;
};

/**
 * @brief The distance field of one mesh, on the CPU.
 *
 * Cell (x, y, z) holds the distance from its centre to the nearest surface.
 * The cell centres are at boxMin + (cell + 0.5) * voxelSize.
 */
struct SdfVolume {
    glm::uvec3 dims{0};           ///< Cells along x, y and z.
    glm::vec3  boxMin{0.0f};      ///< Corner of the volume, in the mesh's space.
    glm::vec3  boxMax{0.0f};      ///< Opposite corner.
    float      voxelSize = 0.0f;  ///< Side of a cell, in the mesh's space.

    /// dims.x * dims.y * dims.z distances, x running fastest.
    std::vector<float> distances;

    /// @return `true` if the volume holds a field.
    bool valid() const { return !distances.empty(); }

    /// @return Index of a cell inside #distances.
    size_t index(uint32_t x, uint32_t y, uint32_t z) const {
        return (static_cast<size_t>(z) * dims.y + y) * dims.x + x;
    }

    /// @return Distance at a point of the mesh's space (trilinear; clamped to the volume).
    float sample(const glm::vec3& p) const;
};

/**
 * @brief Builds the distance field of a triangle mesh.
 *
 * Distances are exact near the surface and approximate far from it (they
 * are spread outwards from the surface cell by cell). The sign comes from
 * counting how many times a line crosses the surface, so it is right for
 * closed meshes; for a surface that is not closed (a plane, a leaf) every
 * cell is outside and the field is simply the distance to it.
 *
 * Meant to run on a background thread: it uses no GPU and no shared state.
 *
 * @param positions Vertex positions.
 * @param indices   Three per triangle.
 * @param settings  Resolution.
 * @param cancel    Optional. Set to `true` from another thread to stop early;
 *                  the result is then empty.
 * @return The field, or an empty volume for a mesh without triangles or when cancelled.
 */
SdfVolume buildMeshSdf(const std::vector<glm::vec3>& positions,
                       const std::vector<uint32_t>&  indices,
                       const SdfSettings&            settings = {},
                       const std::atomic<bool>*      cancel   = nullptr);

/// @return @p value as an IEEE half float (the format the GPU copies of the fields use).
uint16_t sdfFloatToHalf(float value);

} // namespace fe
